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

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from pc_map import (  # noqa: E402
    SymbolEntry,
    _parse_objdump_symbols,
    lookup_pc,
)


class TestPcMapParse(unittest.TestCase):
    def test_parse_objdump_symbols(self):
        sample = """
0000000080000100 l     F .text	0000000000000042 kernel_body
0000000080000200 g     F .text	0000000000000010 main
"""
        symbols = _parse_objdump_symbols(sample)
        self.assertEqual(len(symbols), 2)
        self.assertEqual(symbols[0].name, "kernel_body")
        self.assertEqual(symbols[1].addr, 0x80000200)

    def test_parse_disasm_labels(self):
        sample = """
80000100 <kernel_body>:
80000104: 00000013  nop
80000200 <main>:
"""
        symbols = _parse_objdump_symbols(sample)
        self.assertEqual(symbols[0].name, "kernel_body")
        self.assertEqual(symbols[1].name, "main")

    def test_lookup_pc(self):
        symbols = [
            SymbolEntry(addr=0x80000100, name="kernel_body", size=0x40),
            SymbolEntry(addr=0x80000200, name="main", size=0x10),
        ]
        hit = lookup_pc(0x80000118, symbols)
        self.assertEqual(hit.symbol, "kernel_body")
        self.assertEqual(hit.offset, 0x18)


if __name__ == "__main__":
    unittest.main()
