#!/bin/sh
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

# Project demo: capture trace, build CFG, evaluate quality (run from build/).
#
#   cd build && source ci/toolchain_env.sh
#   ../configure  # if new regression apps were added
#   make -s
#   ../cfg_recovery/demo.sh [app]
#
# Default app: tests/regression/cfg_diverge_lab

set -e

if [ ! -f "./config.mk" ]; then
  echo "Error: run from Vortex build/ directory." >&2
  exit 1
fi

APP=${1:-tests/regression/cfg_diverge_lab}
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
OUT=cfg_recovery/out/demo
mkdir -p "$OUT"

echo "=== CFG Recovery Demo ==="
echo "App: $APP"

if [ ! -x "$APP/cfg_diverge_lab" ] && [ ! -x "$APP/$(basename "$APP")" ]; then
  echo "Building $APP ..."
  make -C "$APP" > /dev/null
fi

echo "Capturing SimX debug trace ..."
"$SCRIPT_DIR/capture_trace.sh" "$APP" -n4
cp cfg_recovery/run.log "$OUT/run.log"

python3 "$SCRIPT_DIR/parse_simx.py" "$OUT/run.log" -o "$OUT/events.json"
python3 "$SCRIPT_DIR/pc_map.py" "$APP/kernel.elf" -o "$OUT/pc_map.json"
python3 "$SCRIPT_DIR/build_cfg.py" "$OUT/events.json" --cfg -o "$OUT/cfg.json" --pc-map "$OUT/pc_map.json"
python3 "$SCRIPT_DIR/visualize.py" "$OUT/cfg.json" -o "$OUT/cfg_warp0.png" --warp 0
python3 "$SCRIPT_DIR/eval.py" "$OUT/events.json" "$OUT/cfg.json" -o "$OUT/eval.json" --warp 0

echo "=== Demo artifacts: $OUT ==="
ls -la "$OUT"
