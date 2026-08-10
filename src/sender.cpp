#include "rtp/audio_format.hpp"
#include "rtp/udp_socket.hpp"
#include "rtp/wav.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>

namespace {

void print_usage(const char* argv0) {
  std::fprintf(stderr, "usage: %s <input.wav> [--host H] [--port P]\n", argv0);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    print_usage(argv[0]);
    return 1;
  }

  std::string input_path = argv[1];
  std::string host = "127.0.0.1";
  uint16_t port = 5004;

  for (int i = 2; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--host" && i + 1 < argc) {
      host = argv[++i];
    } else if (arg == "--port" && i + 1 < argc) {
      port = static_cast<uint16_t>(std::atoi(argv[++i]));
    } else {
      print_usage(argv[0]);
      return 1;
    }
  }

  rtp::wav::WavData wav;
  try {
    wav = rtp::wav::read(input_path);

    rtp::net::UdpSocket socket;
    const size_t total_samples = wav.samples.size();
    size_t sent = 0;
    while (sent < total_samples) {
      size_t frame_samples =
          std::min(static_cast<size_t>(rtp::audio::kFrameSamples), total_samples - sent);
      socket.send_to(wav.samples.data() + sent, frame_samples * sizeof(int16_t), host, port);
      sent += frame_samples;
    }
    std::fprintf(stderr, "sent %zu samples to %s:%u\n", total_samples, host.c_str(), port);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "%s: %s\n", argv[0], e.what());
    return 1;
  }

  return 0;
}
