#!/usr/bin/env bash
# GATE (PLAN.md Phase 6): a ThreadSanitizer build must run the full
# threaded pipeline clean -- no reported data race. Only registered as a
# CTest when the build was configured with -DRTP_ENABLE_TSAN=ON. Content
# fidelity isn't checked here (TSan's instrumentation overhead can itself
# perturb real-time playout timing); only the absence of a reported race
# and a clean exit from both processes matters.
set -uo pipefail

GEN_TONE="$1"
SENDER="$2"
RECEIVER="$3"
PORT="${4:-6160}"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

INPUT="$TMP/input.wav"
OUTPUT="$TMP/output.wav"
RECV_LOG="$TMP/receiver.log"
SEND_LOG="$TMP/sender.log"

"$GEN_TONE" "$INPUT" 1.0 440

export TSAN_OPTIONS="halt_on_error=1:exitcode=66"

"$RECEIVER" "$OUTPUT" --port "$PORT" --idle-timeout-ms 800 >"$RECV_LOG" 2>&1 &
RECV_PID=$!
sleep 0.2

"$SENDER" "$INPUT" --host 127.0.0.1 --port "$PORT" >"$SEND_LOG" 2>&1
SEND_EXIT=$?

wait "$RECV_PID"
RECV_EXIT=$?

cat "$SEND_LOG" >&2
cat "$RECV_LOG" >&2

if [ "$SEND_EXIT" -ne 0 ] || [ "$RECV_EXIT" -ne 0 ]; then
  echo "FAIL: sender exit=$SEND_EXIT receiver exit=$RECV_EXIT (ThreadSanitizer likely reported a race)" >&2
  exit 1
fi

if grep -qi "ThreadSanitizer" "$SEND_LOG" "$RECV_LOG"; then
  echo "FAIL: ThreadSanitizer reported an issue -- see log above" >&2
  exit 1
fi

echo "ThreadSanitizer clean pipeline run: OK"
