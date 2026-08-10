#include "rtp/wav.hpp"
#include "test_util.hpp"

#include <cstdio>
#include <stdexcept>
#include <vector>

int main() {
  const char* path = "wav_test_roundtrip.wav";

  // Round trip: extremes (0, +/-1, INT16 min/max) must come back bit-exact.
  {
    std::vector<int16_t> samples = {0, 1, -1, 32767, -32768, 100, -100, 0};
    rtp::wav::write(path, 8000, samples);
    rtp::wav::WavData data = rtp::wav::read(path);
    RTP_CHECK(data.sample_rate == 8000);
    RTP_CHECK(data.num_channels == 1);
    RTP_CHECK(data.bits_per_sample == 16);
    RTP_CHECK(data.samples.size() == samples.size());
    for (size_t i = 0; i < samples.size(); ++i) {
      RTP_CHECK(data.samples[i] == samples[i]);
    }
    std::remove(path);
  }

  // Empty input: zero samples must round-trip to zero samples, not a parse error.
  {
    std::vector<int16_t> empty_samples;
    rtp::wav::write(path, 8000, empty_samples);
    rtp::wav::WavData data = rtp::wav::read(path);
    RTP_CHECK(data.samples.empty());
    std::remove(path);
  }

  // Malformed header: not a RIFF file at all.
  {
    const char* bad_path = "wav_test_bad_riff.wav";
    std::FILE* f = std::fopen(bad_path, "wb");
    const char junk[4] = {'X', 'X', 'X', 'X'};
    std::fwrite(junk, 1, 4, f);
    std::fclose(f);

    bool threw = false;
    try {
      rtp::wav::read(bad_path);
    } catch (const std::exception&) {
      threw = true;
    }
    RTP_CHECK(threw);
    std::remove(bad_path);
  }

  // Malformed data: RIFF/WAVE header present but data chunk truncated.
  {
    const char* bad_path = "wav_test_truncated.wav";
    std::FILE* f = std::fopen(bad_path, "wb");
    std::fwrite("RIFF", 1, 4, f);
    const uint8_t size_bytes[4] = {100, 0, 0, 0};  // claims 100 bytes, has none
    std::fwrite(size_bytes, 1, 4, f);
    std::fwrite("WAVE", 1, 4, f);
    std::fwrite("fmt ", 1, 4, f);
    const uint8_t fmt_size[4] = {16, 0, 0, 0};
    std::fwrite(fmt_size, 1, 4, f);
    const uint8_t fmt_body[16] = {1, 0, 1, 0, 0x40, 0x1f, 0, 0,
                                   0x80, 0x3e, 0, 0, 2, 0, 16, 0};
    std::fwrite(fmt_body, 1, 16, f);
    std::fwrite("data", 1, 4, f);
    const uint8_t data_size[4] = {100, 0, 0, 0};  // claims 100 bytes of payload
    std::fwrite(data_size, 1, 4, f);
    // ... but the file ends here, no payload written.
    std::fclose(f);

    bool threw = false;
    try {
      rtp::wav::read(bad_path);
    } catch (const std::exception&) {
      threw = true;
    }
    RTP_CHECK(threw);
    std::remove(bad_path);
  }

  std::puts("wav_test OK");
  return 0;
}
