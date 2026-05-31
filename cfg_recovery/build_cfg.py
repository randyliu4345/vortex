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
    symbol: str | None = None
    is_merge_point: bool = False
    is_divergence_point: bool = False


@dataclass
class WarpCfg:
    warp_id: int
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


def build_basic_blocks(warp_paths: list[WarpPath]) -> dict[int, list[CfgBlock]]:
    """TODO(M4): Segment paths into dynamic basic blocks."""
    raise NotImplementedError("build_basic_blocks is a stub — implement in M4")


def build_dynamic_cfg(
    parsed: ParsedTrace,
    pc_map_path: Path | None = None,
) -> DynamicCfg:
    """TODO(M5): Full CFG with divergence / reconvergence annotations."""
    raise NotImplementedError("build_dynamic_cfg is a stub — implement in M5")


def write_cfg(cfg: DynamicCfg, out_path: Path) -> None:
    out_path.write_text(json.dumps(asdict(cfg), indent=2))


def main() -> None:
    parser = argparse.ArgumentParser(description="Build warp paths or dynamic CFG from trace.")
    parser.add_argument("input", type=Path, help="events.json or run.log")
    parser.add_argument("-o", "--output", type=Path, default=Path("warp_paths.json"))
    parser.add_argument(
        "--paths-only",
        action="store_true",
        default=True,
        help="Emit warp_paths.json (M3, default)",
    )
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
