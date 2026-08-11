#include "rtp/rtp_packet.hpp"

#include <arpa/inet.h>

#include <cstring>

namespace rtp::packet {

namespace {

// htons/htonl (RFC 3550's mandated network byte order) operate on the
// integer *value*; memcpy'ing the 2/4-byte result into the wire buffer
// copies its in-memory representation, which is exactly the big-endian
// bytes we want. Note this is memcpy'ing a *scalar*, which is safe --
// memcpy'ing RtpHeader itself would not be, since it has padding between
// its bool/uint8_t/uint16_t/uint32_t fields that doesn't exist on the wire.

void write_u16(uint8_t* p, uint16_t host_value) {
  uint16_t net = htons(host_value);
  std::memcpy(p, &net, sizeof(net));
}

void write_u32(uint8_t* p, uint32_t host_value) {
  uint32_t net = htonl(host_value);
  std::memcpy(p, &net, sizeof(net));
}

uint16_t read_u16(const uint8_t* p) {
  uint16_t net;
  std::memcpy(&net, p, sizeof(net));
  return ntohs(net);
}

uint32_t read_u32(const uint8_t* p) {
  uint32_t net;
  std::memcpy(&net, p, sizeof(net));
  return ntohl(net);
}

}  // namespace

size_t serialize(const RtpHeader& header, const uint8_t* payload, size_t payload_len,
                  uint8_t* out_buffer, size_t out_buffer_len) {
  const size_t total = kHeaderBytes + payload_len;
  if (out_buffer_len < total) {
    return 0;
  }

  out_buffer[0] = static_cast<uint8_t>(kVersion << 6);  // V=2, P=0, X=0, CC=0
  out_buffer[1] = static_cast<uint8_t>((header.marker ? 0x80 : 0x00) |
                                        (header.payload_type & 0x7F));
  write_u16(out_buffer + 2, header.sequence_number);
  write_u32(out_buffer + 4, header.timestamp);
  write_u32(out_buffer + 8, header.ssrc);

  for (size_t i = 0; i < payload_len; ++i) {
    out_buffer[kHeaderBytes + i] = payload[i];
  }

  return total;
}

std::optional<ParsedPacket> parse(const uint8_t* data, size_t len) {
  if (len < kHeaderBytes) {
    return std::nullopt;
  }

  uint8_t version = static_cast<uint8_t>(data[0] >> 6);
  uint8_t padding_ext_cc = static_cast<uint8_t>(data[0] & 0x3F);  // P|X|CC bits
  if (version != kVersion || padding_ext_cc != 0) {
    return std::nullopt;
  }

  uint8_t payload_type = static_cast<uint8_t>(data[1] & 0x7F);
  if (payload_type != kPayloadTypePcmu && payload_type != kPayloadTypeAdpcm &&
      payload_type != kPayloadTypePcm) {
    return std::nullopt;
  }

  ParsedPacket result;
  result.header.marker = (data[1] & 0x80) != 0;
  result.header.payload_type = payload_type;
  result.header.sequence_number = read_u16(data + 2);
  result.header.timestamp = read_u32(data + 4);
  result.header.ssrc = read_u32(data + 8);
  result.payload = data + kHeaderBytes;
  result.payload_len = len - kHeaderBytes;
  return result;
}

}  // namespace rtp::packet
