#include "rtp/wav.hpp"

#include "rtp/audio_format.hpp"

#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace rtp::wav {

namespace {

// WAV/RIFF is little-endian on the wire regardless of host byte order
// (unlike RTP, which is big-endian) -- pack/unpack explicitly
// rather than trusting host endianness or memcpy'ing a struct over padding.

void write_u32le(std::FILE* f, uint32_t v) {
  uint8_t b[4] = {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8),
                   static_cast<uint8_t>(v >> 16), static_cast<uint8_t>(v >> 24)};
  std::fwrite(b, 1, 4, f);
}

void write_u16le(std::FILE* f, uint16_t v) {
  uint8_t b[2] = {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8)};
  std::fwrite(b, 1, 2, f);
}

uint32_t read_u32le(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

uint16_t read_u16le(const uint8_t* p) {
  return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

[[noreturn]] void fail(const std::string& path, const std::string& reason) {
  throw std::runtime_error("wav: " + path + ": " + reason);
}

}  // namespace

void write(const std::string& path, uint32_t sample_rate, const std::vector<int16_t>& samples) {
  std::FILE* f = std::fopen(path.c_str(), "wb");
  if (f == nullptr) {
    fail(path, "cannot open for writing");
  }

  const uint16_t num_channels = static_cast<uint16_t>(audio::kChannels);
  const uint16_t bits_per_sample = static_cast<uint16_t>(audio::kBitsPerSample);
  const uint16_t block_align = static_cast<uint16_t>(num_channels * bits_per_sample / 8);
  const uint32_t byte_rate = sample_rate * block_align;
  const uint32_t data_bytes = static_cast<uint32_t>(samples.size() * sizeof(int16_t));

  std::fwrite("RIFF", 1, 4, f);
  write_u32le(f, 36 + data_bytes);
  std::fwrite("WAVE", 1, 4, f);

  std::fwrite("fmt ", 1, 4, f);
  write_u32le(f, 16);  // fmt chunk size for uncompressed PCM
  write_u16le(f, 1);   // AudioFormat = PCM
  write_u16le(f, num_channels);
  write_u32le(f, sample_rate);
  write_u32le(f, byte_rate);
  write_u16le(f, block_align);
  write_u16le(f, bits_per_sample);

  std::fwrite("data", 1, 4, f);
  write_u32le(f, data_bytes);
  for (int16_t s : samples) {
    write_u16le(f, static_cast<uint16_t>(s));
  }

  std::fclose(f);
}

WavData read(const std::string& path) {
  std::FILE* f = std::fopen(path.c_str(), "rb");
  if (f == nullptr) {
    fail(path, "cannot open for reading");
  }

  uint8_t header[12];
  if (std::fread(header, 1, 12, f) != 12 || std::memcmp(header, "RIFF", 4) != 0 ||
      std::memcmp(header + 8, "WAVE", 4) != 0) {
    std::fclose(f);
    fail(path, "not a RIFF/WAVE file");
  }

  WavData result;
  bool have_fmt = false;
  bool have_data = false;

  while (!have_data) {
    uint8_t chunk_header[8];
    if (std::fread(chunk_header, 1, 8, f) != 8) {
      break;  // EOF before a data chunk showed up -- reported as missing below
    }
    uint32_t chunk_size = read_u32le(chunk_header + 4);

    if (std::memcmp(chunk_header, "fmt ", 4) == 0) {
      if (chunk_size < 16) {
        std::fclose(f);
        fail(path, "fmt chunk too small");
      }
      std::vector<uint8_t> fmt(chunk_size);
      if (std::fread(fmt.data(), 1, chunk_size, f) != chunk_size) {
        std::fclose(f);
        fail(path, "truncated fmt chunk");
      }
      uint16_t audio_format = read_u16le(fmt.data() + 0);
      result.num_channels = read_u16le(fmt.data() + 2);
      result.sample_rate = read_u32le(fmt.data() + 4);
      result.bits_per_sample = read_u16le(fmt.data() + 14);
      if (audio_format != 1) {
        std::fclose(f);
        fail(path, "not PCM (only uncompressed PCM is supported)");
      }
      have_fmt = true;
    } else if (std::memcmp(chunk_header, "data", 4) == 0) {
      if (!have_fmt) {
        std::fclose(f);
        fail(path, "data chunk before fmt chunk");
      }
      std::vector<uint8_t> raw(chunk_size);
      if (chunk_size > 0 && std::fread(raw.data(), 1, chunk_size, f) != chunk_size) {
        std::fclose(f);
        fail(path, "truncated data chunk");
      }
      result.samples.resize(chunk_size / sizeof(int16_t));
      for (size_t i = 0; i < result.samples.size(); ++i) {
        result.samples[i] = static_cast<int16_t>(read_u16le(raw.data() + i * 2));
      }
      have_data = true;
    } else {
      // Skip unknown chunks (e.g. LIST/INFO); RIFF chunks are word-aligned.
      long skip = static_cast<long>(chunk_size + (chunk_size % 2));
      if (std::fseek(f, skip, SEEK_CUR) != 0) {
        std::fclose(f);
        fail(path, "truncated file while skipping chunk");
      }
    }
  }

  std::fclose(f);

  if (!have_fmt || !have_data) {
    fail(path, "missing fmt or data chunk");
  }
  if (result.num_channels != audio::kChannels) {
    fail(path, "only mono WAV files are supported");
  }
  if (result.bits_per_sample != audio::kBitsPerSample) {
    fail(path, "only 16-bit PCM WAV files are supported");
  }

  return result;
}

}  // namespace rtp::wav
