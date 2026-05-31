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

"""Parse SimX debug logs into normalized instruction events."""

from __future__ import annotations

import argparse
import json
import re
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Iterator, TextIO


CONFIG_PATTERN = re.compile(
    r"CONFIGS: num_threads=(\d+), num_warps=(\d+), num_cores=(\d+), "
    r"num_clusters=(\d+), socket_size=(\d+), local_mem_base=0x([0-9a-fA-F]+), "
    r"num_barriers=(\d+)"
)
PIPELINE_PATTERN = re.compile(
    r"TRACE\s+(\d+): pipeline-(schedule|ibuffer|dispatch|commit):.*#(\d+)"
)
OPCODE_PATTERN = re.compile(r"Instr: ([^,]+)")
PC_PATTERN = re.compile(r"PC=(0x[0-9a-fA-F]+)")
CORE_ID_PATTERN = re.compile(r"cid=(\d+)")
WARP_ID_PATTERN = re.compile(r"wid=(\d+)")
TMASK_PATTERN = re.compile(r"tmask=([01]+|\d+)")
UUID_PATTERN = re.compile(r"#(\d+)")
OPERANDS_PATTERN = re.compile(r"Src\d+ Reg: (.+)")
DESTINATION_PATTERN = re.compile(r"Dest Reg: (.+)")


@dataclass
class SimxConfig:
    num_threads: int
    num_warps: int
    num_cores: int
    num_clusters: int
    socket_size: int
    local_mem_base: int
    num_barriers: int


@dataclass
class InstructionEvent:
    uuid: int
    core_id: int
    warp_id: int
    pc: str
    opcode: str
    tmask: str
    cycle_schedule: int | None = None
    cycle_ibuffer: int | None = None
    cycle_dispatch: int | None = None
    cycle_commit: int | None = None
    operands: str = ""
    destination: str = ""


@dataclass
class ParsedTrace:
    config: SimxConfig
    events: list[InstructionEvent] = field(default_factory=list)


def load_config(lines: Iterator[str]) -> SimxConfig:
    for line in lines:
        match = CONFIG_PATTERN.search(line)
        if match:
            return SimxConfig(
                num_threads=int(match.group(1)),
                num_warps=int(match.group(2)),
                num_cores=int(match.group(3)),
                num_clusters=int(match.group(4)),
                socket_size=int(match.group(5)),
                local_mem_base=int(match.group(6), 16),
                num_barriers=int(match.group(7)),
            )
    raise ValueError("missing CONFIGS: header in log")


def _instr_dict_to_event(
    data: dict,
    schd_ticks: dict[int, int],
    ibuf_ticks: dict[int, int],
    disp_ticks: dict[int, int],
    commit_ticks: dict[int, int],
) -> InstructionEvent:
    uuid = data["uuid"]
    return InstructionEvent(
        uuid=uuid,
        core_id=data["core_id"],
        warp_id=data["warp_id"],
        pc=data["PC"],
        opcode=data["opcode"].strip(),
        tmask=data["tmask"],
        cycle_schedule=schd_ticks.get(uuid),
        cycle_ibuffer=ibuf_ticks.get(uuid),
        cycle_dispatch=disp_ticks.get(uuid),
        cycle_commit=commit_ticks.get(uuid),
        operands=data.get("operands", ""),
        destination=data.get("destination", ""),
    )


def parse_simx_lines(lines: Iterator[str]) -> ParsedTrace:
    """Parse SimX log lines (reference: ci/trace_csv.py parse_simx)."""
    line_iter = iter(lines)
    config = load_config(line_iter)

    entries: list[dict] = []
    instr_data: dict | None = None
    schd_ticks: dict[int, int] = {}
    ibuf_ticks: dict[int, int] = {}
    disp_ticks: dict[int, int] = {}
    commit_ticks: dict[int, int] = {}

    for line in line_iter:
        try:
            if line.startswith("DEBUG Instr:"):
                if instr_data:
                    entries.append(instr_data)
                instr_data = {
                    "opcode": OPCODE_PATTERN.search(line).group(1),
                    "PC": PC_PATTERN.search(line).group(1),
                    "core_id": int(CORE_ID_PATTERN.search(line).group(1)),
                    "warp_id": int(WARP_ID_PATTERN.search(line).group(1)),
                    "tmask": TMASK_PATTERN.search(line).group(1),
                    "uuid": int(UUID_PATTERN.search(line).group(1)),
                }
            elif line.startswith("DEBUG Src") and instr_data:
                src_reg = OPERANDS_PATTERN.search(line).group(1)
                prev = instr_data.get("operands", "")
                instr_data["operands"] = f"{prev}, {src_reg}" if prev else src_reg
            elif line.startswith("DEBUG Dest") and instr_data:
                instr_data["destination"] = DESTINATION_PATTERN.search(line).group(1)
            elif line.startswith("TRACE"):
                match = PIPELINE_PATTERN.search(line)
                if not match:
                    continue
                timestamp = int(match.group(1))
                stage = match.group(2)
                uuid = int(match.group(3))
                if stage == "schedule":
                    schd_ticks[uuid] = timestamp
                elif stage == "ibuffer":
                    ibuf_ticks[uuid] = timestamp
                elif stage == "dispatch":
                    disp_ticks[uuid] = timestamp
                elif stage == "commit":
                    commit_ticks[uuid] = timestamp
        except (AttributeError, ValueError):
            instr_data = None

    if instr_data:
        entries.append(instr_data)

    events = [
        _instr_dict_to_event(e, schd_ticks, ibuf_ticks, disp_ticks, commit_ticks)
        for e in entries
    ]
    return ParsedTrace(config=config, events=events)


def parse_simx_log(log_path: Path) -> ParsedTrace:
    with log_path.open(encoding="utf-8", errors="replace") as f:
        return parse_simx_lines(f)


def write_events_json(parsed: ParsedTrace, out_path: Path) -> None:
    payload = {
        "config": asdict(parsed.config),
        "events": [asdict(e) for e in parsed.events],
    }
    out_path.write_text(json.dumps(payload, indent=2))


def main() -> None:
    parser = argparse.ArgumentParser(description="Parse SimX debug log to events JSON.")
    parser.add_argument("log", type=Path, help="SimX run.log")
    parser.add_argument("-o", "--output", type=Path, default=Path("events.json"))
    args = parser.parse_args()
    parsed = parse_simx_log(args.log)
    write_events_json(parsed, args.output)
    print(f"wrote {len(parsed.events)} events to {args.output}")


if __name__ == "__main__":
    main()
