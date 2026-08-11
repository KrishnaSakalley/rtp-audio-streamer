#!/usr/bin/env bash
# Checks that docs/METRICS.md exists with real numbers (no
# placeholders) and all four demo WAVs are present and non-trivial. Checks
# the checked-in deliverable rather than re-running the (slow) generation
# script on every ctest invocation.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
METRICS_FILE="$ROOT_DIR/docs/METRICS.md"
AUDIO_DIR="$ROOT_DIR/docs/audio"

if [ ! -f "$METRICS_FILE" ]; then
  echo "FAIL: $METRICS_FILE does not exist -- run scripts/generate_metrics.sh" >&2
  exit 1
fi

if grep -qE '\bXX\b|TODO|PLACEHOLDER' "$METRICS_FILE"; then
  echo "FAIL: $METRICS_FILE still contains placeholder text" >&2
  exit 1
fi

for f in original.wav loss_5pct_concealment_off.wav loss_5pct_concealment_on.wav adpcm_roundtrip.wav; do
  path="$AUDIO_DIR/$f"
  if [ ! -f "$path" ]; then
    echo "FAIL: $path does not exist -- run scripts/generate_metrics.sh" >&2
    exit 1
  fi
  size=$(stat -c%s "$path" 2>/dev/null || stat -f%z "$path")
  if [ "$size" -le 44 ]; then
    echo "FAIL: $path has no audio data ($size bytes)" >&2
    exit 1
  fi
done

echo "docs/METRICS.md and docs/audio/ demo assets present and non-trivial: OK"
