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

# End-to-end CFG recovery pipeline.
# Artifacts live under the Vortex *build* tree (see docs/cfg_recovery.md).
#
# Usage (from repo root):
#   ./cfg_recovery/run.sh
#   ./cfg_recovery/run.sh build/cfg_recovery/run.log build/tests/regression/diverge/kernel.elf
#
# Usage (from build/):
#   ../cfg_recovery/run.sh cfg_recovery/run.log tests/regression/diverge/kernel.elf

set -e

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
VORTEX_HOME=$(cd "$SCRIPT_DIR/.." && pwd)

# Resolve build directory: prefer cwd if it is the configured build root.
if [ -f "./config.mk" ] && grep -q "INSTALLDIR" ./config.mk 2>/dev/null; then
  BUILD_DIR=$(pwd)
else
  BUILD_DIR=${VORTEX_BUILD:-$VORTEX_HOME/build}
fi

OUT_DIR=${CFG_RECOVERY_OUT:-"$BUILD_DIR/cfg_recovery/out"}
LOG=${1:-"$BUILD_DIR/cfg_recovery/run.log"}
ELF=${2:-"$BUILD_DIR/tests/regression/diverge/kernel.elf"}

mkdir -p "$OUT_DIR"

# pc_map.py reads TOOLDIR from ./config.mk when cwd is build/
if [ -f "$BUILD_DIR/config.mk" ]; then
  cd "$BUILD_DIR"
fi

echo "[cfg_recovery] build dir: $BUILD_DIR"
echo "[cfg_recovery] parse: $LOG"
python3 "$SCRIPT_DIR/parse_simx.py" "$LOG" -o "$OUT_DIR/events.json"

if [ -n "$ELF" ] && [ -f "$ELF" ]; then
  echo "[cfg_recovery] pc_map: $ELF"
  python3 "$SCRIPT_DIR/pc_map.py" "$ELF" -o "$OUT_DIR/pc_map.json"
  PC_MAP="--pc-map $OUT_DIR/pc_map.json"
else
  PC_MAP=""
fi

echo "[cfg_recovery] warp paths (M3)"
python3 "$SCRIPT_DIR/build_cfg.py" "$OUT_DIR/events.json" --paths-only -o "$OUT_DIR/warp_paths.json"

echo "[cfg_recovery] dynamic CFG (M4–M5)"
python3 "$SCRIPT_DIR/build_cfg.py" "$OUT_DIR/events.json" --cfg -o "$OUT_DIR/cfg.json" $PC_MAP

echo "[cfg_recovery] visualize CFG warp 0"
python3 "$SCRIPT_DIR/visualize.py" "$OUT_DIR/cfg.json" -o "$OUT_DIR/cfg_warp0.png" --warp 0

echo "[cfg_recovery] evaluate (M7)"
python3 "$SCRIPT_DIR/eval.py" "$OUT_DIR/events.json" "$OUT_DIR/cfg.json" -o "$OUT_DIR/eval.json" --warp 0

echo "[cfg_recovery] done → $OUT_DIR"
