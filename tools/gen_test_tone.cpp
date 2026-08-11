// Synthesizes a mono sine-wave WAV file. Used as the correctness oracle for
// loopback tests, as the codec SNR test signal, and to generate the demo
// audio -- synthetic and self-generated, so nothing here risks a copyright
// claim.

#include "rtp/audio_format.hpp"
#include "rtp/wav.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void print_usage(const char* argv0) {
  std::fprintf(stderr, "usage: %s <output.wav> <duration_s> <freq_hz> [amplitude 0..1]\n", argv0);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 4) {
    print_usage(argv[0]);
    return 1;
  }

  std::string output_path = argv[1];
  double duration_s = std::atof(argv[2]);
  double freq_hz = std::atof(argv[3]);
  double amplitude = argc > 4 ? std::atof(argv[4]) : 0.5;

  try {
    const size_t num_samples =
        static_cast<size_t>(duration_s * rtp::audio::kSampleRateHz);
    std::vector<int16_t> samples(num_samples);
    constexpr double kTwoPi = 6.283185307179586476925286766559;
    for (size_t i = 0; i < num_samples; ++i) {
      double t = static_cast<double>(i) / rtp::audio::kSampleRateHz;
      double value = amplitude * std::sin(kTwoPi * freq_hz * t);
      samples[i] = static_cast<int16_t>(value * 32767.0);
    }
    rtp::wav::write(output_path, rtp::audio::kSampleRateHz, samples);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "%s: %s\n", argv[0], e.what());
    return 1;
  }

  return 0;
}
