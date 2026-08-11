#include "rtp/audio_format.hpp"
#include "rtp/jitter_buffer.hpp"
#include "rtp/ring_buffer.hpp"
#include "rtp/rtp_packet.hpp"
#include "rtp/udp_socket.hpp"
#include "rtp/wav.hpp"

#include <time.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

void print_usage(const char* argv0) {
  std::fprintf(stderr,
                "usage: %s <output.wav> [--port P] [--idle-timeout-ms N] [--collect-stats] "
                "[--no-plc]\n",
                argv0);
}

// Sorts a copy of `values` and returns the element at percentile `p`
// (0.0-1.0). Metrics-only helper -- not on the audio path.
double percentile(std::vector<uint32_t> values, double p) {
  if (values.empty()) {
    return 0.0;
  }
  std::sort(values.begin(), values.end());
  size_t idx = static_cast<size_t>(p * static_cast<double>(values.size() - 1));
  return static_cast<double>(values[idx]);
}

double mean_of(const std::vector<int>& values) {
  if (values.empty()) {
    return 0.0;
  }
  double sum = 0.0;
  for (int v : values) {
    sum += v;
  }
  return sum / static_cast<double>(values.size());
}

double percentile_int(std::vector<int> values, double p) {
  if (values.empty()) {
    return 0.0;
  }
  std::sort(values.begin(), values.end());
  size_t idx = static_cast<size_t>(p * static_cast<double>(values.size() - 1));
  return static_cast<double>(values[idx]);
}

// Receive thread's socket poll granularity -- short enough that idle
// detection and the shared clock stay responsive between packets.
constexpr int kPollIntervalMs = 5;

// Ring capacity between the playout thread and the main thread's WAV
// writer: a power of two (PLAN.md Phase 6), sized to ~20s of audio at
// 20ms/frame -- generous headroom, since the consumer here (a vector
// insert) is far faster than the 20ms/frame production rate and should
// never come close to filling it.
constexpr size_t kRingCapacity = 1024;

struct AudioFrame {
  int16_t samples[rtp::audio::kFrameSamples];
};

uint32_t samples_since(const timespec& start, const timespec& now) {
  int64_t sec_diff = static_cast<int64_t>(now.tv_sec) - static_cast<int64_t>(start.tv_sec);
  int64_t nsec_diff = static_cast<int64_t>(now.tv_nsec) - static_cast<int64_t>(start.tv_nsec);
  int64_t total_ns = sec_diff * 1000000000LL + nsec_diff;
  if (total_ns < 0) {
    total_ns = 0;
  }
  int64_t samples = total_ns * rtp::audio::kSampleRateHz / 1000000000LL;
  return static_cast<uint32_t>(samples);
}

void add_millis(timespec& ts, long millis) {
  ts.tv_nsec += millis * 1000000L;
  while (ts.tv_nsec >= 1000000000L) {
    ts.tv_nsec -= 1000000000L;
    ts.tv_sec += 1;
  }
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
  bool collect_stats = false;
  bool no_plc = false;

  for (int i = 2; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--port" && i + 1 < argc) {
      port = static_cast<uint16_t>(std::atoi(argv[++i]));
    } else if (arg == "--idle-timeout-ms" && i + 1 < argc) {
      idle_timeout_ms = std::atoi(argv[++i]);
    } else if (arg == "--collect-stats") {
      collect_stats = true;
    } else if (arg == "--no-plc") {
      no_plc = true;
    } else {
      print_usage(argv[0]);
      return 1;
    }
  }

  try {
    rtp::net::UdpSocket socket;
    socket.bind_to(port);
    socket.set_receive_timeout(kPollIntervalMs);

    timespec process_start{};
    clock_gettime(CLOCK_MONOTONIC, &process_start);

    rtp::jitter::JitterBuffer::Options jbuf_options;
    jbuf_options.collect_stats = collect_stats;
    jbuf_options.disable_plc_fade = no_plc;
    rtp::jitter::JitterBuffer jbuf(jbuf_options);
    // JitterBuffer has no internal synchronization of its own (it's
    // exercised single-threaded by its unit tests); this mutex is what
    // makes push() (receive thread) and try_pull_due_frame() /
    // mark_stream_ended() / finished() (playout thread) safe to call from
    // two different threads. The ring buffer below is the pipeline's one
    // genuinely lock-free handoff (PLAN.md Phase 6); this cross-thread
    // access to a plain stateful object is a plain mutex by design, not an
    // oversight -- the plan's lock-free requirement is specifically the
    // ring buffer.
    std::mutex jbuf_mutex;

    rtp::ring::SpscRingBuffer<AudioFrame, kRingCapacity> ring;
    std::atomic<bool> playout_done{false};
    std::atomic<uint64_t> underruns{0};  // ring was full when the playout thread tried to push
    size_t malformed_dropped = 0;

    std::thread receive_thread([&]() {
      uint8_t buffer[rtp::net::kMaxDatagramBytes];
      timespec last_activity = process_start;
      for (;;) {
        ssize_t n = socket.receive(buffer, sizeof(buffer));
        timespec now{};
        clock_gettime(CLOCK_MONOTONIC, &now);
        uint32_t now_samples = samples_since(process_start, now);

        if (n >= 0) {
          last_activity = now;
          auto parsed = rtp::packet::parse(buffer, static_cast<size_t>(n));
          if (parsed.has_value()) {
            std::lock_guard<std::mutex> lock(jbuf_mutex);
            jbuf.push(parsed->header.sequence_number, parsed->header.timestamp,
                      parsed->header.payload_type, parsed->payload, parsed->payload_len,
                      now_samples);
          } else {
            malformed_dropped += 1;
          }
        } else {
          long idle_ms = (now.tv_sec - last_activity.tv_sec) * 1000L +
                         (now.tv_nsec - last_activity.tv_nsec) / 1000000L;
          if (idle_ms > idle_timeout_ms) {
            std::lock_guard<std::mutex> lock(jbuf_mutex);
            jbuf.mark_stream_ended();
          }
        }

        if (playout_done.load(std::memory_order_acquire)) {
          break;
        }
      }
    });

    // Playout thread: a fixed 20ms clock via clock_nanosleep(TIMER_ABSTIME)
    // against an absolute, monotonically-advancing deadline -- not
    // sleep_for, which drifts (PLAN.md §9) -- pulling due frames from the
    // jitter buffer and handing them to the main thread through the
    // lock-free ring. This thread never allocates and never blocks on I/O;
    // that separation is the entire reason the ring buffer exists.
    std::thread playout_thread([&]() {
      timespec deadline = process_start;
      for (;;) {
        add_millis(deadline, rtp::audio::kFrameDurationMs);
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, nullptr);

        uint32_t now_samples = samples_since(process_start, deadline);
        bool done;
        {
          std::lock_guard<std::mutex> lock(jbuf_mutex);
          AudioFrame frame;
          while (jbuf.try_pull_due_frame(now_samples, frame.samples)) {
            if (!ring.push(frame)) {
              underruns.fetch_add(1, std::memory_order_relaxed);
            }
            if (jbuf.finished()) {
              break;
            }
          }
          done = jbuf.finished();
        }
        if (done) {
          playout_done.store(true, std::memory_order_release);
          break;
        }
      }
    });

    // Main thread: drains the ring into the accumulation buffer. This is
    // where allocation and (eventually) file I/O are allowed to happen --
    // deliberately kept off the two time-critical threads above.
    std::vector<int16_t> samples;
    samples.reserve(static_cast<size_t>(rtp::audio::kSampleRateHz) * 60 * 10);
    AudioFrame frame;
    for (;;) {
      if (ring.pop(frame)) {
        samples.insert(samples.end(), frame.samples, frame.samples + rtp::audio::kFrameSamples);
        continue;
      }
      if (!playout_done.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        continue;
      }
      // playout_done is true (an acquire-load, so every push the playout
      // thread made before setting it is now visible here): one final pop
      // attempt catches anything pushed in the race between our last
      // failed pop and the flag being set.
      if (!ring.pop(frame)) {
        break;
      }
      samples.insert(samples.end(), frame.samples, frame.samples + rtp::audio::kFrameSamples);
    }

    receive_thread.join();
    playout_thread.join();

    rtp::wav::write(output_path, rtp::audio::kSampleRateHz, samples);

    rtp::jitter::Counts counts;
    int target_depth_ms;
    std::vector<uint32_t> latency_ms;
    std::vector<int> depth_history_ms;
    {
      std::lock_guard<std::mutex> lock(jbuf_mutex);
      counts = jbuf.counts();
      target_depth_ms = jbuf.target_depth_ms();
      if (collect_stats) {
        latency_ms = jbuf.latency_samples_ms();
        depth_history_ms = jbuf.target_depth_history_ms();
      }
    }
    std::fprintf(stderr,
                 "wrote %zu samples to %s (target_depth_ms=%d received=%llu lost=%llu "
                 "late_dropped=%llu reordered=%llu concealed=%llu duplicate=%llu "
                 "malformed_dropped=%zu underruns=%llu)\n",
                 samples.size(), output_path.c_str(), target_depth_ms,
                 static_cast<unsigned long long>(counts.received),
                 static_cast<unsigned long long>(counts.lost),
                 static_cast<unsigned long long>(counts.late_dropped),
                 static_cast<unsigned long long>(counts.reordered),
                 static_cast<unsigned long long>(counts.concealed),
                 static_cast<unsigned long long>(counts.duplicate), malformed_dropped,
                 static_cast<unsigned long long>(underruns.load()));

    if (collect_stats) {
      std::fprintf(stderr,
                   "STATS latency_median_ms=%.1f latency_p95_ms=%.1f depth_mean_ms=%.1f "
                   "depth_p95_ms=%.1f\n",
                   percentile(latency_ms, 0.5), percentile(latency_ms, 0.95),
                   mean_of(depth_history_ms), percentile_int(depth_history_ms, 0.95));
    }
  } catch (const std::exception& e) {
    std::fprintf(stderr, "%s: %s\n", argv[0], e.what());
    return 1;
  }

  return 0;
}
