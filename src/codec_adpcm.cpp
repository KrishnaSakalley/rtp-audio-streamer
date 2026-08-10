#include "rtp/codec.hpp"

#include <array>

namespace rtp::codec {

namespace {

// IMA ADPCM reference step-size table (89 entries) and index-adjustment
// table (16 entries) -- fixed constants defined by the Interactive
// Multimedia Association ADPCM specification. These are specification data,
// not original expression: any compliant IMA ADPCM codec uses these exact
// values.
constexpr std::array<int16_t, 89> kStepTable = {
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,
    19,    21,    23,    25,    28,    31,    34,    37,    41,    45,
    50,    55,    60,    66,    73,    80,    88,    97,    107,   118,
    130,   143,   157,   173,   190,   209,   230,   253,   279,   307,
    337,   371,   408,   449,   494,   544,   598,   658,   724,   796,
    876,   963,   1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
    2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,
    5894,  6484,  7132,  7845,  8630,  9493,  10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};

constexpr std::array<int8_t, 16> kIndexTable = {-1, -1, -1, -1, 2, 4, 6, 8,
                                                 -1, -1, -1, -1, 2, 4, 6, 8};

int clamp_step_index(int index) {
  if (index < 0) return 0;
  if (index > 88) return 88;
  return index;
}

int32_t clamp_predictor(int32_t value) {
  if (value < -32768) return -32768;
  if (value > 32767) return 32767;
  return value;
}

uint8_t encode_sample(int16_t sample, AdpcmState& state) {
  int32_t step = kStepTable[state.step_index];
  int32_t diff = static_cast<int32_t>(sample) - state.predictor;

  uint8_t code = 0;
  if (diff < 0) {
    code = 0x08;
    diff = -diff;
  }

  int32_t diffq = step >> 3;
  int32_t half_step = step;
  if (diff >= half_step) {
    code |= 0x04;
    diff -= half_step;
    diffq += half_step;
  }
  half_step >>= 1;
  if (diff >= half_step) {
    code |= 0x02;
    diff -= half_step;
    diffq += half_step;
  }
  half_step >>= 1;
  if (diff >= half_step) {
    code |= 0x01;
    diffq += half_step;
  }

  int32_t predictor = state.predictor + ((code & 0x08) ? -diffq : diffq);
  state.predictor = static_cast<int16_t>(clamp_predictor(predictor));
  state.step_index = static_cast<uint8_t>(clamp_step_index(state.step_index + kIndexTable[code]));

  return code;
}

int16_t decode_sample(uint8_t code, AdpcmState& state) {
  int32_t step = kStepTable[state.step_index];

  int32_t diffq = step >> 3;
  if (code & 0x04) diffq += step;
  if (code & 0x02) diffq += step >> 1;
  if (code & 0x01) diffq += step >> 2;

  int32_t predictor = state.predictor + ((code & 0x08) ? -diffq : diffq);
  state.predictor = static_cast<int16_t>(clamp_predictor(predictor));
  state.step_index = static_cast<uint8_t>(clamp_step_index(state.step_index + kIndexTable[code]));

  return state.predictor;
}

}  // namespace

void adpcm_encode_frame(const int16_t* pcm, size_t num_samples, AdpcmState& state, uint8_t* out) {
  for (size_t i = 0; i < num_samples; i += 2) {
    uint8_t low = encode_sample(pcm[i], state);
    uint8_t high = (i + 1 < num_samples) ? encode_sample(pcm[i + 1], state) : 0;
    out[i / 2] = static_cast<uint8_t>(low | (high << 4));
  }
}

void adpcm_decode_frame(const uint8_t* packed, size_t num_samples, AdpcmState& state, int16_t* out) {
  for (size_t i = 0; i < num_samples; i += 2) {
    uint8_t byte = packed[i / 2];
    out[i] = decode_sample(static_cast<uint8_t>(byte & 0x0F), state);
    if (i + 1 < num_samples) {
      out[i + 1] = decode_sample(static_cast<uint8_t>((byte >> 4) & 0x0F), state);
    }
  }
}

void pack_adpcm_state(const AdpcmState& state, uint8_t out[kAdpcmStateHeaderBytes]) {
  uint16_t predictor_bits = static_cast<uint16_t>(state.predictor);
  out[0] = static_cast<uint8_t>(predictor_bits >> 8);
  out[1] = static_cast<uint8_t>(predictor_bits & 0xFF);
  out[2] = state.step_index;
  out[3] = 0;  // reserved
}

AdpcmState unpack_adpcm_state(const uint8_t in[kAdpcmStateHeaderBytes]) {
  AdpcmState state;
  uint16_t predictor_bits = static_cast<uint16_t>((in[0] << 8) | in[1]);
  state.predictor = static_cast<int16_t>(predictor_bits);
  // Clamp: a corrupt or malicious packet could carry an out-of-range step
  // index, which would be an out-of-bounds read into kStepTable otherwise.
  state.step_index = static_cast<uint8_t>(in[2] > 88 ? 88 : in[2]);
  return state;
}

}  // namespace rtp::codec
