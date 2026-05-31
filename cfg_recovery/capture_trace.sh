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

# Capture a SimX debug trace (M0). Run from the configured build/ directory.
#
#   cd build && source ci/toolchain_env.sh
#   ../cfg_recovery/capture_trace.sh [app-path] [run-args...]
#
# Example:
#   ../cfg_recovery/capture_trace.sh tests/regression/diverge -n4
#
# Output: build/cfg_recovery/run.log

set -e

if [ ! -f "./config.mk" ]; then
  echo "Error: run from Vortex build/ (missing ./config.mk). Use: cd build && ../cfg_recovery/capture_trace.sh ..." >&2
  exit 1
fi

APP=${1:-tests/regression/diverge}
shift || true
ARGS=${*:- -n4}
BUILD_DIR=$(pwd)
LOG="$BUILD_DIR/cfg_recovery/run.log"
APP_DIR="$BUILD_DIR/$APP"
APP_NAME=$(basename "$APP")
BIN="$APP_DIR/$APP_NAME"

mkdir -p "$BUILD_DIR/cfg_recovery"

if [ ! -f "$BUILD_DIR/runtime/libsimx.so" ]; then
  echo "[cfg_recovery] building simx with DEBUG=3 ..."
  DEBUG=3 make -C runtime/simx
fi

if [ ! -x "$BIN" ]; then
  echo "[cfg_recovery] building $APP ..."
  make -C "$APP_DIR"
fi

if [ ! -x "$BIN" ]; then
  echo "Error: executable not found: $BIN" >&2
  exit 1
fi

echo "[cfg_recovery] running $BIN $ARGS → $LOG"
(
  cd "$APP_DIR"
  LD_LIBRARY_PATH="$BUILD_DIR/runtime" VORTEX_DRIVER=simx "./$(basename "$BIN")" $ARGS
) >"$LOG" 2>&1
COUNT=$(grep -c 'DEBUG Instr' "$LOG" || true)
echo "[cfg_recovery] $COUNT DEBUG Instr lines in $LOG"
