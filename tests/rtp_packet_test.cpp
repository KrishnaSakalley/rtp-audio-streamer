#include "rtp/rtp_packet.hpp"
#include "test_util.hpp"

#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

void check_round_trip(const rtp::packet::RtpHeader& header, const std::vector<uint8_t>& payload) {
  uint8_t buffer[2000];
  size_t len = rtp::packet::serialize(header, payload.data(), payload.size(), buffer, sizeof(buffer));
  RTP_CHECK(len == rtp::packet::kHeaderBytes + payload.size());

  auto parsed = rtp::packet::parse(buffer, len);
  RTP_CHECK(parsed.has_value());
  RTP_CHECK(parsed->header.marker == header.marker);
  RTP_CHECK(parsed->header.payload_type == header.payload_type);
  RTP_CHECK(parsed->header.sequence_number == header.sequence_number);
  RTP_CHECK(parsed->header.timestamp == header.timestamp);
  RTP_CHECK(parsed->header.ssrc == header.ssrc);
  RTP_CHECK(parsed->payload_len == payload.size());
  for (size_t i = 0; i < payload.size(); ++i) {
    RTP_CHECK(parsed->payload[i] == payload[i]);
  }
}

}  // namespace

int main() {
  const std::vector<uint8_t> payload = {1, 2, 3, 4, 5, 6, 7, 8};

  // Ordinary header round-trips.
  {
    rtp::packet::RtpHeader h;
    h.marker = true;
    h.payload_type = rtp::packet::kPayloadTypePcm;
    h.sequence_number = 1234;
    h.timestamp = 987654;
    h.ssrc = 0xDEADBEEF;
    check_round_trip(h, payload);
  }

  // Boundary values: zero and max for every field, and every valid PT.
  {
    rtp::packet::RtpHeader h;
    h.marker = false;
    h.payload_type = rtp::packet::kPayloadTypePcmu;
    h.sequence_number = 0;
    h.timestamp = 0;
    h.ssrc = 0;
    check_round_trip(h, payload);

    h.marker = true;
    h.payload_type = rtp::packet::kPayloadTypeAdpcm;
    h.sequence_number = 0xFFFF;
    h.timestamp = 0xFFFFFFFF;
    h.ssrc = 0xFFFFFFFF;
    check_round_trip(h, payload);
  }

  // Empty payload is a valid (if useless) packet.
  {
    rtp::packet::RtpHeader h;
    check_round_trip(h, {});
  }

  // Sequence number wraps 65535 -> 0 without special-casing at serialize time.
  {
    rtp::packet::RtpHeader h;
    h.sequence_number = 65535;
    check_round_trip(h, payload);
    h.sequence_number = 0;
    check_round_trip(h, payload);
  }

  // Malformed: buffer shorter than the fixed 12-byte header.
  {
    uint8_t short_buf[11] = {0};
    RTP_CHECK(!rtp::packet::parse(short_buf, sizeof(short_buf)).has_value());
    RTP_CHECK(!rtp::packet::parse(short_buf, 0).has_value());
  }

  // Malformed: wrong version (V=1 in the top 2 bits instead of V=2).
  {
    uint8_t buf[rtp::packet::kHeaderBytes] = {0};
    buf[0] = static_cast<uint8_t>(1 << 6);
    RTP_CHECK(!rtp::packet::parse(buf, sizeof(buf)).has_value());
  }

  // Malformed: P/X/CC bits set (this pipeline never sends them).
  {
    uint8_t buf[rtp::packet::kHeaderBytes] = {0};
    buf[0] = static_cast<uint8_t>((rtp::packet::kVersion << 6) | 0x20);  // P=1
    RTP_CHECK(!rtp::packet::parse(buf, sizeof(buf)).has_value());
  }

  // Malformed: unrecognized payload type.
  {
    uint8_t buf[rtp::packet::kHeaderBytes] = {0};
    buf[0] = static_cast<uint8_t>(rtp::packet::kVersion << 6);
    buf[1] = 42;  // not PCMU (0), ADPCM (96), or PCM (97)
    RTP_CHECK(!rtp::packet::parse(buf, sizeof(buf)).has_value());
  }

  // serialize() refuses to write past a too-small buffer instead of
  // truncating silently.
  {
    rtp::packet::RtpHeader h;
    uint8_t tiny[5];
    size_t len = rtp::packet::serialize(h, payload.data(), payload.size(), tiny, sizeof(tiny));
    RTP_CHECK(len == 0);
  }

  std::puts("rtp_packet_test OK");
  return 0;
}
