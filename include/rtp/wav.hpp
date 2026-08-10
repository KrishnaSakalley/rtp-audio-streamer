#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace rtp::wav {

struct WavData {
  uint32_t sample_rate = 0;
  uint16_t num_channels = 0;
  uint16_t bits_per_sample = 0;
  std::vector<int16_t> samples;
};

// Reads a canonical RIFF/WAVE file. Only mono, 16-bit PCM is accepted; throws
// std::runtime_error with a specific reason (not RIFF, not PCM, wrong channel
// count, wrong bit depth, truncated chunk) on anything else. Not on the audio
// path -- this runs once at startup, so an exception here is fine.
WavData read(const std::string& path);

// Writes `samples` as a canonical 44-byte-header WAV file at the pipeline's
// fixed mono/16-bit format (see audio_format.hpp).
void write(const std::string& path, uint32_t sample_rate, const std::vector<int16_t>& samples);

}  // namespace rtp::wav
