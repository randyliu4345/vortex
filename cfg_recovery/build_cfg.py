#!/usr/bin/env python3
# Copyright © 2019-2023
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Build dynamic CFGs from parsed instruction events."""

from __future__ import annotations

import argparse
import json
import re
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Any, Literal

from parse_simx import (
    InstructionEvent,
    ParsedTrace,
    load_events_json,
    parse_simx_log,
)
from pc_map import SymbolEntry, load_pc_map_json, lookup_pc

BRANCH_OPCODE = re.compile(
    r"\b(BR|JAL|JALR|BEQ|BNE|BLT|BGE|BLTU|BGEU|CBEQZ|CBNEZ|C\.J|C\.JR|C\.JAL|C\.BEQZ|C\.BNEZ)\b",
    re.IGNORECASE,
)


def pc_to_int(pc: str) -> int:
    return int(pc, 16)


def is_fallthrough(src_pc: str, dst_pc: str) -> bool:
    return pc_to_int(dst_pc) == pc_to_int(src_pc) + 4


def is_branch_opcode(opcode: str) -> bool:
    return BRANCH_OPCODE.search(opcode) is not None


@dataclass
class WarpPathEdge:
    src_pc: str
    dst_pc: str
    src_uuid: int
    dst_uuid: int
    warp_id: int
    core_id: int
    kind: Literal["fallthrough", "branch"]
    tmask_before: str
    tmask_after: str
    opcode: str
    cycle_commit: int | None = None
    weight: int = 1
    tmask_changed: bool = False


@dataclass
class WarpPath:
    warp_id: int
    core_id: int
    edges: list[WarpPathEdge] = field(default_factory=list)


@dataclass
class CfgEdge:
    src_block: str
    dst_block: str
    weight: int = 1
    divergent: bool = False
    tmask_before: str | None = None
    tmask_after: str | None = None


@dataclass
class CfgBlock:
    block_id: str
    warp_id: int
    start_pc: str
    end_pc: str
    first_uuid: int = 0
    last_uuid: int = 0
    symbol: str | None = None
    is_merge_point: bool = False
    is_divergence_point: bool = False


@dataclass
class WarpCfg:
    warp_id: int
    core_id: int = 0
    blocks: list[CfgBlock] = field(default_factory=list)
    edges: list[CfgEdge] = field(default_factory=list)


@dataclass
class DynamicCfg:
    warps: list[WarpCfg] = field(default_factory=list)
    metadata: dict[str, Any] = field(default_factory=dict)


def _event_sort_key(event: InstructionEvent) -> tuple[int, int]:
    cycle = event.cycle_commit if event.cycle_commit is not None else -1
    return (cycle, event.uuid)


def _group_events_by_warp(events: list[InstructionEvent]) -> dict[tuple[int, int], list[InstructionEvent]]:
    groups: dict[tuple[int, int], list[InstructionEvent]] = {}
    for event in events:
        key = (event.core_id, event.warp_id)
        groups.setdefault(key, []).append(event)
    for key in groups:
        groups[key].sort(key=_event_sort_key)
    return groups


def _merge_edge(bucket: dict[tuple, WarpPathEdge], edge: WarpPathEdge) -> None:
    key = (edge.core_id, edge.warp_id, edge.src_pc, edge.dst_pc)
    existing = bucket.get(key)
    if existing:
        existing.weight += 1
        return
    bucket[key] = edge


def build_warp_paths(parsed: ParsedTrace) -> list[WarpPath]:
    """Build per-warp control-flow edges from consecutive committed instructions."""
    bucket: dict[tuple, WarpPathEdge] = {}
    for (core_id, warp_id), events in _group_events_by_warp(parsed.events).items():
        for prev, curr in zip(events, events[1:]):
            fallthrough = is_fallthrough(prev.pc, curr.pc)
            kind: Literal["fallthrough", "branch"] = "fallthrough" if fallthrough else "branch"
            edge = WarpPathEdge(
                src_pc=prev.pc,
                dst_pc=curr.pc,
                src_uuid=prev.uuid,
                dst_uuid=curr.uuid,
                warp_id=warp_id,
                core_id=core_id,
                kind=kind,
                tmask_before=prev.tmask,
                tmask_after=curr.tmask,
                opcode=prev.opcode,
                cycle_commit=curr.cycle_commit,
                tmask_changed=prev.tmask != curr.tmask,
            )
            _merge_edge(bucket, edge)

    paths_by_warp: dict[tuple[int, int], list[WarpPathEdge]] = {}
    for edge in bucket.values():
        key = (edge.core_id, edge.warp_id)
        paths_by_warp.setdefault(key, []).append(edge)

    result: list[WarpPath] = []
    for (core_id, warp_id), edges in sorted(paths_by_warp.items()):
        edges.sort(key=lambda e: (e.cycle_commit or 0, e.dst_uuid))
        result.append(WarpPath(warp_id=warp_id, core_id=core_id, edges=edges))
    return result


def write_warp_paths_json(paths: list[WarpPath], out_path: Path, metadata: dict[str, Any] | None = None) -> None:
    payload = {
        "metadata": metadata or {},
        "paths": [asdict(path) for path in paths],
    }
    out_path.write_text(json.dumps(payload, indent=2))


def load_parsed(input_path: Path) -> ParsedTrace:
    if input_path.suffix == ".json":
        return load_events_json(input_path)
    return parse_simx_log(input_path)


def _block_id(core_id: int, warp_id: int, first_uuid: int) -> str:
    return f"c{core_id}w{warp_id}_#{first_uuid}"


def build_basic_blocks_for_warp(
    events: list[InstructionEvent],
    warp_id: int,
    core_id: int,
    symbols: list[SymbolEntry] | None = None,
) -> tuple[list[CfgBlock], dict[int, CfgBlock]]:
    """Maximal fallthrough sequences from ordered warp events."""
    if not events:
        return [], {}

    blocks: list[CfgBlock] = []
    uuid_block: dict[int, CfgBlock] = {}
    index = 0
    while index < len(events):
        first = events[index]
        start_pc = first.pc
        end_pc = start_pc
        last = first
        cursor = index
        while cursor + 1 < len(events) and is_fallthrough(events[cursor].pc, events[cursor + 1].pc):
            cursor += 1
            last = events[cursor]
            end_pc = last.pc
        symbol = None
        if symbols:
            hit = lookup_pc(pc_to_int(start_pc), symbols)
            symbol = hit.symbol
        block = CfgBlock(
            block_id=_block_id(core_id, warp_id, first.uuid),
            warp_id=warp_id,
            start_pc=start_pc,
            end_pc=end_pc,
            first_uuid=first.uuid,
            last_uuid=last.uuid,
            symbol=symbol,
        )
        for i in range(index, cursor + 1):
            uuid_block[events[i].uuid] = block
        blocks.append(block)
        index = cursor + 1
    return blocks, uuid_block


def build_basic_blocks(
    parsed: ParsedTrace,
    symbols: list[SymbolEntry] | None = None,
) -> tuple[dict[tuple[int, int], list[CfgBlock]], dict[tuple[int, int], dict[int, CfgBlock]]]:
    blocks_map: dict[tuple[int, int], list[CfgBlock]] = {}
    uuid_maps: dict[tuple[int, int], dict[int, CfgBlock]] = {}
    for (core_id, warp_id), events in _group_events_by_warp(parsed.events).items():
        blocks, uuid_block = build_basic_blocks_for_warp(events, warp_id, core_id, symbols)
        blocks_map[(core_id, warp_id)] = blocks
        uuid_maps[(core_id, warp_id)] = uuid_block
    return blocks_map, uuid_maps


def _annotate_block_points(
    blocks: list[CfgBlock],
    path_edges: list[WarpPathEdge],
    uuid_block: dict[int, CfgBlock],
) -> None:
    outgoing: dict[str, set[str]] = {}
    incoming: dict[str, set[str]] = {}

    for edge in path_edges:
        src_block = uuid_block.get(edge.src_uuid)
        dst_block = uuid_block.get(edge.dst_uuid)
        if not src_block or not dst_block:
            continue
        outgoing.setdefault(src_block.block_id, set()).add(dst_block.block_id)
        incoming.setdefault(dst_block.block_id, set()).add(src_block.block_id)

    for block in blocks:
        outs = outgoing.get(block.block_id, set())
        ins = incoming.get(block.block_id, set())
        if len(outs) > 1:
            block.is_divergence_point = True
        if len(ins) > 1:
            block.is_merge_point = True
        exit_edges = [e for e in path_edges if e.src_uuid == block.last_uuid]
        if any(e.kind == "branch" or is_branch_opcode(e.opcode) for e in exit_edges):
            block.is_divergence_point = True


def build_warp_cfg(
    path: WarpPath,
    blocks: list[CfgBlock],
    uuid_block: dict[int, CfgBlock],
) -> WarpCfg:
    _annotate_block_points(blocks, path.edges, uuid_block)
    edge_bucket: dict[tuple[str, str], CfgEdge] = {}

    for path_edge in path.edges:
        src_block = uuid_block.get(path_edge.src_uuid)
        dst_block = uuid_block.get(path_edge.dst_uuid)
        if not src_block or not dst_block:
            continue
        key = (src_block.block_id, dst_block.block_id)
        divergent = (
            path_edge.kind == "branch"
            or path_edge.tmask_changed
            or is_branch_opcode(path_edge.opcode)
        )
        existing = edge_bucket.get(key)
        if existing:
            existing.weight += path_edge.weight
            existing.divergent = existing.divergent or divergent
            continue
        edge_bucket[key] = CfgEdge(
            src_block=src_block.block_id,
            dst_block=dst_block.block_id,
            weight=path_edge.weight,
            divergent=divergent,
            tmask_before=path_edge.tmask_before,
            tmask_after=path_edge.tmask_after,
        )

    return WarpCfg(
        warp_id=path.warp_id,
        core_id=path.core_id,
        blocks=blocks,
        edges=list(edge_bucket.values()),
    )


def build_dynamic_cfg(
    parsed: ParsedTrace,
    pc_map_path: Path | None = None,
) -> DynamicCfg:
    symbols: list[SymbolEntry] | None = None
    if pc_map_path:
        symbols = load_pc_map_json(pc_map_path)

    paths = build_warp_paths(parsed)
    block_map, uuid_maps = build_basic_blocks(parsed, symbols)
    path_by_key = {(p.core_id, p.warp_id): p for p in paths}

    warps: list[WarpCfg] = []
    for key, blocks in sorted(block_map.items()):
        if not blocks:
            continue
        path = path_by_key.get(key)
        if path:
            warps.append(build_warp_cfg(path, blocks, uuid_maps[key]))
        else:
            warps.append(WarpCfg(warp_id=key[1], core_id=key[0], blocks=blocks, edges=[]))

    return DynamicCfg(
        warps=warps,
        metadata={
            "event_count": len(parsed.events),
            "warp_count": len(warps),
        },
    )


def write_cfg(cfg: DynamicCfg, out_path: Path) -> None:
    out_path.write_text(json.dumps(asdict(cfg), indent=2))


def main() -> None:
    parser = argparse.ArgumentParser(description="Build warp paths or dynamic CFG from trace.")
    parser.add_argument("input", type=Path, help="events.json or run.log")
    parser.add_argument("-o", "--output", type=Path, default=Path("warp_paths.json"))
    parser.add_argument("--paths-only", action="store_true", help="Emit warp_paths.json (M3)")
    parser.add_argument("--cfg", action="store_true", help="Emit full cfg.json (M5)")
    parser.add_argument("--pc-map", type=Path, default=None, help="Optional pc_map.json (M5)")
    args = parser.parse_args()

    parsed = load_parsed(args.input)
    if args.cfg:
        cfg = build_dynamic_cfg(parsed, args.pc_map)
        write_cfg(cfg, args.output)
        print(f"wrote CFG with {len(cfg.warps)} warps to {args.output}")
        return

    paths = build_warp_paths(parsed)
    write_warp_paths_json(
        paths,
        args.output,
        metadata={"event_count": len(parsed.events), "warp_count": len(paths)},
    )
    edge_count = sum(len(p.edges) for p in paths)
    print(f"wrote {len(paths)} warp paths ({edge_count} edges) to {args.output}")


if __name__ == "__main__":
    main()
