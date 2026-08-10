# Metrics

Filled in phase by phase as each measurement becomes available; `scripts/generate_metrics.sh`
(Phase 7) regenerates this file in full once the whole pipeline exists.

## Codec SNR (Phase 3)

Measured by `tools/snr_harness.cpp`: a 1 second, 1 kHz sine at 50% full-scale amplitude
(8 kHz mono, 16-bit PCM) is encoded and decoded, and SNR is computed against the original
signal as `10 * log10(signal_power / noise_power)`.

| Codec | SNR (dB) | Gate threshold | Result |
|---|---|---|---|
| G.711 mu-law | 33.83 | > 30 dB | PASS |
| IMA ADPCM | 21.07 | > 18 dB | PASS |

Reproduce with:

```bash
cmake --build build -j
./build/snr_harness --codec ulaw
./build/snr_harness --codec adpcm
```
