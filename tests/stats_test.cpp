#include "rtp/stats.hpp"
#include "test_util.hpp"

#include <cstdio>

int main() {
  // In-order stream: no gaps, no reorders.
  {
    rtp::stats::SequenceTracker t;
    for (uint16_t seq = 100; seq < 110; ++seq) {
      t.on_packet(seq);
    }
    const auto& c = t.counts();
    RTP_CHECK(c.received == 10);
    RTP_CHECK(c.gaps == 0);
    RTP_CHECK(c.reordered == 0);
  }

  // A forward jump of N is N-1 missing packets.
  {
    rtp::stats::SequenceTracker t;
    t.on_packet(5);
    t.on_packet(10);  // 6,7,8,9 missing
    const auto& c = t.counts();
    RTP_CHECK(c.received == 2);
    RTP_CHECK(c.gaps == 4);
    RTP_CHECK(c.reordered == 0);
  }

  // A packet that arrives behind the highest seen sequence number.
  {
    rtp::stats::SequenceTracker t;
    t.on_packet(5);
    t.on_packet(6);
    t.on_packet(4);  // arrived late, behind 6
    const auto& c = t.counts();
    RTP_CHECK(c.received == 3);
    RTP_CHECK(c.reordered == 1);
  }

  // The critical edge case: sequence wraps 65535 -> 0. A naive
  // `seq > last_seq` comparison would misread this as a huge jump backward
  // and either explode the gap count or flag every subsequent packet as
  // reordered. The signed-delta tracker must see it as simple in-order flow.
  {
    rtp::stats::SequenceTracker t;
    t.on_packet(65533);
    t.on_packet(65534);
    t.on_packet(65535);
    t.on_packet(0);
    t.on_packet(1);
    t.on_packet(2);
    const auto& c = t.counts();
    RTP_CHECK(c.received == 6);
    RTP_CHECK(c.gaps == 0);
    RTP_CHECK(c.reordered == 0);
  }

  // Wrap combined with a genuine gap: 65535 -> 2 should register exactly
  // two missing packets (0 and 1), not garbage from unsigned underflow.
  {
    rtp::stats::SequenceTracker t;
    t.on_packet(65535);
    t.on_packet(2);
    const auto& c = t.counts();
    RTP_CHECK(c.gaps == 2);
    RTP_CHECK(c.reordered == 0);
  }

  // Wrap combined with reorder: after wrapping to 1, a late packet from
  // just before the wrap (65535) must be seen as reordered, not as another
  // huge forward jump.
  {
    rtp::stats::SequenceTracker t;
    t.on_packet(0);
    t.on_packet(1);
    t.on_packet(65535);  // late arrival from before the wrap
    const auto& c = t.counts();
    RTP_CHECK(c.reordered == 1);
  }

  // Duplicate packet: counted as received, but not a gap or a reorder.
  {
    rtp::stats::SequenceTracker t;
    t.on_packet(7);
    t.on_packet(7);
    const auto& c = t.counts();
    RTP_CHECK(c.received == 2);
    RTP_CHECK(c.gaps == 0);
    RTP_CHECK(c.reordered == 0);
  }

  std::puts("stats_test OK");
  return 0;
}
