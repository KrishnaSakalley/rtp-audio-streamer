#pragma once

#include <cstdint>

namespace rtp::stats {

// Tracks arrival order in RFC 3550's wrap-around sequence space (16-bit,
// wraps at 65535 -> 0). A signed 16-bit delta orders sequence numbers
// correctly across that wrap; `if (seq > last_seq)` does not -- it misreads
// the wrap as a massive jump backward.
class SequenceTracker {
 public:
  struct Counts {
    uint64_t received = 0;
    uint64_t gaps = 0;       // missing sequence numbers inferred from forward jumps
    uint64_t reordered = 0;  // packets that arrived behind the highest seen so far
  };

  void on_packet(uint16_t sequence_number);
  const Counts& counts() const { return counts_; }

 private:
  bool has_last_ = false;
  uint16_t last_sequence_ = 0;
  Counts counts_;
};

}  // namespace rtp::stats
