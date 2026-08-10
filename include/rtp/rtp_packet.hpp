#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

namespace rtp::packet {

// RFC 3550 §5.1 fixed RTP header: 12 bytes, no CSRC list (CC is always 0 --
// this pipeline has exactly one source, never a mixer).
constexpr size_t kHeaderBytes = 12;
constexpr uint8_t kVersion = 2;

// RFC 3551 assigns PT=0 to G.711 mu-law statically. IMA ADPCM has no static
// assignment so it takes a dynamic PT (RFC 3551 §6, range 96-127); raw PCM
// gets a dynamic PT too, since our 8 kHz mono format doesn't match any
// static PCM assignment either.
constexpr uint8_t kPayloadTypePcmu = 0;
constexpr uint8_t kPayloadTypeAdpcm = 96;
constexpr uint8_t kPayloadTypePcm = 97;

struct RtpHeader {
  bool marker = false;
  uint8_t payload_type = kPayloadTypePcm;
  uint16_t sequence_number = 0;
  uint32_t timestamp = 0;
  uint32_t ssrc = 0;
};

// Packs header + payload into out_buffer (header first, then payload,
// wire fields big-endian per RFC 3550 §5.1). Returns the total bytes
// written, or 0 if out_buffer is too small to hold the header + payload.
// Caller owns out_buffer's storage, so this never allocates -- safe to call
// on the per-packet send path.
size_t serialize(const RtpHeader& header, const uint8_t* payload, size_t payload_len,
                  uint8_t* out_buffer, size_t out_buffer_len);

struct ParsedPacket {
  RtpHeader header;
  const uint8_t* payload = nullptr;  // points into the caller's buffer, not owned
  size_t payload_len = 0;
};

// Parses a received datagram in place: no copy, no allocation, so this is
// safe on the per-packet receive path. Returns std::nullopt if `data` is
// shorter than the fixed header, the version isn't 2, padding/extension/CSRC
// bits are set (this pipeline never sends those), or the payload type isn't
// one this pipeline understands.
std::optional<ParsedPacket> parse(const uint8_t* data, size_t len);

}  // namespace rtp::packet
