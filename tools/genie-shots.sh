#!/usr/bin/env bash
# Runs the genie selftest with a framecap recording over its region.
# Usage: tools/genie-shots.sh <region "x,y,w,h"> [duration_ms]
set -euo pipefail
cd "$(dirname "$0")/.."
region="${1:-1440,620,440,560}"
dur="${2:-5000}"
IFS=, read -r cx cy cw ch <<< "$region"
cd build
rm -rf selftest-shots
mkdir -p selftest-shots
./framecap.exe "$dur" selftest.csv selftest-shots "$cx" "$cy" "$cw" "$ch" 1 >/dev/null 2>&1 &
cap=$!
sleep 1.5
./genie_selftest.exe
wait "$cap" 2>/dev/null || true
echo "frames: $(ls selftest-shots | wc -l)"
