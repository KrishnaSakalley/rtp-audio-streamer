// Computes SNR in dB between two WAV files (original vs. reconstructed),
// truncated to the shorter of the two. Used by generate_metrics.sh to find
// max survivable packet loss: the point where SNR against the original
// drops below an "still intelligible" threshold.

#include "rtp/wav.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: %s <original.wav> <reconstructed.wav>\n", argv[0]);
    return 1;
  }

  try {
    rtp::wav::WavData original = rtp::wav::read(argv[1]);
    rtp::wav::WavData reconstructed = rtp::wav::read(argv[2]);
    size_t n = std::min(original.samples.size(), reconstructed.samples.size());

    double signal_power = 0.0;
    double noise_power = 0.0;
    for (size_t i = 0; i < n; ++i) {
      double s = original.samples[i];
      double e = static_cast<double>(original.samples[i]) -
                 static_cast<double>(reconstructed.samples[i]);
      signal_power += s * s;
      noise_power += e * e;
    }

    double snr_db = (noise_power == 0.0) ? 1000.0 : 10.0 * std::log10(signal_power / noise_power);
    std::printf("%.2f\n", snr_db);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "%s: %s\n", argv[0], e.what());
    return 1;
  }

  return 0;
}
