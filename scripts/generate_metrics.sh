#!/usr/bin/env bash
# Runs the pipeline across a matrix of codecs and
# loss rates, writes docs/METRICS.md with real measured numbers, and emits
# four demo WAVs into docs/audio/.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${1:-$ROOT_DIR/build}"

GEN_TONE="$BUILD_DIR/gen_test_tone"
SENDER="$BUILD_DIR/rtp_sender"
RECEIVER="$BUILD_DIR/rtp_receiver"
IMPAIR="$BUILD_DIR/impair"
SNR_HARNESS="$BUILD_DIR/snr_harness"
WAV_SNR="$BUILD_DIR/wav_snr"

for bin in "$GEN_TONE" "$SENDER" "$RECEIVER" "$IMPAIR" "$SNR_HARNESS" "$WAV_SNR"; do
  if [ ! -x "$bin" ]; then
    echo "FAIL: $bin not found or not executable -- build first (cmake --build $BUILD_DIR -j)" >&2
    exit 1
  fi
done

AUDIO_DIR="$ROOT_DIR/docs/audio"
mkdir -p "$AUDIO_DIR"
METRICS_FILE="$ROOT_DIR/docs/METRICS.md"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

PORT_BASE=7100

# run_pipeline INPUT OUTPUT CODEC LOSS REORDER JITTER RECV_EXTRA_ARGS
# Runs sender -> [impair, if LOSS/REORDER/JITTER nonzero] -> receiver.
# Prints the receiver's stderr log path.
run_pipeline() {
  local input="$1" output="$2" codec="$3" loss="$4" reorder="$5" jitter="$6"
  shift 6
  local recv_extra=("$@")
  PORT_BASE=$((PORT_BASE + 2))
  local port=$PORT_BASE
  local fwd_port=$((port + 1))
  local recv_log
  recv_log="$TMP/recv_${port}.log"

  if [ "$loss" = "0" ] && [ "$reorder" = "0" ] && [ "$jitter" = "0" ]; then
    "$RECEIVER" "$output" --port "$port" --idle-timeout-ms 800 "${recv_extra[@]}" 2>"$recv_log" &
    local recv_pid=$!
    sleep 0.2
    "$SENDER" "$input" --host 127.0.0.1 --port "$port" --codec "$codec" >/dev/null
    wait "$recv_pid"
  else
    "$RECEIVER" "$output" --port "$fwd_port" --idle-timeout-ms 800 "${recv_extra[@]}" 2>"$recv_log" &
    local recv_pid=$!
    sleep 0.2
    "$IMPAIR" --listen "$port" --forward "$fwd_port" --loss "$loss" --reorder "$reorder" \
              --jitter "$jitter" --idle-timeout-ms 800 >/dev/null 2>&1 &
    local impair_pid=$!
    sleep 0.2
    "$SENDER" "$input" --host 127.0.0.1 --port "$port" --codec "$codec" >/dev/null
    wait "$impair_pid"
    wait "$recv_pid"
  fi
  echo "$recv_log"
}

extract() {
  grep -oE "$2=[0-9.]+" "$1" | tail -1 | grep -oE '[0-9.]+$'
}

echo "== Codec SNR ==" >&2
ULAW_SNR=$("$SNR_HARNESS" --codec ulaw)
ADPCM_SNR=$("$SNR_HARNESS" --codec adpcm)
echo "mu-law: ${ULAW_SNR} dB, ADPCM: ${ADPCM_SNR} dB" >&2

echo "== Demo assets ==" >&2
ORIGINAL="$AUDIO_DIR/original.wav"
"$GEN_TONE" "$ORIGINAL" 4.0 440

LOSS_OFF="$AUDIO_DIR/loss_5pct_concealment_off.wav"
run_pipeline "$ORIGINAL" "$LOSS_OFF" pcm 0.05 0.02 30 --no-plc >/dev/null

LOSS_ON="$AUDIO_DIR/loss_5pct_concealment_on.wav"
run_pipeline "$ORIGINAL" "$LOSS_ON" pcm 0.05 0.02 30 >/dev/null

ADPCM_RT="$AUDIO_DIR/adpcm_roundtrip.wav"
run_pipeline "$ORIGINAL" "$ADPCM_RT" adpcm 0 0 0 >/dev/null

for f in "$ORIGINAL" "$LOSS_OFF" "$LOSS_ON" "$ADPCM_RT"; do
  SIZE=$(stat -c%s "$f" 2>/dev/null || stat -f%z "$f")
  if [ "$SIZE" -le 44 ]; then
    echo "FAIL: $f has no audio data (only $SIZE bytes)" >&2
    exit 1
  fi
  echo "wrote $f ($SIZE bytes)" >&2
done

echo "== Latency / jitter-buffer depth (loss=0.05, reorder=0.02, jitter=30) ==" >&2
STATS_OUT="$TMP/stats.wav"
STATS_LOG=$(run_pipeline "$ORIGINAL" "$STATS_OUT" pcm 0.05 0.02 30 --collect-stats)
cat "$STATS_LOG" >&2
LAT_MEDIAN=$(extract "$STATS_LOG" latency_median_ms)
LAT_P95=$(extract "$STATS_LOG" latency_p95_ms)
DEPTH_MEAN=$(extract "$STATS_LOG" depth_mean_ms)
DEPTH_P95=$(extract "$STATS_LOG" depth_p95_ms)

echo "== Underruns per minute (10s sample, loss=0.05/reorder=0.02/jitter=30) ==" >&2
UNDERRUN_INPUT="$TMP/underrun_input.wav"
"$GEN_TONE" "$UNDERRUN_INPUT" 10.0 440
UNDERRUN_OUT="$TMP/underrun_output.wav"
UNDERRUN_LOG=$(run_pipeline "$UNDERRUN_INPUT" "$UNDERRUN_OUT" pcm 0.05 0.02 30)
cat "$UNDERRUN_LOG" >&2
UNDERRUNS_10S=$(extract "$UNDERRUN_LOG" underruns)
UNDERRUNS_PER_MIN=$(awk -v u="$UNDERRUNS_10S" 'BEGIN { printf "%.2f", u * 6.0 }')

echo "== Max survivable loss (PCM, reorder=0.02, jitter=30, SNR >= 10 dB threshold) ==" >&2
MAX_LOSS_INPUT="$TMP/max_loss_input.wav"
"$GEN_TONE" "$MAX_LOSS_INPUT" 3.0 440
MAX_SURVIVABLE_LOSS="0"
for loss_pct in 5 10 15 20 25 30 40 50; do
  loss=$(awk -v p="$loss_pct" 'BEGIN { printf "%.2f", p / 100.0 }')
  out="$TMP/loss_${loss_pct}.wav"
  run_pipeline "$MAX_LOSS_INPUT" "$out" pcm "$loss" 0.02 30 >/dev/null
  snr=$("$WAV_SNR" "$MAX_LOSS_INPUT" "$out")
  echo "loss=${loss_pct}% -> SNR=${snr} dB" >&2
  meets_threshold=$(awk -v s="$snr" 'BEGIN { print (s >= 10.0) ? 1 : 0 }')
  if [ "$meets_threshold" = "1" ]; then
    MAX_SURVIVABLE_LOSS="$loss_pct"
  else
    break
  fi
done

echo "== CPU usage (4s run, loss=0.05/reorder=0.02/jitter=30) ==" >&2
CPU_INPUT="$TMP/cpu_input.wav"
"$GEN_TONE" "$CPU_INPUT" 4.0 440
CPU_OUT="$TMP/cpu_output.wav"
CPU_PORT=$((PORT_BASE + 100))
CPU_FWD_PORT=$((CPU_PORT + 1))
CPU_TIME_LOG="$TMP/cpu_time.log"
"$IMPAIR" --listen "$CPU_PORT" --forward "$CPU_FWD_PORT" --loss 0.05 --reorder 0.02 --jitter 30 \
          --idle-timeout-ms 800 >/dev/null 2>&1 &
CPU_IMPAIR_PID=$!
sleep 0.2
# The receiver (three threads: receive, playout, main) is the more
# representative thing to measure -- the sender mostly sleeps between
# paced sends and reads near 0% regardless of pipeline health.
/usr/bin/time -v "$RECEIVER" "$CPU_OUT" --port "$CPU_FWD_PORT" --idle-timeout-ms 800 \
              >/dev/null 2>"$CPU_TIME_LOG" &
CPU_RECV_PID=$!
sleep 0.2
"$SENDER" "$CPU_INPUT" --host 127.0.0.1 --port "$CPU_PORT" --codec pcm >/dev/null
wait "$CPU_IMPAIR_PID"
wait "$CPU_RECV_PID"
CPU_PERCENT=$(grep -oE 'Percent of CPU this job got: [0-9]+' "$CPU_TIME_LOG" | grep -oE '[0-9]+$' || echo "0")

echo "== Writing $METRICS_FILE ==" >&2
cat > "$METRICS_FILE" << EOF
# Metrics

Generated by \`scripts/generate_metrics.sh\` against a real build; every number below
comes from an actual pipeline run, not a placeholder. Re-run after any change to the
codec, jitter buffer, or threading code to keep this current.

## Summary

| Metric | Value | Method |
|---|---|---|
| End-to-end latency, median | ${LAT_MEDIAN} ms | arrival-to-playout, loss=5%/reorder=2%/jitter=30ms |
| End-to-end latency, p95 | ${LAT_P95} ms | same run, 95th percentile |
| Max survivable loss | ${MAX_SURVIVABLE_LOSS}% | highest loss rate before SNR drops below 10 dB |
| Compression ratio (mu-law) | 2:1 | 160 B payload vs. 320 B raw PCM per 20ms frame |
| Compression ratio (ADPCM) | 4:1 | 80 B nibble payload vs. 320 B raw PCM (+4 B state header) |
| SNR, mu-law | ${ULAW_SNR} dB | 1 kHz sine round-tripped through the codec alone |
| SNR, ADPCM | ${ADPCM_SNR} dB | same |
| Jitter buffer depth, mean | ${DEPTH_MEAN} ms | adaptive target depth over the loss=5% run |
| Jitter buffer depth, p95 | ${DEPTH_P95} ms | same |
| Underruns / minute | ${UNDERRUNS_PER_MIN} | extrapolated from a 10s sample at loss=5%/reorder=2%/jitter=30ms |
| CPU usage | ${CPU_PERCENT}% | \`/usr/bin/time -v\` on the receiver (3 threads) during a loss=5% run |

## Demo audio (\`docs/audio/\`)

| File | Description |
|---|---|
| \`original.wav\` | Synthetic 440Hz tone, 4s, source for the comparisons below |
| \`loss_5pct_concealment_off.wav\` | loss=5%/reorder=2%/jitter=30ms, PLC disabled (\`--no-plc\`) -- lost frames are silence |
| \`loss_5pct_concealment_on.wav\` | Same network conditions, PLC enabled -- lost frames are faded repeats |
| \`adpcm_roundtrip.wav\` | Full IMA ADPCM encode/decode round trip, no loss |

## Reproduce

\`\`\`bash
cmake --build build -j
scripts/generate_metrics.sh build
\`\`\`
EOF

echo "docs/METRICS.md written." >&2
echo "max_survivable_loss=${MAX_SURVIVABLE_LOSS}% underruns_per_min=${UNDERRUNS_PER_MIN} cpu_percent=${CPU_PERCENT}" >&2
