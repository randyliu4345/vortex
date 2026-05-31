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

"""M7: Quality metrics for recovered dynamic CFGs."""

from __future__ import annotations

import argparse
import json
from dataclasses import dataclass
from pathlib import Path

from build_cfg import build_warp_paths, pc_to_int
from parse_simx import load_events_json


DEFAULT_THRESHOLDS = {
    "pc_coverage_min": 0.95,
    "edge_soundness_min": 0.95,
    "divergent_edge_min": 1,
}


@dataclass
class EvalReport:
    pc_coverage: float
    edge_soundness: float
    divergent_edges: int
    total_events: int
    total_cfg_edges: int
    passed: bool
    details: dict

    def to_dict(self) -> dict:
        return {
            "pc_coverage": self.pc_coverage,
            "edge_soundness": self.edge_soundness,
            "divergent_edges": self.divergent_edges,
            "total_events": self.total_events,
            "total_cfg_edges": self.total_cfg_edges,
            "passed": self.passed,
            "details": self.details,
        }


def _pc_in_block(pc: int, start: str, end: str) -> bool:
    return pc_to_int(start) <= pc <= pc_to_int(end)


def _blocks_for_warp(cfg: dict, warp_id: int) -> list[dict]:
    for warp in cfg.get("warps", []):
        if warp["warp_id"] == warp_id:
            return warp.get("blocks", [])
    return []


def _edges_for_warp(cfg: dict, warp_id: int) -> list[dict]:
    for warp in cfg.get("warps", []):
        if warp["warp_id"] == warp_id:
            return warp.get("edges", [])
    return []


def evaluate(events_path: Path, cfg_path: Path, warp_id: int = 0) -> EvalReport:
    parsed = load_events_json(events_path)
    cfg = json.loads(cfg_path.read_text())
    paths = build_warp_paths(parsed)

    warp_events = [e for e in parsed.events if e.warp_id == warp_id]
    blocks = _blocks_for_warp(cfg, warp_id)
    cfg_edges = _edges_for_warp(cfg, warp_id)

    covered = 0
    for event in warp_events:
        pc = pc_to_int(event.pc)
        if any(_pc_in_block(pc, b["start_pc"], b["end_pc"]) for b in blocks):
            covered += 1
    pc_coverage = covered / len(warp_events) if warp_events else 1.0

    path = next((p for p in paths if p.warp_id == warp_id), None)
    block_by_id = {b["block_id"]: b for b in blocks}
    sound = 0
    for edge in cfg_edges:
        src = block_by_id.get(edge["src_block"])
        dst = block_by_id.get(edge["dst_block"])
        if not src or not dst or not path:
            continue
        for pe in path.edges:
            if _pc_in_block(pc_to_int(pe.src_pc), src["start_pc"], src["end_pc"]) and _pc_in_block(
                pc_to_int(pe.dst_pc), dst["start_pc"], dst["end_pc"]
            ):
                sound += 1
                break

    total_cfg_edges = len(cfg_edges)
    edge_soundness = sound / total_cfg_edges if total_cfg_edges else 1.0
    divergent_edges = sum(1 for e in cfg_edges if e.get("divergent"))

    passed = (
        pc_coverage >= DEFAULT_THRESHOLDS["pc_coverage_min"]
        and edge_soundness >= DEFAULT_THRESHOLDS["edge_soundness_min"]
        and divergent_edges >= DEFAULT_THRESHOLDS["divergent_edge_min"]
    )

    return EvalReport(
        pc_coverage=pc_coverage,
        edge_soundness=edge_soundness,
        divergent_edges=divergent_edges,
        total_events=len(warp_events),
        total_cfg_edges=total_cfg_edges,
        passed=passed,
        details={"warp_id": warp_id, "thresholds": DEFAULT_THRESHOLDS},
    )


def main() -> None:
    parser = argparse.ArgumentParser(description="Evaluate cfg.json quality (M7).")
    parser.add_argument("events", type=Path, help="events.json")
    parser.add_argument("cfg", type=Path, help="cfg.json")
    parser.add_argument("--warp", type=int, default=0)
    parser.add_argument("-o", "--output", type=Path, default=None)
    args = parser.parse_args()

    report = evaluate(args.events, args.cfg, args.warp)
    text = json.dumps(report.to_dict(), indent=2)
    if args.output:
        args.output.write_text(text)
    print(text)
    raise SystemExit(0 if report.passed else 1)


if __name__ == "__main__":
    main()
