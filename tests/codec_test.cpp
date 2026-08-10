#include "rtp/codec.hpp"
#include "test_util.hpp"

#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <vector>

namespace {

int abs_diff(int16_t a, int16_t b) {
  return std::abs(static_cast<int>(a) - static_cast<int>(b));
}

}  // namespace

int main() {
  // mu-law: silence round-trips to (near) silence.
  {
    uint8_t code = rtp::codec::linear_to_ulaw(0);
    int16_t back = rtp::codec::ulaw_to_linear(code);
    RTP_CHECK(abs_diff(back, 0) < 40);
  }

  // mu-law: extremes clip and round-trip with the correct sign, within the
  // codec's known quantization error at the top segment.
  {
    int16_t max_back = rtp::codec::ulaw_to_linear(rtp::codec::linear_to_ulaw(32767));
    RTP_CHECK(max_back > 32000);
    int16_t min_back = rtp::codec::ulaw_to_linear(rtp::codec::linear_to_ulaw(-32768));
    RTP_CHECK(min_back < -32000);
  }

  // mu-law: encode is antisymmetric around zero -- decoding -x should be the
  // negation of decoding x, since only the sign bit differs.
  {
    int16_t values[] = {100, 1000, 5000, 20000};
    for (int16_t x : values) {
      int16_t pos = rtp::codec::ulaw_to_linear(rtp::codec::linear_to_ulaw(x));
      int16_t neg = rtp::codec::ulaw_to_linear(rtp::codec::linear_to_ulaw(static_cast<int16_t>(-x)));
      RTP_CHECK(pos == -neg);
    }
  }

  // mu-law: empty frame does nothing, doesn't crash.
  {
    rtp::codec::ulaw_encode_frame(nullptr, 0, nullptr);
    rtp::codec::ulaw_decode_frame(nullptr, 0, nullptr);
  }

  // ADPCM: a constant signal should track closely once the predictor adapts.
  {
    std::vector<int16_t> pcm(160, 5000);
    std::vector<uint8_t> packed(80);
    rtp::codec::AdpcmState enc_state;
    rtp::codec::adpcm_encode_frame(pcm.data(), pcm.size(), enc_state, packed.data());

    std::vector<int16_t> out(160);
    rtp::codec::AdpcmState dec_state;
    rtp::codec::adpcm_decode_frame(packed.data(), pcm.size(), dec_state, out.data());

    for (size_t i = 20; i < pcm.size(); ++i) {
      RTP_CHECK(abs_diff(out[i], pcm[i]) < 400);
    }
  }

  // ADPCM: per-packet state carry. A packet's own header state must be
  // sufficient to decode it correctly even if every prior packet was lost --
  // this is the mechanism PLAN.md requires so one loss doesn't desync the
  // rest of the stream. Encode two frames back-to-back (state threaded
  // through), then decode frame 2 using only the state captured after
  // frame 1, simulating frame 1 never having arrived.
  {
    std::vector<int16_t> frame1(160), frame2(160);
    for (size_t i = 0; i < 160; ++i) {
      frame1[i] = static_cast<int16_t>(3000.0 * std::sin(static_cast<double>(i) / 10.0));
      frame2[i] = static_cast<int16_t>(3000.0 * std::sin(static_cast<double>(i + 160) / 10.0));
    }

    rtp::codec::AdpcmState enc_state;
    std::vector<uint8_t> packed1(80), packed2(80);
    rtp::codec::adpcm_encode_frame(frame1.data(), 160, enc_state, packed1.data());
    rtp::codec::AdpcmState state_after_frame1 = enc_state;
    rtp::codec::adpcm_encode_frame(frame2.data(), 160, enc_state, packed2.data());

    // "Frame 1 was lost": decode frame 2 starting only from the state
    // recorded in its own header.
    std::vector<int16_t> decoded2(160);
    rtp::codec::AdpcmState dec_state = state_after_frame1;
    rtp::codec::adpcm_decode_frame(packed2.data(), 160, dec_state, decoded2.data());

    // Reference: a continuous session that did receive frame 1.
    rtp::codec::AdpcmState ref_state;
    std::vector<int16_t> discard1(160);
    rtp::codec::adpcm_decode_frame(packed1.data(), 160, ref_state, discard1.data());
    std::vector<int16_t> decoded2_ref(160);
    rtp::codec::adpcm_decode_frame(packed2.data(), 160, ref_state, decoded2_ref.data());

    for (size_t i = 0; i < 160; ++i) {
      RTP_CHECK(decoded2[i] == decoded2_ref[i]);
    }
  }

  // ADPCM: state header round-trips, and clamps an out-of-range step index
  // from a corrupt packet instead of indexing the step table out of bounds.
  {
    rtp::codec::AdpcmState state;
    state.predictor = -12345;
    state.step_index = 42;
    uint8_t header[rtp::codec::kAdpcmStateHeaderBytes];
    rtp::codec::pack_adpcm_state(state, header);
    rtp::codec::AdpcmState back = rtp::codec::unpack_adpcm_state(header);
    RTP_CHECK(back.predictor == -12345);
    RTP_CHECK(back.step_index == 42);

    uint8_t bad_header[rtp::codec::kAdpcmStateHeaderBytes] = {0, 0, 255, 0};
    rtp::codec::AdpcmState clamped = rtp::codec::unpack_adpcm_state(bad_header);
    RTP_CHECK(clamped.step_index == 88);
  }

  // ADPCM: empty frame does nothing, doesn't crash.
  {
    rtp::codec::AdpcmState state;
    rtp::codec::adpcm_encode_frame(nullptr, 0, state, nullptr);
    rtp::codec::adpcm_decode_frame(nullptr, 0, state, nullptr);
  }

  std::puts("codec_test OK");
  return 0;
}
