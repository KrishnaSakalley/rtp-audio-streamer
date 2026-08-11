#pragma once

#include <cstddef>
#include <cstdint>

namespace rtp::codec {

// ---- G.711 mu-law (ITU-T G.711, RFC 3551 static PT 0) ----
// Memoryless: each sample compands independently, no state carried between
// samples or frames.
uint8_t linear_to_ulaw(int16_t sample);
int16_t ulaw_to_linear(uint8_t code);

void ulaw_encode_frame(const int16_t* pcm, size_t num_samples, uint8_t* out);
void ulaw_decode_frame(const uint8_t* codes, size_t num_samples, int16_t* out);

// ---- IMA ADPCM (Interactive Multimedia Association, RFC 3551 dynamic PT) ----
// Stateful: each sample's code depends on the running predictor and
// quantizer step index left by the previous sample.
struct AdpcmState {
  int16_t predictor = 0;
  uint8_t step_index = 0;
};

// Packs 4-bit codes two per byte; `out` must hold (num_samples+1)/2 bytes.
// `state` is read as the starting point and left holding the ending point,
// so the caller can write it into the *next* packet's header.
void adpcm_encode_frame(const int16_t* pcm, size_t num_samples, AdpcmState& state, uint8_t* out);
void adpcm_decode_frame(const uint8_t* packed, size_t num_samples, AdpcmState& state, int16_t* out);

// ADPCM's predictor/step-index state travels explicitly in every packet's
// payload (not just carried in memory), so that a lost packet can't
// desynchronize the decoder for the rest of the stream: the next packet
// that *does* arrive carries its own valid starting state.
constexpr size_t kAdpcmStateHeaderBytes = 4;  // predictor (2B) + step_index (1B) + reserved (1B)

void pack_adpcm_state(const AdpcmState& state, uint8_t out[kAdpcmStateHeaderBytes]);
AdpcmState unpack_adpcm_state(const uint8_t in[kAdpcmStateHeaderBytes]);

}  // namespace rtp::codec
