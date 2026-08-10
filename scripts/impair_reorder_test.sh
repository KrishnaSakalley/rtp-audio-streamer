#!/usr/bin/env bash
# GATE: --reorder 0.3 must demonstrably produce out-of-order sequence numbers
# in the receiver's log (its SequenceTracker "reordered=" counter).
set -euo pipefail

GEN_TONE="$1"
SENDER="$2"
RECEIVER="$3"
IMPAIR="$4"
LISTEN_PORT="${5:-6120}"
FORWARD_PORT="${6:-6121}"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

INPUT="$TMP/input.wav"
OUTPUT="$TMP/output.wav"
RECV_LOG="$TMP/receiver.log"
"$GEN_TONE" "$INPUT" 3.0 440  # longer clip: more packets, more chances to reorder

"$RECEIVER" "$OUTPUT" --port "$FORWARD_PORT" --idle-timeout-ms 800 2>"$RECV_LOG" &
RECV_PID=$!
sleep 0.2

"$IMPAIR" --listen "$LISTEN_PORT" --forward "$FORWARD_PORT" --reorder 0.3 --idle-timeout-ms 800 &
IMPAIR_PID=$!
sleep 0.2

"$SENDER" "$INPUT" --host 127.0.0.1 --port "$LISTEN_PORT"

wait "$IMPAIR_PID"
wait "$RECV_PID"

cat "$RECV_LOG" >&2
REORDERED=$(grep -oE 'reordered=[0-9]+' "$RECV_LOG" | grep -oE '[0-9]+')
if [ -z "$REORDERED" ] || [ "$REORDERED" -eq 0 ]; then
  echo "FAIL: expected reordered > 0 in receiver log, got '$REORDERED'" >&2
  exit 1
fi
echo "reorder=0.3 produced $REORDERED out-of-order packets: OK"
