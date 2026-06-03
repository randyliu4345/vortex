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

"""Export warp paths or dynamic CFG JSON to Graphviz."""

from __future__ import annotations

import argparse
import json
import subprocess
from pathlib import Path
from typing import Any


def _dot_escape(text: str) -> str:
    return text.replace("\\", "\\\\").replace('"', '\\"')


def _edge_style(edge: dict[str, Any]) -> str:
    divergent = edge.get("divergent", False)
    kind = edge.get("kind", "")
    tmask_changed = edge.get("tmask_changed", False)
    if divergent or kind == "branch" or tmask_changed:
        return ' [color="red", style="dashed"]'
    return ""


def paths_to_dot(data: dict[str, Any], warp_id: int | None = None, core_id: int | None = None) -> str:
    """Render M3 warp_paths.json as a PC-level graph."""
    lines = ["digraph warp_paths {", '  rankdir="LR";', "  node [shape=box, fontsize=10];"]
    paths = data.get("paths", [])
    for path in paths:
        wid = path["warp_id"]
        cid = path["core_id"]
        if warp_id is not None and wid != warp_id:
            continue
        if core_id is not None and cid != core_id:
            continue
        subgraph = f"cluster_w{cid}_{wid}"
        label = f"core {cid} warp {wid}"
        lines.append(f"  subgraph {subgraph} {{")
        lines.append(f'    label="{_dot_escape(label)}";')
        for edge in path.get("edges", []):
            src = _dot_escape(edge["src_pc"])
            dst = _dot_escape(edge["dst_pc"])
            weight = edge.get("weight", 1)
            style = _edge_style(edge)
            label = f"×{weight}" if weight > 1 else ""
            if label:
                lines.append(f'    "{src}" -> "{dst}" [label="{label}"]{style};')
            else:
                lines.append(f'    "{src}" -> "{dst}"{style};')
        lines.append("  }")
    lines.append("}")
    return "\n".join(lines) + "\n"


def _resolve_symbol_name(block: dict[str, Any], symbols: list[Any] | None) -> str:
    stored = block.get("symbol") or ""
    if not symbols:
        return stored
    from pc_map import SymbolEntry, lookup_pc

    entries = symbols if isinstance(symbols[0], SymbolEntry) else [SymbolEntry(**s) for s in symbols]
    pc = int(block["start_pc"], 16)
    for tag in ("path_high", "path_mid", "path_low"):
        helper = next((s for s in entries if tag in s.name), None)
        if helper and helper.addr <= pc < helper.addr + max(helper.size, 1):
            return tag
    resolved = lookup_pc(pc, entries).symbol
    for tag in ("path_high", "path_mid", "path_low", "kernel_body"):
        if tag in resolved:
            return tag
    if resolved.startswith("$x."):
        return resolved
    return stored or resolved


def _block_dot_label(block: dict[str, Any], symbols: list[Any] | None = None) -> str:
    node_id = block["block_id"]
    start = block["start_pc"]
    end = block["end_pc"]
    symbol = _resolve_symbol_name(block, symbols)
    label = f"{node_id}\\n{start}"
    if end != start:
        label += f" – {end}"
    if symbol:
        label += f"\\n{symbol}"
    return label


def _block_node_attrs(block: dict[str, Any]) -> str:
    if block.get("is_divergence_point"):
        return ', color="red"'
    if block.get("is_merge_point"):
        return ', color="blue"'
    return ""


def _emit_cfg_subgraph(
    lines: list[str],
    warp: dict[str, Any],
    blocks: list[dict[str, Any]],
    edges: list[dict[str, Any]],
    symbols: list[Any] | None = None,
) -> None:
    cid = warp.get("core_id", 0)
    wid = warp["warp_id"]
    subgraph = f"cluster_c{cid}_w{wid}"
    lines.append(f"  subgraph {subgraph} {{")
    lines.append(f'    label="core {cid} warp {wid}";')
    for block in blocks:
        node_id = _dot_escape(block["block_id"])
        label = _block_dot_label(block, symbols)
        attrs = _block_node_attrs(block)
        lines.append(f'    "{node_id}" [label="{_dot_escape(label)}"{attrs}];')
    for edge in edges:
        src = _dot_escape(edge["src_block"])
        dst = _dot_escape(edge["dst_block"])
        style = _edge_style(edge)
        weight = edge.get("weight", 1)
        elabel = f"×{weight}" if weight > 1 else ""
        if elabel:
            lines.append(f'    "{src}" -> "{dst}" [label="{elabel}"]{style};')
        else:
            lines.append(f'    "{src}" -> "{dst}"{style};')
    lines.append("  }")


def cfg_to_dot(
    cfg: dict[str, Any],
    warp_id: int | None = None,
    symbols: list[Any] | None = None,
) -> str:
    """Render M5 cfg.json with basic blocks and annotated edges."""
    lines = ["digraph cfg {", '  rankdir="LR";', "  node [shape=box, fontsize=10];"]
    for warp in cfg.get("warps", []):
        wid = warp["warp_id"]
        if warp_id is not None and wid != warp_id:
            continue
        _emit_cfg_subgraph(lines, warp, warp.get("blocks", []), warp.get("edges", []), symbols)
    lines.append("}")
    return "\n".join(lines) + "\n"


def _kernel_pc_window(symbols: list[Any] | None) -> tuple[int, int] | None:
    if not symbols:
        return None
    from pc_map import SymbolEntry

    entries = symbols if isinstance(symbols[0], SymbolEntry) else [SymbolEntry(**s) for s in symbols]
    body = next((s for s in entries if "kernel_body" in s.name), None)
    if not body:
        return None
    end = body.addr + max(body.size, 0)
    for tag in ("path_low", "path_mid", "path_high"):
        helper = next((s for s in entries if tag in s.name), None)
        if helper:
            end = max(end, helper.addr + max(helper.size, 0))
    return body.addr, end


def _slide_edge_ok(edge: dict[str, Any], block_ids: set[str]) -> bool:
    if edge["src_block"] not in block_ids or edge["dst_block"] not in block_ids:
        return False
    if edge["src_block"] == edge["dst_block"]:
        return bool(edge.get("divergent"))
    return True


def _select_slide_blocks(
    blocks: dict[str, dict[str, Any]],
    edges: list[dict[str, Any]],
    *,
    max_depth: int,
    max_nodes: int,
) -> set[str]:
    """Keep branch points (incl. divergent back-edges), then BFS from kernel entry."""
    if not blocks:
        return set()
    adj: dict[str, set[str]] = {}
    for e in edges:
        adj.setdefault(e["src_block"], set()).add(e["dst_block"])

    keep: set[str] = set()
    branch_nodes = sorted(
        (n for n, outs in adj.items() if len(outs) >= 2),
        key=lambda n: blocks[n]["first_uuid"],
    )
    for node in branch_nodes:
        if len(keep) >= max_nodes:
            break
        keep.add(node)
        for dst in sorted(adj[node], key=lambda d: blocks[d]["first_uuid"]):
            if len(keep) < max_nodes:
                keep.add(dst)

    entry = min(blocks, key=lambda bid: blocks[bid]["first_uuid"])
    if entry not in keep and len(keep) < max_nodes:
        keep.add(entry)

    frontier = [n for n in [entry] if n in keep] or [entry]
    depth = 0
    while frontier and depth <= max_depth and len(keep) < max_nodes:
        next_frontier: list[str] = []
        for node in frontier:
            if node not in keep:
                if len(keep) >= max_nodes:
                    continue
                keep.add(node)
            if depth < max_depth:
                for nxt in adj.get(node, ()):
                    if nxt not in keep and nxt not in next_frontier:
                        next_frontier.append(nxt)
        frontier = next_frontier
        depth += 1

    return keep


def cfg_to_slide_dot(
    cfg: dict[str, Any],
    *,
    warp_id: int = 0,
    max_depth: int = 8,
    max_nodes: int = 18,
    pc_window: tuple[int, int] | None = None,
    symbols: list[Any] | None = None,
) -> str:
    """Smaller CFG for slides: same dot style as cfg_to_dot, kernel PC window only."""
    warp = next(w for w in cfg["warps"] if w["warp_id"] == warp_id)
    window = pc_window or _kernel_pc_window(symbols)

    def in_window(block: dict[str, Any]) -> bool:
        if window is None:
            return "kernel_body" in (block.get("symbol") or "")
        pc = int(block["start_pc"], 16)
        return window[0] <= pc < window[1]

    blocks = {b["block_id"]: b for b in warp["blocks"] if in_window(b)}
    if not blocks:
        return cfg_to_dot(cfg, warp_id=warp_id, symbols=symbols)

    block_ids = set(blocks)
    edges = [e for e in warp["edges"] if _slide_edge_ok(e, block_ids)]
    keep = _select_slide_blocks(blocks, edges, max_depth=max_depth, max_nodes=max_nodes)
    blocks = {k: v for k, v in blocks.items() if k in keep}

    edge_seen: set[tuple[str, str]] = set()
    slide_edges: list[dict[str, Any]] = []
    for e in edges:
        if e["src_block"] not in keep or e["dst_block"] not in keep:
            continue
        key = (e["src_block"], e["dst_block"])
        if key in edge_seen:
            continue
        edge_seen.add(key)
        slide_edges.append(e)

    ordered_blocks = sorted(blocks.values(), key=lambda b: b["first_uuid"])
    lines = ["digraph cfg {", '  rankdir="LR";', "  node [shape=box, fontsize=10];"]
    _emit_cfg_subgraph(lines, warp, ordered_blocks, slide_edges, symbols)
    lines.append("}")
    return "\n".join(lines) + "\n"


def json_to_dot(
    data: dict[str, Any],
    warp_id: int | None = None,
    slide: bool = False,
    pc_map_path: Path | None = None,
    slide_max_depth: int = 8,
    slide_max_nodes: int = 18,
) -> str:
    symbols = None
    if pc_map_path and pc_map_path.is_file():
        from pc_map import load_pc_map_json

        symbols = load_pc_map_json(pc_map_path)
    if slide and "warps" in data:
        return cfg_to_slide_dot(
            data,
            warp_id=warp_id or 0,
            symbols=symbols,
            max_depth=slide_max_depth,
            max_nodes=slide_max_nodes,
        )
    if "paths" in data:
        return paths_to_dot(data, warp_id=warp_id)
    return cfg_to_dot(data, warp_id=warp_id, symbols=symbols)


def render_dot(dot: str, out_path: Path, fmt: str = "png") -> None:
    dot_path = out_path.with_suffix(".dot")
    dot_path.write_text(dot)
    try:
        subprocess.run(
            ["dot", f"-T{fmt}", str(dot_path), "-o", str(out_path)],
            check=True,
            capture_output=True,
        )
    except FileNotFoundError:
        print(f"graphviz not installed; wrote {dot_path} only")


def main() -> None:
    parser = argparse.ArgumentParser(description="Visualize warp_paths.json or cfg.json.")
    parser.add_argument("input", type=Path, help="warp_paths.json or cfg.json")
    parser.add_argument("-o", "--output", type=Path, default=Path("graph.png"))
    parser.add_argument("--warp", type=int, default=None, help="Only render this warp")
    parser.add_argument(
        "--slide",
        action="store_true",
        help="Kernel-slice CFG (same dot style, fewer blocks via --slide-max-*)",
    )
    parser.add_argument("--slide-max-depth", type=int, default=8)
    parser.add_argument("--slide-max-nodes", type=int, default=18)
    parser.add_argument(
        "--pc-map",
        type=Path,
        default=None,
        help="pc_map.json for symbol resolution on node labels",
    )
    parser.add_argument("--format", default="png", choices=("png", "svg", "pdf"))
    args = parser.parse_args()
    data = json.loads(args.input.read_text())
    dot = json_to_dot(
        data,
        warp_id=args.warp,
        slide=args.slide,
        pc_map_path=args.pc_map,
        slide_max_depth=args.slide_max_depth,
        slide_max_nodes=args.slide_max_nodes,
    )
    render_dot(dot, args.output, args.format)
    print(f"rendered {args.output}")


if __name__ == "__main__":
    main()
