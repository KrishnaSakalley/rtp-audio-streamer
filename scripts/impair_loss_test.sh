#!/usr/bin/env bash
# Check: --loss 1.0 must result in the receiver getting nothing at all.
set -euo pipefail

GEN_TONE="$1"
SENDER="$2"
RECEIVER="$3"
IMPAIR="$4"
LISTEN_PORT="${5:-6100}"
FORWARD_PORT="${6:-6101}"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

INPUT="$TMP/input.wav"
OUTPUT="$TMP/output.wav"
"$GEN_TONE" "$INPUT" 1.0 440

"$RECEIVER" "$OUTPUT" --port "$FORWARD_PORT" --idle-timeout-ms 800 &
RECV_PID=$!
sleep 0.2

"$IMPAIR" --listen "$LISTEN_PORT" --forward "$FORWARD_PORT" --loss 1.0 --idle-timeout-ms 800 &
IMPAIR_PID=$!
sleep 0.2

"$SENDER" "$INPUT" --host 127.0.0.1 --port "$LISTEN_PORT"

wait "$IMPAIR_PID"
wait "$RECV_PID"

# A 44-byte file is a valid WAV header with a zero-length data chunk -- the
# receiver still writes *a* file on idle timeout, it just carries no samples.
SIZE=$(stat -c%s "$OUTPUT" 2>/dev/null || stat -f%z "$OUTPUT")
if [ "$SIZE" -gt 44 ]; then
  echo "FAIL: expected an empty output WAV at --loss 1.0, got $SIZE bytes" >&2
  exit 1
fi
echo "loss=1.0 drops everything: OK"
