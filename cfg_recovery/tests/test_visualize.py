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

from visualize import cfg_to_dot, paths_to_dot  # noqa: E402


class TestVisualize(unittest.TestCase):
    def test_paths_to_dot(self):
        data = {
            "paths": [
                {
                    "warp_id": 0,
                    "core_id": 0,
                    "edges": [
                        {
                            "src_pc": "0x1",
                            "dst_pc": "0x2",
                            "kind": "branch",
                            "weight": 2,
                            "tmask_changed": True,
                        }
                    ],
                }
            ]
        }
        dot = paths_to_dot(data, warp_id=0)
        self.assertIn("digraph warp_paths", dot)
        self.assertIn("0x1", dot)
        self.assertIn("color=\"red\"", dot)

    def test_cfg_to_dot(self):
        data = {
            "warps": [
                {
                    "warp_id": 0,
                    "blocks": [
                        {
                            "block_id": "b0",
                            "start_pc": "0x10",
                            "end_pc": "0x14",
                            "symbol": "foo",
                            "is_divergence_point": True,
                            "is_merge_point": False,
                        }
                    ],
                    "edges": [
                        {
                            "src_block": "b0",
                            "dst_block": "b1",
                            "weight": 1,
                            "divergent": True,
                        }
                    ],
                }
            ]
        }
        dot = cfg_to_dot(data)
        self.assertIn("digraph cfg", dot)
        self.assertIn("b0", dot)


if __name__ == "__main__":
    unittest.main()
