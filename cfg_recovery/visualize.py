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


def cfg_to_dot(cfg: dict[str, Any], warp_id: int | None = None) -> str:
    """Render M5 cfg.json with basic blocks and annotated edges."""
    lines = ["digraph cfg {", '  rankdir="LR";', "  node [shape=box, fontsize=10];"]
    for warp in cfg.get("warps", []):
        wid = warp["warp_id"]
        cid = warp.get("core_id", 0)
        if warp_id is not None and wid != warp_id:
            continue
        subgraph = f"cluster_c{cid}_w{wid}"
        lines.append(f"  subgraph {subgraph} {{")
        lines.append(f'    label="core {cid} warp {wid}";')
        for block in warp.get("blocks", []):
            node_id = _dot_escape(block["block_id"])
            start = block["start_pc"]
            end = block["end_pc"]
            symbol = block.get("symbol") or ""
            label = f"{node_id}\\n{start}"
            if end != start:
                label += f" – {end}"
            if symbol:
                label += f"\\n{symbol}"
            attrs = ""
            if block.get("is_divergence_point"):
                attrs = ', color="red"'
            elif block.get("is_merge_point"):
                attrs = ', color="blue"'
            lines.append(f'    "{node_id}" [label="{_dot_escape(label)}"]{attrs};')
        for edge in warp.get("edges", []):
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
    lines.append("}")
    return "\n".join(lines) + "\n"


def json_to_dot(data: dict[str, Any], warp_id: int | None = None) -> str:
    if "paths" in data:
        return paths_to_dot(data, warp_id=warp_id)
    return cfg_to_dot(data, warp_id=warp_id)


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
    parser.add_argument("--format", default="png", choices=("png", "svg", "pdf"))
    args = parser.parse_args()
    data = json.loads(args.input.read_text())
    dot = json_to_dot(data, warp_id=args.warp)
    render_dot(dot, args.output, args.format)
    print(f"rendered {args.output}")


if __name__ == "__main__":
    main()
