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

import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from build_cfg import build_dynamic_cfg, write_cfg  # noqa: E402
from eval import DEFAULT_THRESHOLDS, evaluate  # noqa: E402
from parse_simx import parse_simx_log, write_events_json  # noqa: E402


FIXTURE = Path(__file__).parent / "fixtures" / "diverge_snippet.log"


class TestCfgQuality(unittest.TestCase):
    def test_evaluate_fixture_passes(self):
        parsed = parse_simx_log(FIXTURE)
        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            events_path = tmp_path / "events.json"
            cfg_path = tmp_path / "cfg.json"
            write_events_json(parsed, events_path)
            write_cfg(build_dynamic_cfg(parsed), cfg_path)
            report = evaluate(events_path, cfg_path, warp_id=0)
            self.assertGreaterEqual(report.pc_coverage, DEFAULT_THRESHOLDS["pc_coverage_min"])
            self.assertGreaterEqual(report.edge_soundness, DEFAULT_THRESHOLDS["edge_soundness_min"])
            self.assertGreaterEqual(report.divergent_edges, 1)


if __name__ == "__main__":
    unittest.main()
