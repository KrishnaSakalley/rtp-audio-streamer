#pragma once

#include <cstdint>

namespace rtp::audio {

// Fixed wire format for the whole pipeline. Every codec, the packetiser, and
// the jitter buffer assume these values; they live in exactly one place so
// nothing downstream ever inlines the number 160.

constexpr int kSampleRateHz = 8000;  // 8 kHz mono, telephony-grade PCM
constexpr int kChannels = 1;
constexpr int kBitsPerSample = 16;
constexpr int kFrameDurationMs = 20;

// 8000 samples/s * 20 ms / 1000 ms/s = 160 samples per frame. RTP's
// timestamp field advances by this many samples per packet (RFC 3550 §5.1),
// not by milliseconds -- see rtp_packet.hpp.
constexpr int kFrameSamples = kSampleRateHz * kFrameDurationMs / 1000;
static_assert(kFrameSamples == 160, "frame size must stay exactly 160 samples");

constexpr int kBytesPerSample = kBitsPerSample / 8;
constexpr int kFrameBytesPcm = kFrameSamples * kBytesPerSample;  // 320 B raw PCM

}  // namespace rtp::audio
