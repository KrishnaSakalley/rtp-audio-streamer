#include "rtp/jitter_buffer.hpp"
#include "rtp/rtp_packet.hpp"
#include "test_util.hpp"

#include <cstdio>
#include <cstring>
#include <vector>

namespace {

using rtp::audio::kFrameSamples;

std::vector<uint8_t> make_pcm_payload(int16_t value) {
  std::vector<uint8_t> payload(kFrameSamples * sizeof(int16_t));
  for (size_t i = 0; i < kFrameSamples; ++i) {
    int16_t sample = value;
    std::memcpy(payload.data() + i * sizeof(int16_t), &sample, sizeof(sample));
  }
  return payload;
}

bool frame_is_constant(const int16_t* frame, int16_t value) {
  for (size_t i = 0; i < kFrameSamples; ++i) {
    if (frame[i] != value) return false;
  }
  return true;
}

}  // namespace

int main() {
  // Empty stream: mark ended immediately, nothing to drain.
  {
    rtp::jitter::JitterBuffer jb;
    jb.mark_stream_ended();
    RTP_CHECK(jb.finished());
    int16_t out[kFrameSamples];
    RTP_CHECK(!jb.try_pull_due_frame(0, out));
  }

  // Deadline gating: a pushed frame isn't due until now_samples reaches it.
  {
    rtp::jitter::JitterBuffer jb;
    auto payload = make_pcm_payload(111);
    jb.push(1000, 0, rtp::packet::kPayloadTypePcm, payload.data(), payload.size(), /*arrival=*/0);
    int16_t out[kFrameSamples];
    RTP_CHECK(!jb.try_pull_due_frame(0, out));  // target depth hasn't elapsed yet
    uint32_t far_future = 100u * rtp::audio::kSampleRateHz;  // 100s: certainly past deadline
    RTP_CHECK(jb.try_pull_due_frame(far_future, out));
    RTP_CHECK(frame_is_constant(out, 111));
  }

  // Must not speculate past the frontier of what's actually been received
  // while the stream is still active: a real-time deadline alone can't
  // distinguish "lost" from "not sent yet, transmission still in progress".
  // Regression test for a bug where try_pull_due_frame(), once real time
  // caught up to a frame's nominal deadline, would conceal indefinitely far
  // past the last packet ever pushed, because only mark_stream_ended()
  // capped playout via finished() -- nothing stopped it beforehand.
  {
    rtp::jitter::JitterBuffer jb;
    auto payload = make_pcm_payload(42);
    jb.push(0, 0, rtp::packet::kPayloadTypePcm, payload.data(), payload.size(), 0);
    int16_t out[kFrameSamples];
    uint32_t far_future = 100u * rtp::audio::kSampleRateHz;  // 100s: past every real deadline

    RTP_CHECK(jb.try_pull_due_frame(far_future, out));  // seq 0: the one real frame
    RTP_CHECK(frame_is_constant(out, 42));

    // Nothing beyond seq 0 was ever received, and the stream hasn't been
    // marked ended -- must wait, not conceal, no matter how far "now" is.
    RTP_CHECK(!jb.try_pull_due_frame(far_future, out));
    RTP_CHECK(!jb.try_pull_due_frame(far_future, out));
    RTP_CHECK(jb.counts().concealed == 0);
    RTP_CHECK(jb.counts().lost == 0);

    // Only once the stream is explicitly ended does draining resume (and
    // immediately finish, since there's nothing left beyond seq 0).
    jb.mark_stream_ended();
    RTP_CHECK(!jb.try_pull_due_frame(far_future, out));
    RTP_CHECK(jb.finished());
  }

  // Normal in-order playback: N frames, no loss, decoded content matches.
  {
    rtp::jitter::JitterBuffer jb;
    const int kNumFrames = 5;
    uint32_t timestamp = 0;
    for (int i = 0; i < kNumFrames; ++i) {
      auto payload = make_pcm_payload(static_cast<int16_t>((i + 1) * 100));
      jb.push(static_cast<uint16_t>(i), timestamp, rtp::packet::kPayloadTypePcm, payload.data(),
              payload.size(), /*arrival=*/timestamp);
      timestamp += kFrameSamples;
    }
    jb.mark_stream_ended();
    int16_t out[kFrameSamples];
    for (int i = 0; i < kNumFrames; ++i) {
      RTP_CHECK(jb.try_pull_due_frame(0xFFFFFFFFu, out));
      RTP_CHECK(frame_is_constant(out, static_cast<int16_t>((i + 1) * 100)));
    }
    RTP_CHECK(jb.finished());
    const auto& c = jb.counts();
    RTP_CHECK(c.received == kNumFrames);
    RTP_CHECK(c.lost == 0);
    RTP_CHECK(c.concealed == 0);
  }

  // Packet loss and PLC: skip one sequence number, expect a faded repeat of
  // the previous real frame, then playback resumes normally.
  {
    rtp::jitter::JitterBuffer jb;
    uint32_t ts = 0;
    auto payload0 = make_pcm_payload(1000);
    jb.push(0, ts, rtp::packet::kPayloadTypePcm, payload0.data(), payload0.size(), ts);
    ts += kFrameSamples;
    // seq 1 is lost -- never pushed.
    ts += kFrameSamples;
    auto payload2 = make_pcm_payload(2000);
    jb.push(2, ts, rtp::packet::kPayloadTypePcm, payload2.data(), payload2.size(), ts);

    jb.mark_stream_ended();
    int16_t out[kFrameSamples];

    RTP_CHECK(jb.try_pull_due_frame(0xFFFFFFFFu, out));
    RTP_CHECK(frame_is_constant(out, 1000));

    RTP_CHECK(jb.try_pull_due_frame(0xFFFFFFFFu, out));  // concealed frame for seq 1
    // Faded repeat of 1000 at -6dB: 1000 * 10^(-6/20) ~= 501.
    RTP_CHECK(out[0] > 400 && out[0] < 600);
    RTP_CHECK(out[0] == out[kFrameSamples - 1]);  // uniform fade across the frame

    RTP_CHECK(jb.try_pull_due_frame(0xFFFFFFFFu, out));
    RTP_CHECK(frame_is_constant(out, 2000));

    const auto& c = jb.counts();
    RTP_CHECK(c.lost == 1);
    RTP_CHECK(c.concealed == 1);
  }

  // Four consecutive losses: first 3 fade progressively quieter, the 4th+
  // is silence (PLAN.md §4's "cap at 3 concealments, then silence"). seq5
  // has to arrive so the buffer can infer seq1..4 were ever expected --
  // it can't conceal a gap it has no evidence of.
  {
    rtp::jitter::JitterBuffer jb;
    auto payload0 = make_pcm_payload(2000);
    auto payload5 = make_pcm_payload(6000);
    jb.push(0, 0, rtp::packet::kPayloadTypePcm, payload0.data(), payload0.size(), 0);
    jb.push(5, 5 * kFrameSamples, rtp::packet::kPayloadTypePcm, payload5.data(), payload5.size(),
            5 * kFrameSamples);
    // seq 1..4 are all lost.
    jb.mark_stream_ended();

    int16_t out[kFrameSamples];
    RTP_CHECK(jb.try_pull_due_frame(0xFFFFFFFFu, out));  // seq 0, real
    RTP_CHECK(frame_is_constant(out, 2000));

    int16_t prev_magnitude = 2000;
    for (int i = 0; i < 3; ++i) {
      RTP_CHECK(jb.try_pull_due_frame(0xFFFFFFFFu, out));
      RTP_CHECK(out[0] > 0);
      RTP_CHECK(out[0] < prev_magnitude);  // strictly quieter each concealed frame
      prev_magnitude = out[0];
    }
    // 4th consecutive concealment (seq 4): past the cap, must be silence.
    RTP_CHECK(jb.try_pull_due_frame(0xFFFFFFFFu, out));
    RTP_CHECK(frame_is_constant(out, 0));

    // seq 5: a real frame arrives again and playback recovers cleanly.
    RTP_CHECK(jb.try_pull_due_frame(0xFFFFFFFFu, out));
    RTP_CHECK(frame_is_constant(out, 6000));

    RTP_CHECK(jb.counts().concealed == 4);
    RTP_CHECK(jb.counts().lost == 4);
    RTP_CHECK(jb.finished());
  }

  // Duplicate: the same sequence number pushed twice before it's played.
  {
    rtp::jitter::JitterBuffer jb;
    auto payload = make_pcm_payload(555);
    jb.push(0, 0, rtp::packet::kPayloadTypePcm, payload.data(), payload.size(), 0);
    jb.push(0, 0, rtp::packet::kPayloadTypePcm, payload.data(), payload.size(), 0);
    RTP_CHECK(jb.counts().duplicate == 1);
    RTP_CHECK(jb.counts().received == 2);
  }

  // Reordered and too late: the playout cursor bootstraps on the first
  // packet *received* (seq 5), so a seq 3 that arrives afterward is behind
  // the cursor -- both reordered (arrived behind the highest seen) and
  // late-dropped (its slot is already behind playout).
  {
    rtp::jitter::JitterBuffer jb;
    auto payload5 = make_pcm_payload(500);
    auto payload3 = make_pcm_payload(300);
    jb.push(5, 5 * kFrameSamples, rtp::packet::kPayloadTypePcm, payload5.data(), payload5.size(),
            5 * kFrameSamples);
    jb.push(3, 3 * kFrameSamples, rtp::packet::kPayloadTypePcm, payload3.data(), payload3.size(),
            3 * kFrameSamples);
    RTP_CHECK(jb.counts().reordered == 1);
    RTP_CHECK(jb.counts().late_dropped == 1);
  }

  // Reordered, but still in-window and still played: bootstrap on seq 10,
  // then seq 12 arrives before seq 11 (both ahead of the playout cursor).
  {
    rtp::jitter::JitterBuffer jb;
    auto p10 = make_pcm_payload(10);
    auto p11 = make_pcm_payload(11);
    auto p12 = make_pcm_payload(12);
    jb.push(10, 0, rtp::packet::kPayloadTypePcm, p10.data(), p10.size(), 0);
    jb.push(12, 2 * kFrameSamples, rtp::packet::kPayloadTypePcm, p12.data(), p12.size(),
            2 * kFrameSamples);
    jb.push(11, kFrameSamples, rtp::packet::kPayloadTypePcm, p11.data(), p11.size(), kFrameSamples);

    RTP_CHECK(jb.counts().reordered == 1);     // seq 11 arrived behind seq 12
    RTP_CHECK(jb.counts().late_dropped == 0);  // but still within the window

    jb.mark_stream_ended();
    int16_t out[kFrameSamples];
    RTP_CHECK(jb.try_pull_due_frame(0xFFFFFFFFu, out));
    RTP_CHECK(frame_is_constant(out, 10));
    RTP_CHECK(jb.try_pull_due_frame(0xFFFFFFFFu, out));
    RTP_CHECK(frame_is_constant(out, 11));  // reordered arrival still played in the right slot
    RTP_CHECK(jb.try_pull_due_frame(0xFFFFFFFFu, out));
    RTP_CHECK(frame_is_constant(out, 12));
  }

  // Buffer-full / burst arrival: push far more packets than kCapacity back
  // to back with no draining in between. Packets beyond the ring's reach
  // are dropped cleanly at push time (not stored), but the buffer still
  // knows they were expected (from their sequence numbers), so at drain
  // time their slots come up as lost/concealed rather than being skipped.
  {
    rtp::jitter::JitterBuffer jb;
    auto payload = make_pcm_payload(7);
    const uint16_t total = static_cast<uint16_t>(rtp::jitter::kCapacity + 10);
    for (uint16_t seq = 0; seq < total; ++seq) {
      jb.push(seq, static_cast<uint32_t>(seq) * kFrameSamples, rtp::packet::kPayloadTypePcm,
              payload.data(), payload.size(), static_cast<uint32_t>(seq) * kFrameSamples);
    }
    RTP_CHECK(jb.counts().received == total);
    RTP_CHECK(jb.counts().late_dropped == 10);  // the 10 that didn't fit in the ring

    jb.mark_stream_ended();
    int16_t out[kFrameSamples];
    size_t played = 0;
    while (jb.try_pull_due_frame(0xFFFFFFFFu, out)) {
      ++played;
    }
    RTP_CHECK(played == total);           // every sequence position gets *some* output
    RTP_CHECK(jb.counts().lost == 10);    // the 10 dropped-at-push slots come up empty
    RTP_CHECK(jb.counts().concealed == 10);
  }

  // Sequence number wrap: stream crosses 65535 -> 0 mid-flight.
  {
    rtp::jitter::JitterBuffer jb;
    auto pa = make_pcm_payload(1);
    auto pb = make_pcm_payload(2);
    auto pc = make_pcm_payload(3);
    jb.push(65534, 0, rtp::packet::kPayloadTypePcm, pa.data(), pa.size(), 0);
    jb.push(65535, kFrameSamples, rtp::packet::kPayloadTypePcm, pb.data(), pb.size(), kFrameSamples);
    jb.push(0, 2 * kFrameSamples, rtp::packet::kPayloadTypePcm, pc.data(), pc.size(),
            2 * kFrameSamples);

    RTP_CHECK(jb.counts().reordered == 0);  // strictly increasing across the wrap
    RTP_CHECK(jb.counts().late_dropped == 0);

    jb.mark_stream_ended();
    int16_t out[kFrameSamples];
    RTP_CHECK(jb.try_pull_due_frame(0xFFFFFFFFu, out));
    RTP_CHECK(frame_is_constant(out, 1));
    RTP_CHECK(jb.try_pull_due_frame(0xFFFFFFFFu, out));
    RTP_CHECK(frame_is_constant(out, 2));
    RTP_CHECK(jb.try_pull_due_frame(0xFFFFFFFFu, out));
    RTP_CHECK(frame_is_constant(out, 3));
    RTP_CHECK(jb.finished());
  }

  std::puts("jitter_buffer_test OK");
  return 0;
}
