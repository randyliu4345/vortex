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
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Any

from parse_simx import ParsedTrace, parse_simx_log


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


def build_warp_paths(parsed: ParsedTrace) -> dict[int, list[Any]]:
    """TODO(M3): Per-warp ordered edge list from events."""
    raise NotImplementedError("build_warp_paths is a stub — implement in M3")


def build_basic_blocks(warp_paths: dict[int, list[Any]]) -> dict[int, list[CfgBlock]]:
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
    parser = argparse.ArgumentParser(description="Build dynamic CFG from events JSON or log.")
    parser.add_argument("input", type=Path, help="events.json or run.log")
    parser.add_argument("-o", "--output", type=Path, default=Path("cfg.json"))
    parser.add_argument("--pc-map", type=Path, default=None, help="Optional pc_map.json")
    args = parser.parse_args()

    if args.input.suffix == ".json":
        raise NotImplementedError("events.json input — wire after M1")
    parsed = parse_simx_log(args.input)
    cfg = build_dynamic_cfg(parsed, args.pc_map)
    write_cfg(cfg, args.output)
    print(f"wrote CFG with {len(cfg.warps)} warps to {args.output}")


if __name__ == "__main__":
    main()
