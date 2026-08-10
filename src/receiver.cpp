#include "rtp/audio_format.hpp"
#include "rtp/rtp_packet.hpp"
#include "rtp/stats.hpp"
#include "rtp/udp_socket.hpp"
#include "rtp/wav.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
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

    rtp::stats::SequenceTracker tracker;
    size_t malformed_dropped = 0;
    uint8_t buffer[rtp::net::kMaxDatagramBytes];

    for (;;) {
      ssize_t n = socket.receive(buffer, sizeof(buffer));
      if (n < 0) {
        break;  // idle timeout: no packet for idle_timeout_ms, stream is done
      }

      auto parsed = rtp::packet::parse(buffer, static_cast<size_t>(n));
      if (!parsed.has_value()) {
        malformed_dropped += 1;
        continue;
      }

      tracker.on_packet(parsed->header.sequence_number);

      // Payload bytes are the sender's raw int16_t samples exactly as they
      // sit in its memory (no network-order conversion -- RFC 3550 only
      // mandates byte order for the header, PCM sample order is a codec
      // concern). memcpy per sample avoids any alignment assumption about
      // where the payload starts inside the datagram buffer.
      size_t num_samples = parsed->payload_len / sizeof(int16_t);
      samples.reserve(samples.size() + num_samples);
      for (size_t i = 0; i < num_samples; ++i) {
        int16_t sample;
        std::memcpy(&sample, parsed->payload + i * sizeof(int16_t), sizeof(sample));
        samples.push_back(sample);
      }
    }

    rtp::wav::write(output_path, rtp::audio::kSampleRateHz, samples);

    const auto& counts = tracker.counts();
    std::fprintf(stderr,
                 "wrote %zu samples to %s (received=%llu gaps=%llu reordered=%llu "
                 "malformed_dropped=%zu)\n",
                 samples.size(), output_path.c_str(),
                 static_cast<unsigned long long>(counts.received),
                 static_cast<unsigned long long>(counts.gaps),
                 static_cast<unsigned long long>(counts.reordered), malformed_dropped);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "%s: %s\n", argv[0], e.what());
    return 1;
  }

  return 0;
}
