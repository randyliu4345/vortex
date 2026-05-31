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

"""Build PC → symbol maps from Vortex kernel ELF binaries."""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
from dataclasses import asdict, dataclass
from pathlib import Path


SYMBOL_LINE = re.compile(
    r"^\s*([0-9a-fA-F]+)\s+[lg]\s+"
    r"(?:F\s+\.)?\s*\S+\s+([0-9a-fA-F]+)\s+(\S+)"
)
DISASM_LABEL = re.compile(r"^([0-9a-fA-F]+)\s+<([^>]+)>:")


@dataclass
class SymbolEntry:
    addr: int
    name: str
    size: int


@dataclass
class PcSymbol:
    pc: str
    symbol: str
    offset: int


@dataclass
class PcMap:
    elf_path: str
    objdump: str
    symbols: list[SymbolEntry]


def _tooldir_from_build_config() -> str | None:
    """Read TOOLDIR from ./config.mk when run from Vortex build/."""
    config = Path("config.mk")
    if not config.is_file():
        return None
    for line in config.read_text().splitlines():
        if line.startswith("TOOLDIR"):
            _, _, value = line.partition("?=" if "?=" in line else "=")
            return value.strip()
    return None


def resolve_objdump(explicit: str | None = None) -> str:
    if explicit:
        return explicit
    candidates = []
    llvm_vortex = os.environ.get("LLVM_VORTEX")
    tooldir = os.environ.get("TOOLDIR") or _tooldir_from_build_config()
    if not llvm_vortex and tooldir:
        llvm_vortex = str(Path(tooldir) / "llvm-vortex")
    if llvm_vortex:
        candidates.append(Path(llvm_vortex) / "bin" / "llvm-objdump")
    riscv_path = os.environ.get("RISCV_TOOLCHAIN_PATH")
    prefix = os.environ.get("RISCV_PREFIX", "riscv64-unknown-elf-")
    if riscv_path:
        candidates.append(Path(riscv_path) / "bin" / f"{prefix}objdump")
    candidates.extend(
        [
            Path("llvm-objdump"),
            Path("riscv64-unknown-elf-objdump"),
            Path("riscv32-unknown-elf-objdump"),
        ]
    )
    for path in candidates:
        p = str(path)
        if os.path.isfile(p) and os.access(p, os.X_OK):
            return p
    raise FileNotFoundError(
        "no objdump found; set LLVM_VORTEX or RISCV_TOOLCHAIN_PATH, or pass --objdump"
    )


def _parse_objdump_symbols(text: str) -> list[SymbolEntry]:
    symbols: list[SymbolEntry] = []
    for line in text.splitlines():
        match = SYMBOL_LINE.match(line)
        if match:
            addr = int(match.group(1), 16)
            size = int(match.group(2), 16)
            name = match.group(3)
            symbols.append(SymbolEntry(addr=addr, name=name, size=size))
    if symbols:
        return sorted(symbols, key=lambda s: s.addr)

    # llvm-objdump -d label form
    for line in text.splitlines():
        match = DISASM_LABEL.match(line)
        if match:
            symbols.append(
                SymbolEntry(addr=int(match.group(1), 16), name=match.group(2), size=0)
            )
    return sorted(symbols, key=lambda s: s.addr)


def build_pc_map(elf_path: Path, objdump: str | None = None) -> PcMap:
    tool = resolve_objdump(objdump)
    sym_result = subprocess.run(
        [tool, "-t", str(elf_path)],
        check=True,
        capture_output=True,
        text=True,
    )
    symbols = _parse_objdump_symbols(sym_result.stdout)
    if not symbols:
        dis_result = subprocess.run(
            [tool, "-d", str(elf_path)],
            check=True,
            capture_output=True,
            text=True,
        )
        symbols = _parse_objdump_symbols(dis_result.stdout)
    if not symbols:
        raise RuntimeError(f"no symbols parsed from {elf_path} using {tool}")
    return PcMap(elf_path=str(elf_path.resolve()), objdump=tool, symbols=symbols)


def lookup_pc(pc: int, symbols: list[SymbolEntry]) -> PcSymbol:
    chosen = symbols[0]
    for sym in symbols:
        if sym.addr <= pc:
            chosen = sym
        else:
            break
    return PcSymbol(
        pc=f"0x{pc:x}",
        symbol=chosen.name,
        offset=pc - chosen.addr,
    )


def write_pc_map(pc_map: PcMap, out_path: Path) -> None:
    out_path.write_text(json.dumps(asdict(pc_map), indent=2))


def load_pc_map_json(path: Path) -> list[SymbolEntry]:
    data = json.loads(path.read_text())
    return [SymbolEntry(**item) for item in data["symbols"]]


def main() -> None:
    parser = argparse.ArgumentParser(description="Build PC symbol map from kernel ELF.")
    parser.add_argument("elf", type=Path, help="Path to kernel .elf or binary")
    parser.add_argument("-o", "--output", type=Path, default=Path("pc_map.json"))
    parser.add_argument("--objdump", default=None, help="objdump binary path")
    args = parser.parse_args()
    pc_map = build_pc_map(args.elf, args.objdump)
    write_pc_map(pc_map, args.output)
    print(f"wrote {len(pc_map.symbols)} symbols to {args.output}")


if __name__ == "__main__":
    main()
