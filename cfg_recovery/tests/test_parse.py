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

from parse_simx import load_config, parse_simx_log  # noqa: E402


FIXTURE = Path(__file__).parent / "fixtures" / "diverge_snippet.log"


class TestParseSimx(unittest.TestCase):
    def test_load_config(self):
        lines = FIXTURE.read_text().splitlines(keepends=True)
        cfg = load_config(iter(lines))
        self.assertEqual(cfg.num_warps, 4)
        self.assertEqual(cfg.num_threads, 4)

    def test_parse_simx_log_fixture(self):
        parsed = parse_simx_log(FIXTURE)
        self.assertEqual(len(parsed.events), 3)

        e0 = parsed.events[0]
        self.assertEqual(e0.uuid, 1)
        self.assertEqual(e0.warp_id, 0)
        self.assertEqual(e0.pc, "0x80000000")
        self.assertEqual(e0.tmask, "1111")
        self.assertEqual(e0.cycle_schedule, 10)
        self.assertEqual(e0.cycle_commit, 20)

        e1 = parsed.events[1]
        self.assertEqual(e1.uuid, 2)
        self.assertEqual(e1.tmask, "0111")
        self.assertEqual(e1.cycle_commit, 40)

        e2 = parsed.events[2]
        self.assertEqual(e2.warp_id, 1)
        self.assertEqual(e2.cycle_schedule, 50)
        self.assertIsNone(e2.cycle_ibuffer)
        self.assertEqual(e2.cycle_commit, 52)


if __name__ == "__main__":
    unittest.main()
