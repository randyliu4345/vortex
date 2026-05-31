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

from build_cfg import (  # noqa: E402
    build_basic_blocks_for_warp,
    build_dynamic_cfg,
    build_warp_paths,
    is_fallthrough,
)
from parse_simx import parse_simx_log  # noqa: E402


FIXTURE = Path(__file__).parent / "fixtures" / "diverge_snippet.log"


class TestBuildCfg(unittest.TestCase):
    def test_is_fallthrough(self):
        self.assertTrue(is_fallthrough("0x80000000", "0x80000004"))
        self.assertFalse(is_fallthrough("0x80000010", "0x80000020"))

    def test_build_warp_paths_fixture(self):
        parsed = parse_simx_log(FIXTURE)
        paths = build_warp_paths(parsed)
        # Warp 1 has a single instruction in the fixture (no consecutive pair).
        self.assertEqual(len(paths), 1)

        w0 = paths[0]
        self.assertEqual(w0.warp_id, 0)
        self.assertEqual(len(w0.edges), 1)
        edge = w0.edges[0]
        self.assertEqual(edge.src_pc, "0x80000000")
        self.assertEqual(edge.dst_pc, "0x80000010")
        self.assertEqual(edge.kind, "branch")
        self.assertTrue(edge.tmask_changed)

    def test_build_basic_blocks_fixture(self):
        parsed = parse_simx_log(FIXTURE)
        events = [e for e in parsed.events if e.warp_id == 0]
        blocks, _ = build_basic_blocks_for_warp(events, 0, 0)
        self.assertEqual(len(blocks), 2)
        self.assertEqual(blocks[0].start_pc, "0x80000000")
        self.assertEqual(blocks[0].end_pc, "0x80000000")
        self.assertEqual(blocks[1].start_pc, "0x80000010")

    def test_build_dynamic_cfg_fixture(self):
        parsed = parse_simx_log(FIXTURE)
        cfg = build_dynamic_cfg(parsed)
        self.assertEqual(len(cfg.warps), 2)
        w0 = next(w for w in cfg.warps if w.warp_id == 0)
        self.assertEqual(len(w0.blocks), 2)
        self.assertEqual(len(w0.edges), 1)
        self.assertTrue(w0.edges[0].divergent)


if __name__ == "__main__":
    unittest.main()
