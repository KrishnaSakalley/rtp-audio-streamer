#include "rtp/audio_format.hpp"
#include "rtp/udp_socket.hpp"
#include "rtp/wav.hpp"

#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void print_usage(const char* argv0) {
  std::fprintf(stderr, "usage: %s <output.wav> [--port P] [--idle-timeout-ms N]\n", argv0);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    print_usage(argv[0]);
    return 1;
  }

  std::string output_path = argv[1];
  uint16_t port = 5004;
  int idle_timeout_ms = 500;

  for (int i = 2; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--port" && i + 1 < argc) {
      port = static_cast<uint16_t>(std::atoi(argv[++i]));
    } else if (arg == "--idle-timeout-ms" && i + 1 < argc) {
      idle_timeout_ms = std::atoi(argv[++i]);
    } else {
      print_usage(argv[0]);
      return 1;
    }
  }

  try {
    rtp::net::UdpSocket socket;
    socket.bind_to(port);
    socket.set_receive_timeout(idle_timeout_ms);

    // Reserve generously up front (10 minutes @ 8 kHz mono) so the common
    // case never reallocates mid-stream. This is file-mode groundwork, not
    // yet the allocation-free steady state the ring buffer delivers in
    // Phase 6.
    std::vector<int16_t> samples;
    samples.reserve(static_cast<size_t>(rtp::audio::kSampleRateHz) * 60 * 10);

    int16_t buffer[rtp::net::kMaxDatagramBytes / sizeof(int16_t)];
    for (;;) {
      ssize_t n = socket.receive(buffer, sizeof(buffer));
      if (n < 0) {
        break;  // idle timeout: no packet for idle_timeout_ms, stream is done
      }
      size_t num_samples = static_cast<size_t>(n) / sizeof(int16_t);
      samples.insert(samples.end(), buffer, buffer + num_samples);
    }

    rtp::wav::write(output_path, rtp::audio::kSampleRateHz, samples);
    std::fprintf(stderr, "wrote %zu samples to %s\n", samples.size(), output_path.c_str());
  } catch (const std::exception& e) {
    std::fprintf(stderr, "%s: %s\n", argv[0], e.what());
    return 1;
  }

  return 0;
}
