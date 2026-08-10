#!/usr/bin/env bash
# GATE: --loss 0.0 (and every other impairment off) must leave output unchanged.
set -euo pipefail

GEN_TONE="$1"
SENDER="$2"
RECEIVER="$3"
IMPAIR="$4"
LISTEN_PORT="${5:-6110}"
FORWARD_PORT="${6:-6111}"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

INPUT="$TMP/input.wav"
OUTPUT="$TMP/output.wav"
"$GEN_TONE" "$INPUT" 1.0 440

"$RECEIVER" "$OUTPUT" --port "$FORWARD_PORT" --idle-timeout-ms 800 &
RECV_PID=$!
sleep 0.2

"$IMPAIR" --listen "$LISTEN_PORT" --forward "$FORWARD_PORT" \
          --loss 0.0 --reorder 0.0 --jitter 0 --dup 0.0 --idle-timeout-ms 800 &
IMPAIR_PID=$!
sleep 0.2

"$SENDER" "$INPUT" --host 127.0.0.1 --port "$LISTEN_PORT"

wait "$IMPAIR_PID"
wait "$RECV_PID"

cmp "$INPUT" "$OUTPUT"
echo "loss=0.0 passthrough bit-identical: OK"
