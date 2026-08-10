#include "rtp/ring_buffer.hpp"
#include "test_util.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <thread>

namespace {

// Single-threaded correctness: fill, drain, and wrap around the ring
// several times over, checking FIFO order and full/empty detection.
void test_basic_correctness() {
  rtp::ring::SpscRingBuffer<int, 4> ring;  // usable capacity is 3 (one slot sacrificed)

  int value = -1;
  RTP_CHECK(!ring.pop(value));  // empty

  RTP_CHECK(ring.push(1));
  RTP_CHECK(ring.push(2));
  RTP_CHECK(ring.push(3));
  RTP_CHECK(!ring.push(4));  // full

  RTP_CHECK(ring.pop(value) && value == 1);
  RTP_CHECK(ring.pop(value) && value == 2);
  RTP_CHECK(ring.push(4));  // room again after draining two
  RTP_CHECK(ring.push(5));
  RTP_CHECK(!ring.push(6));  // full again

  RTP_CHECK(ring.pop(value) && value == 3);
  RTP_CHECK(ring.pop(value) && value == 4);
  RTP_CHECK(ring.pop(value) && value == 5);
  RTP_CHECK(!ring.pop(value));  // empty again

  // Wrap the index space around multiple times to exercise the mask math.
  for (int i = 0; i < 100; ++i) {
    RTP_CHECK(ring.push(i));
    RTP_CHECK(ring.pop(value) && value == i);
  }
}

// SPSC stress test (PLAN.md Phase 6 GATE): two real threads move 10M items
// through the ring; the consumer checks strict FIFO order, proving no item
// is lost, duplicated, or reordered under genuine concurrent contention.
void test_spsc_stress() {
  constexpr size_t kCapacity = 1024;  // power of two
  constexpr uint64_t kItemCount = 10'000'000;

  rtp::ring::SpscRingBuffer<uint64_t, kCapacity> ring;
  std::atomic<bool> producer_done{false};

  std::thread producer([&]() {
    for (uint64_t i = 0; i < kItemCount; ++i) {
      while (!ring.push(i)) {
        std::this_thread::yield();  // full: back off and retry
      }
    }
    producer_done.store(true, std::memory_order_release);
  });

  uint64_t expected = 0;
  std::thread consumer([&]() {
    uint64_t value = 0;
    while (expected < kItemCount) {
      if (ring.pop(value)) {
        if (value != expected) {
          std::fprintf(stderr, "CHECK failed: expected %llu got %llu at position %llu\n",
                       static_cast<unsigned long long>(expected),
                       static_cast<unsigned long long>(value),
                       static_cast<unsigned long long>(expected));
          std::exit(1);
        }
        ++expected;
      } else {
        std::this_thread::yield();  // empty: back off and retry
      }
    }
  });

  producer.join();
  consumer.join();

  RTP_CHECK(producer_done.load(std::memory_order_acquire));
  RTP_CHECK(expected == kItemCount);
}

}  // namespace

int main() {
  test_basic_correctness();
  test_spsc_stress();
  std::puts("ring_buffer_test OK");
  return 0;
}
