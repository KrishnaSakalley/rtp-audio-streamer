#!/usr/bin/env bash
# Check: zero underruns over a 60-second run. "Underrun"
# here means the playout thread found the ring buffer full when handing off
# a frame to the main thread -- with a 1024-slot ring (~20s of headroom)
# draining into a plain vector insert, this should never happen.
set -euo pipefail

GEN_TONE="$1"
SENDER="$2"
RECEIVER="$3"
PORT="${4:-6150}"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

INPUT="$TMP/input.wav"
OUTPUT="$TMP/output.wav"
RECV_LOG="$TMP/receiver.log"

"$GEN_TONE" "$INPUT" 60.0 440

"$RECEIVER" "$OUTPUT" --port "$PORT" --idle-timeout-ms 800 2>"$RECV_LOG" &
RECV_PID=$!
sleep 0.2

"$SENDER" "$INPUT" --host 127.0.0.1 --port "$PORT"

wait "$RECV_PID"

cat "$RECV_LOG" >&2

UNDERRUNS=$(grep -oE 'underruns=[0-9]+' "$RECV_LOG" | grep -oE '[0-9]+')
if [ -z "$UNDERRUNS" ]; then
  echo "FAIL: could not parse underruns from receiver log" >&2
  exit 1
fi
if [ "$UNDERRUNS" -ne 0 ]; then
  echo "FAIL: expected zero underruns over 60s, got $UNDERRUNS" >&2
  exit 1
fi

cmp "$INPUT" "$OUTPUT"
echo "60s run: zero underruns, bit-identical output: OK"
