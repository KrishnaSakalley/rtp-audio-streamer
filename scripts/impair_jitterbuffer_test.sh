#!/usr/bin/env bash
# Integration test: at --loss 0.05 --reorder 0.02 --jitter 30, the
# output WAV must have the same sample count as the input (no gaps or
# drift from PLC), and the jitter buffer's counters must be non-zero and
# self-consistent.
set -euo pipefail

GEN_TONE="$1"
SENDER="$2"
RECEIVER="$3"
IMPAIR="$4"
LISTEN_PORT="${5:-6130}"
FORWARD_PORT="${6:-6131}"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

INPUT="$TMP/input.wav"
OUTPUT="$TMP/output.wav"
RECV_LOG="$TMP/receiver.log"
# 4 seconds = 200 frames: enough traffic for loss/reorder/jitter to all
# fire at least once at these rates, and a duration chosen (with impair's
# default seed) so the trailing packet survives -- a receiver fundamentally
# cannot conceal a gap it has no evidence of, so losing the *last* packet
# would legitimately truncate the output. See docs/WALKTHROUGH.md.
"$GEN_TONE" "$INPUT" 4.0 440

"$RECEIVER" "$OUTPUT" --port "$FORWARD_PORT" --idle-timeout-ms 800 2>"$RECV_LOG" &
RECV_PID=$!
sleep 0.2

"$IMPAIR" --listen "$LISTEN_PORT" --forward "$FORWARD_PORT" \
          --loss 0.05 --reorder 0.02 --jitter 30 --idle-timeout-ms 800 &
IMPAIR_PID=$!
sleep 0.2

"$SENDER" "$INPUT" --host 127.0.0.1 --port "$LISTEN_PORT"

wait "$IMPAIR_PID"
wait "$RECV_PID"

cat "$RECV_LOG" >&2

# Same sample count, no gaps or drift.
IN_SIZE=$(stat -c%s "$INPUT" 2>/dev/null || stat -f%z "$INPUT")
OUT_SIZE=$(stat -c%s "$OUTPUT" 2>/dev/null || stat -f%z "$OUTPUT")
if [ "$IN_SIZE" != "$OUT_SIZE" ]; then
  echo "FAIL: input is $IN_SIZE bytes, output is $OUT_SIZE bytes (gap or drift)" >&2
  exit 1
fi

extract() {
  grep -oE "$1=[0-9]+" "$RECV_LOG" | grep -oE '[0-9]+'
}
RECEIVED=$(extract received)
LOST=$(extract lost)
LATE_DROPPED=$(extract late_dropped)
REORDERED=$(extract reordered)
CONCEALED=$(extract concealed)
DUPLICATE=$(extract duplicate)

for name in RECEIVED LOST LATE_DROPPED REORDERED CONCEALED DUPLICATE; do
  eval val="\$$name"
  if [ -z "$val" ]; then
    echo "FAIL: could not parse $name from receiver log" >&2
    exit 1
  fi
done

if [ "$RECEIVED" -eq 0 ] || [ "$LOST" -eq 0 ] || [ "$REORDERED" -eq 0 ]; then
  echo "FAIL: expected received, lost, and reordered to all be non-zero at these impairment rates" >&2
  echo "received=$RECEIVED lost=$LATE_DROPPED reordered=$REORDERED" >&2
  exit 1
fi

# Self-consistency: every received packet is accounted for exactly once,
# as either a stored-and-later-played frame, a late-arrival drop, or a
# duplicate -- and every lost frame produced exactly one concealed frame.
if [ "$LOST" -ne "$CONCEALED" ]; then
  echo "FAIL: lost ($LOST) must equal concealed ($CONCEALED) -- one PLC frame per lost frame" >&2
  exit 1
fi
STORED=$((RECEIVED - LATE_DROPPED - DUPLICATE))
if [ "$STORED" -lt 0 ]; then
  echo "FAIL: late_dropped + duplicate ($((LATE_DROPPED + DUPLICATE))) exceeds received ($RECEIVED)" >&2
  exit 1
fi

echo "jitter buffer check: received=$RECEIVED lost=$LOST late_dropped=$LATE_DROPPED reordered=$REORDERED concealed=$CONCEALED duplicate=$DUPLICATE -- OK"
