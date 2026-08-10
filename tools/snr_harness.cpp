// Round-trips a synthetic 1 kHz sine through a codec and reports SNR in dB.
// This is the Phase 3 GATE (PLAN.md §4): mu-law must exceed 30 dB, ADPCM
// must exceed 18 dB. Exits 0 if the threshold is met, 1 otherwise, so this
// binary doubles as a CTest.

#include "rtp/audio_format.hpp"
#include "rtp/codec.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

std::vector<int16_t> make_sine(double duration_s, double freq_hz, double amplitude) {
  const size_t n = static_cast<size_t>(duration_s * rtp::audio::kSampleRateHz);
  std::vector<int16_t> samples(n);
  constexpr double kTwoPi = 6.283185307179586476925286766559;
  for (size_t i = 0; i < n; ++i) {
    double t = static_cast<double>(i) / rtp::audio::kSampleRateHz;
    samples[i] = static_cast<int16_t>(amplitude * 32767.0 * std::sin(kTwoPi * freq_hz * t));
  }
  return samples;
}

double compute_snr_db(const std::vector<int16_t>& original, const std::vector<int16_t>& reconstructed) {
  double signal_power = 0.0;
  double noise_power = 0.0;
  for (size_t i = 0; i < original.size(); ++i) {
    double s = original[i];
    double e = static_cast<double>(original[i]) - static_cast<double>(reconstructed[i]);
    signal_power += s * s;
    noise_power += e * e;
  }
  if (noise_power == 0.0) {
    return 1000.0;  // perfect reconstruction; avoid a divide-by-zero / log(0)
  }
  return 10.0 * std::log10(signal_power / noise_power);
}

void print_usage(const char* argv0) {
  std::fprintf(stderr, "usage: %s --codec {ulaw|adpcm}\n", argv0);
}

}  // namespace

int main(int argc, char** argv) {
  std::string codec_name;
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--codec" && i + 1 < argc) {
      codec_name = argv[++i];
    }
  }
  if (codec_name != "ulaw" && codec_name != "adpcm") {
    print_usage(argv[0]);
    return 1;
  }

  std::vector<int16_t> original = make_sine(1.0, 1000.0, 0.5);
  std::vector<int16_t> reconstructed(original.size());
  double threshold_db;

  if (codec_name == "ulaw") {
    threshold_db = 30.0;
    std::vector<uint8_t> encoded(original.size());
    rtp::codec::ulaw_encode_frame(original.data(), original.size(), encoded.data());
    rtp::codec::ulaw_decode_frame(encoded.data(), original.size(), reconstructed.data());
  } else {
    threshold_db = 18.0;
    rtp::codec::AdpcmState enc_state;
    rtp::codec::AdpcmState dec_state;
    std::vector<uint8_t> encoded((original.size() + 1) / 2);
    rtp::codec::adpcm_encode_frame(original.data(), original.size(), enc_state, encoded.data());
    rtp::codec::adpcm_decode_frame(encoded.data(), original.size(), dec_state, reconstructed.data());
  }

  double snr_db = compute_snr_db(original, reconstructed);
  std::printf("%.2f\n", snr_db);

  if (snr_db < threshold_db) {
    std::fprintf(stderr, "FAIL: %s SNR %.2f dB is below the %.1f dB threshold\n",
                 codec_name.c_str(), snr_db, threshold_db);
    return 1;
  }
  return 0;
}
