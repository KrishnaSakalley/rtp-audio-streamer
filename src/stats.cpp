#include "rtp/stats.hpp"

namespace rtp::stats {

void SequenceTracker::on_packet(uint16_t sequence_number) {
  counts_.received += 1;

  if (!has_last_) {
    has_last_ = true;
    last_sequence_ = sequence_number;
    return;
  }

  // Wraps correctly at 65535 -> 0 because the subtraction is done in
  // uint16_t (also wrapping) before the result is reinterpreted as signed.
  int16_t delta = static_cast<int16_t>(sequence_number - last_sequence_);

  if (delta > 1) {
    counts_.gaps += static_cast<uint64_t>(delta - 1);
    last_sequence_ = sequence_number;
  } else if (delta == 1) {
    last_sequence_ = sequence_number;
  } else if (delta == 0) {
    // Duplicate packet -- not a gap or a reorder; Phase 5 gives this its own
    // counter once the jitter buffer needs to act on it.
  } else {
    counts_.reordered += 1;
    // Do not move last_sequence_ backward on a reorder: the highest
    // sequence number seen stays the reference point for future deltas.
  }
}

}  // namespace rtp::stats
