#pragma once

#include <array>
#include <atomic>
#include <cstddef>

namespace rtp::ring {

// Bounded single-producer/single-consumer lock-free queue. Exactly one
// thread may call push(), a different (but only ever that one) thread may
// call pop(); calling either from more than one thread concurrently is
// undefined. Capacity must be a power of two so the wrap-around index can
// be computed with a bitmask instead of a modulo -- a modulo by a
// non-power-of-two is a division, which is one of the more expensive
// things to put on a path this hot.
template <typename T, size_t Capacity>
class SpscRingBuffer {
  static_assert(Capacity >= 2, "capacity must be at least 2");
  static_assert((Capacity & (Capacity - 1)) == 0, "capacity must be a power of two");

 public:
  // Returns false if the buffer is full (caller should retry or drop).
  bool push(const T& item) {
    size_t head = head_.load(std::memory_order_relaxed);
    size_t next = (head + 1) & kMask;
    // Acquire tail_: must observe every slot the consumer has already
    // freed by advancing it, or push() could overwrite a slot pop() hasn't
    // read yet.
    if (next == tail_.load(std::memory_order_acquire)) {
      return false;  // full -- one slot is always left empty to
                      // distinguish full from empty without a separate count
    }
    buffer_[head] = item;
    // Release head_: publishes this slot's write so that once the
    // consumer's acquire-load of head_ observes it, the item it reads back
    // is guaranteed to be the one just written, not a stale or torn value.
    head_.store(next, std::memory_order_release);
    return true;
  }

  // Returns false if the buffer is empty.
  bool pop(T& out) {
    size_t tail = tail_.load(std::memory_order_relaxed);
    if (tail == head_.load(std::memory_order_acquire)) {
      return false;  // empty
    }
    out = buffer_[tail];
    tail_.store((tail + 1) & kMask, std::memory_order_release);
    return true;
  }

 private:
  static constexpr size_t kMask = Capacity - 1;

  std::array<T, Capacity> buffer_{};

  // The producer only ever writes head_ (and reads tail_); the consumer
  // only ever writes tail_ (and reads head_). Without alignas(64) padding
  // between them, the two atomics would very likely share a 64-byte cache
  // line, and every push/pop would bounce that line between the two
  // threads' cores (false sharing) even though the threads never touch the
  // same logical data -- this is the whole reason a "lock-free" queue can
  // still be slower than a mutex if you get the layout wrong.
  alignas(64) std::atomic<size_t> head_{0};
  alignas(64) std::atomic<size_t> tail_{0};
};

}  // namespace rtp::ring
