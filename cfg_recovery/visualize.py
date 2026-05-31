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

"""Export dynamic CFG JSON to Graphviz."""

from __future__ import annotations

import argparse
import json
import subprocess
from pathlib import Path
from typing import Any


def cfg_to_dot(cfg: dict[str, Any], warp_id: int | None = None) -> str:
    """TODO(M5): Emit DOT for one warp or all warps (subgraphs)."""
    raise NotImplementedError("cfg_to_dot is a stub — implement in M5")


def render_dot(dot: str, out_path: Path, fmt: str = "png") -> None:
    """Write DOT and invoke `dot` if available."""
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
    parser = argparse.ArgumentParser(description="Visualize cfg.json with Graphviz.")
    parser.add_argument("cfg", type=Path, help="cfg.json from build_cfg.py")
    parser.add_argument("-o", "--output", type=Path, default=Path("cfg.png"))
    parser.add_argument("--warp", type=int, default=None, help="Only render this warp")
    parser.add_argument("--format", default="png", choices=("png", "svg", "pdf"))
    args = parser.parse_args()
    cfg = json.loads(args.cfg.read_text())
    dot = cfg_to_dot(cfg, args.warp)
    render_dot(dot, args.output, args.format)
    print(f"rendered {args.output}")


if __name__ == "__main__":
    main()
