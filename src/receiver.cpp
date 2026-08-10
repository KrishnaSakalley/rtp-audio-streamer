#include "rtp/audio_format.hpp"
#include "rtp/jitter_buffer.hpp"
#include "rtp/rtp_packet.hpp"
#include "rtp/udp_socket.hpp"
#include "rtp/wav.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void print_usage(const char* argv0) {
  std::fprintf(stderr, "usage: %s <output.wav> [--port P] [--idle-timeout-ms N]\n", argv0);
}

// Short enough that the playout clock below is serviced responsively even
// when no packet is arriving. PLAN.md's precise clock_nanosleep-driven
// playout thread (Phase 6) replaces this single-threaded polling loop with
// an exact 20ms tick; this poll-and-check approach gets the jitter buffer's
// deadline logic correct first, without that machinery.
constexpr int kPollIntervalMs = 5;

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
    socket.set_receive_timeout(kPollIntervalMs);

    // Reserve generously up front (10 minutes @ 8 kHz mono) so the common
    // case never reallocates mid-stream. This is file-mode groundwork, not
    // yet the allocation-free steady state the ring buffer delivers in
    // Phase 6.
    std::vector<int16_t> samples;
    samples.reserve(static_cast<size_t>(rtp::audio::kSampleRateHz) * 60 * 10);

    rtp::jitter::JitterBuffer jbuf;
    size_t malformed_dropped = 0;

    auto process_start = std::chrono::steady_clock::now();
    auto last_activity = process_start;

    uint8_t buffer[rtp::net::kMaxDatagramBytes];
    int16_t frame[rtp::audio::kFrameSamples];

    for (;;) {
      ssize_t n = socket.receive(buffer, sizeof(buffer));
      auto now = std::chrono::steady_clock::now();
      int64_t micros =
          std::chrono::duration_cast<std::chrono::microseconds>(now - process_start).count();
      uint32_t now_samples =
          static_cast<uint32_t>(micros * rtp::audio::kSampleRateHz / 1000000);

      if (n >= 0) {
        last_activity = now;
        auto parsed = rtp::packet::parse(buffer, static_cast<size_t>(n));
        if (parsed.has_value()) {
          jbuf.push(parsed->header.sequence_number, parsed->header.timestamp,
                    parsed->header.payload_type, parsed->payload, parsed->payload_len,
                    now_samples);
        } else {
          malformed_dropped += 1;
        }
      } else {
        auto idle_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(now - last_activity).count();
        if (idle_ms > idle_timeout_ms) {
          jbuf.mark_stream_ended();
        }
      }

      while (jbuf.try_pull_due_frame(now_samples, frame)) {
        samples.insert(samples.end(), frame, frame + rtp::audio::kFrameSamples);
        if (jbuf.finished()) {
          break;
        }
      }

      if (jbuf.finished()) {
        break;
      }
    }

    rtp::wav::write(output_path, rtp::audio::kSampleRateHz, samples);

    const auto& counts = jbuf.counts();
    std::fprintf(stderr,
                 "wrote %zu samples to %s (target_depth_ms=%d received=%llu lost=%llu "
                 "late_dropped=%llu reordered=%llu concealed=%llu duplicate=%llu "
                 "malformed_dropped=%zu)\n",
                 samples.size(), output_path.c_str(), jbuf.target_depth_ms(),
                 static_cast<unsigned long long>(counts.received),
                 static_cast<unsigned long long>(counts.lost),
                 static_cast<unsigned long long>(counts.late_dropped),
                 static_cast<unsigned long long>(counts.reordered),
                 static_cast<unsigned long long>(counts.concealed),
                 static_cast<unsigned long long>(counts.duplicate), malformed_dropped);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "%s: %s\n", argv[0], e.what());
    return 1;
  }

  return 0;
}
