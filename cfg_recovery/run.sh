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

# End-to-end CFG recovery pipeline (stub).
# Usage: ./cfg_recovery/run.sh [run.log] [kernel.elf]

set -e

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
ROOT_DIR=$(cd "$SCRIPT_DIR/.." && pwd)

LOG=${1:-"$SCRIPT_DIR/run.log"}
ELF=${2:-""}
OUT_DIR=${CFG_RECOVERY_OUT:-"$SCRIPT_DIR/out"}

mkdir -p "$OUT_DIR"

echo "[cfg_recovery] parse: $LOG"
python3 "$SCRIPT_DIR/parse_simx.py" "$LOG" -o "$OUT_DIR/events.json"

if [ -n "$ELF" ] && [ -f "$ELF" ]; then
  echo "[cfg_recovery] pc_map: $ELF"
  python3 "$SCRIPT_DIR/pc_map.py" "$ELF" -o "$OUT_DIR/pc_map.json"
  PC_MAP="--pc-map $OUT_DIR/pc_map.json"
else
  PC_MAP=""
fi

echo "[cfg_recovery] build cfg"
python3 "$SCRIPT_DIR/build_cfg.py" "$LOG" -o "$OUT_DIR/cfg.json" $PC_MAP

echo "[cfg_recovery] visualize"
python3 "$SCRIPT_DIR/visualize.py" "$OUT_DIR/cfg.json" -o "$OUT_DIR/cfg.png"

echo "[cfg_recovery] done → $OUT_DIR"
