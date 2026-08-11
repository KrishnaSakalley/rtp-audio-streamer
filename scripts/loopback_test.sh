#!/usr/bin/env bash
# Runs rtp_sender -> UDP -> rtp_receiver on a synthetic tone and asserts the
# output WAV is byte-for-byte identical to the input -- the core correctness
# oracle for the whole pipeline. Invoked by CTest with absolute binary paths.
set -euo pipefail

GEN_TONE="$1"
SENDER="$2"
RECEIVER="$3"
PORT="${4:-5004}"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

INPUT="$TMP/input.wav"
OUTPUT="$TMP/output.wav"

"$GEN_TONE" "$INPUT" 1.5 440

"$RECEIVER" "$OUTPUT" --port "$PORT" --idle-timeout-ms 500 &
RECV_PID=$!

sleep 0.3
"$SENDER" "$INPUT" --host 127.0.0.1 --port "$PORT"

wait "$RECV_PID"

cmp "$INPUT" "$OUTPUT"
echo "loopback bit-identical: OK"
