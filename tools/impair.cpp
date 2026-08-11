// UDP middlebox: sits between rtp_sender and rtp_receiver, receiving on
// --listen and forwarding to --forward, applying probabilistic packet loss,
// reordering, jitter (random delay), and duplication. The RNG is seeded
// (--seed, default fixed) so a run can be reproduced exactly for debugging.

#include "rtp/udp_socket.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

struct Config {
  uint16_t listen_port = 5004;
  uint16_t forward_port = 5006;
  std::string forward_host = "127.0.0.1";
  double loss = 0.0;
  double reorder = 0.0;
  int jitter_ms = 0;
  double dup = 0.0;
  unsigned seed = 42;
  int idle_timeout_ms = 1000;
};

void print_usage(const char* argv0) {
  std::fprintf(stderr,
               "usage: %s --listen L --forward F [--forward-host H] [--loss P] "
               "[--reorder P] [--jitter MS] [--dup P] [--seed N] [--idle-timeout-ms N]\n",
               argv0);
}

bool parse_args(int argc, char** argv, Config& cfg) {
  // argv[++i] (an expression, not a bare statement) matches the idiom used
  // in sender.cpp/receiver.cpp -- Clang's -Wfor-loop-analysis flags a naked
  // `++i;` statement in the loop body as a suspicious double-increment even
  // when (as here) it's intentional, but doesn't flag the same increment
  // folded into an expression.
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (i + 1 >= argc) {
      return false;
    }
    if (arg == "--listen") {
      cfg.listen_port = static_cast<uint16_t>(std::atoi(argv[++i]));
    } else if (arg == "--forward") {
      cfg.forward_port = static_cast<uint16_t>(std::atoi(argv[++i]));
    } else if (arg == "--forward-host") {
      cfg.forward_host = argv[++i];
    } else if (arg == "--loss") {
      cfg.loss = std::atof(argv[++i]);
    } else if (arg == "--reorder") {
      cfg.reorder = std::atof(argv[++i]);
    } else if (arg == "--jitter") {
      cfg.jitter_ms = std::atoi(argv[++i]);
    } else if (arg == "--dup") {
      cfg.dup = std::atof(argv[++i]);
    } else if (arg == "--seed") {
      cfg.seed = static_cast<unsigned>(std::atoi(argv[++i]));
    } else if (arg == "--idle-timeout-ms") {
      cfg.idle_timeout_ms = std::atoi(argv[++i]);
    } else {
      return false;
    }
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  Config cfg;
  if (!parse_args(argc, argv, cfg)) {
    print_usage(argv[0]);
    return 1;
  }

  try {
    rtp::net::UdpSocket listen_socket;
    listen_socket.bind_to(cfg.listen_port);
    listen_socket.set_receive_timeout(cfg.idle_timeout_ms);

    std::mt19937 rng(cfg.seed);
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    std::uniform_int_distribution<int> jitter_dist(0, cfg.jitter_ms > 0 ? cfg.jitter_ms : 0);

    // Extra hold applied on top of jitter when a packet is chosen for
    // reordering, so it reliably lands behind packets sent right after it
    // instead of only incidentally (plain jitter can reorder too, but
    // --reorder is meant to be a deliberate, independent knob).
    constexpr int kReorderExtraDelayMs = 40;

    std::vector<std::thread> in_flight;
    uint64_t received = 0, dropped = 0, duplicated = 0, reordered = 0, forwarded = 0;

    uint8_t buffer[rtp::net::kMaxDatagramBytes];
    for (;;) {
      ssize_t n = listen_socket.receive(buffer, sizeof(buffer));
      if (n < 0) {
        break;  // idle timeout: sender is done
      }
      received += 1;

      if (unit(rng) < cfg.loss) {
        dropped += 1;
        continue;
      }

      std::vector<uint8_t> payload(buffer, buffer + n);
      int delay_ms = cfg.jitter_ms > 0 ? jitter_dist(rng) : 0;
      if (unit(rng) < cfg.reorder) {
        delay_ms += kReorderExtraDelayMs;
        reordered += 1;
      }
      bool is_dup = unit(rng) < cfg.dup;
      if (is_dup) {
        duplicated += 1;
      }
      int send_count = is_dup ? 2 : 1;
      forwarded += static_cast<uint64_t>(send_count);

      if (delay_ms <= 0) {
        // No delay: send synchronously on the main thread so packets that
        // aren't being deliberately impaired keep strict receive order.
        // Spawning a thread even for a zero-delay send would race against
        // adjacent packets' threads and introduce reordering nobody asked
        // for -- exactly the bug that broke the loss=0.0 passthrough case
        // the first time this was written.
        rtp::net::UdpSocket send_socket;
        for (int copy = 0; copy < send_count; ++copy) {
          send_socket.send_to(payload.data(), payload.size(), cfg.forward_host, cfg.forward_port);
        }
      } else {
        // Deliberately delayed (jitter and/or reorder): a background
        // thread holds it without blocking the receive loop. This is a
        // dev/test tool, not the real-time audio path, so thread-per-send
        // is an acceptable simplicity trade-off here.
        in_flight.emplace_back([host = cfg.forward_host, port = cfg.forward_port, payload,
                                 delay_ms, send_count]() {
          std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
          rtp::net::UdpSocket send_socket;
          for (int copy = 0; copy < send_count; ++copy) {
            send_socket.send_to(payload.data(), payload.size(), host, port);
          }
        });
      }
    }

    for (auto& t : in_flight) {
      t.join();
    }

    std::fprintf(
        stderr,
        "impair: received=%llu forwarded=%llu dropped=%llu duplicated=%llu reordered=%llu\n",
        static_cast<unsigned long long>(received), static_cast<unsigned long long>(forwarded),
        static_cast<unsigned long long>(dropped), static_cast<unsigned long long>(duplicated),
        static_cast<unsigned long long>(reordered));
  } catch (const std::exception& e) {
    std::fprintf(stderr, "%s: %s\n", argv[0], e.what());
    return 1;
  }

  return 0;
}
