#include "rtp/codec.hpp"

namespace rtp::codec {

namespace {

// ITU-T G.711 mu-law constants. BIAS=132 is the standard constant for
// companding a full 16-bit magnitude directly; it is 4x the "bias 33"
// figure sometimes quoted, because that figure describes an equivalent
// algorithm operating on a pre-shifted 14-bit magnitude instead.
constexpr int32_t kUlawBias = 132;
constexpr int32_t kUlawClip = 32635;

}  // namespace

uint8_t linear_to_ulaw(int16_t sample) {
  uint8_t sign = 0;
  int32_t magnitude = sample;
  if (magnitude < 0) {
    sign = 0x80;
    magnitude = -magnitude;
  }
  if (magnitude > kUlawClip) {
    magnitude = kUlawClip;
  }
  magnitude += kUlawBias;

  // Segment (3-bit exponent): position of the highest set bit in the biased
  // magnitude, which always falls between bit 7 (smallest segment) and
  // bit 14 (largest) -- exactly 8 segments.
  uint8_t exponent = 7;
  for (int32_t mask = 0x4000; (magnitude & mask) == 0 && exponent > 0; mask >>= 1) {
    --exponent;
  }

  uint8_t mantissa = static_cast<uint8_t>((magnitude >> (exponent + 3)) & 0x0F);
  uint8_t ulaw = static_cast<uint8_t>(sign | (exponent << 4) | mantissa);
  return static_cast<uint8_t>(~ulaw);  // inverted output, per ITU-T G.711
}

int16_t ulaw_to_linear(uint8_t code) {
  code = static_cast<uint8_t>(~code);
  uint8_t sign = code & 0x80;
  uint8_t exponent = (code >> 4) & 0x07;
  uint8_t mantissa = code & 0x0F;

  int32_t magnitude = ((static_cast<int32_t>(mantissa) << 3) + kUlawBias) << exponent;
  magnitude -= kUlawBias;

  return static_cast<int16_t>(sign != 0 ? -magnitude : magnitude);
}

void ulaw_encode_frame(const int16_t* pcm, size_t num_samples, uint8_t* out) {
  for (size_t i = 0; i < num_samples; ++i) {
    out[i] = linear_to_ulaw(pcm[i]);
  }
}

void ulaw_decode_frame(const uint8_t* codes, size_t num_samples, int16_t* out) {
  for (size_t i = 0; i < num_samples; ++i) {
    out[i] = ulaw_to_linear(codes[i]);
  }
}

}  // namespace rtp::codec
