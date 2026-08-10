#include "rtp/audio_format.hpp"
#include "rtp/codec.hpp"
#include "rtp/rtp_packet.hpp"
#include "rtp/udp_socket.hpp"
#include "rtp/wav.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <stdexcept>
#include <string>

namespace {

void print_usage(const char* argv0) {
  std::fprintf(stderr, "usage: %s <input.wav> [--host H] [--port P] [--codec {pcm|ulaw|adpcm}]\n",
                argv0);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    print_usage(argv[0]);
    return 1;
  }

  std::string input_path = argv[1];
  std::string host = "127.0.0.1";
  uint16_t port = 5004;
  std::string codec_name = "pcm";

  for (int i = 2; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--host" && i + 1 < argc) {
      host = argv[++i];
    } else if (arg == "--port" && i + 1 < argc) {
      port = static_cast<uint16_t>(std::atoi(argv[++i]));
    } else if (arg == "--codec" && i + 1 < argc) {
      codec_name = argv[++i];
    } else {
      print_usage(argv[0]);
      return 1;
    }
  }

  uint8_t payload_type;
  if (codec_name == "pcm") {
    payload_type = rtp::packet::kPayloadTypePcm;
  } else if (codec_name == "ulaw") {
    payload_type = rtp::packet::kPayloadTypePcmu;
  } else if (codec_name == "adpcm") {
    payload_type = rtp::packet::kPayloadTypeAdpcm;
  } else {
    std::fprintf(stderr, "%s: unknown codec '%s' (expected pcm, ulaw, or adpcm)\n", argv[0],
                 codec_name.c_str());
    return 1;
  }

  try {
    rtp::wav::WavData wav = rtp::wav::read(input_path);
    rtp::net::UdpSocket socket;

    // RFC 3550 §5.1: sequence number and timestamp start at random values
    // (not zero) so an eavesdropper can't infer stream length across
    // sessions from a predictable starting point.
    std::random_device rd;
    std::mt19937 rng(rd());
    std::uniform_int_distribution<uint16_t> dist16(0, 0xFFFF);
    std::uniform_int_distribution<uint32_t> dist32(0, 0xFFFFFFFF);

    rtp::packet::RtpHeader header;
    header.payload_type = payload_type;
    header.sequence_number = dist16(rng);
    header.timestamp = dist32(rng);
    header.ssrc = dist32(rng);  // constant for the whole session

    rtp::codec::AdpcmState adpcm_state;  // only used when codec_name == "adpcm"

    uint8_t payload_buf[rtp::audio::kFrameBytesPcm];  // largest payload is raw PCM
    uint8_t out_buffer[rtp::net::kMaxDatagramBytes];
    const size_t total_samples = wav.samples.size();
    size_t sent = 0;
    bool first_packet = true;

    while (sent < total_samples) {
      size_t frame_samples =
          std::min(static_cast<size_t>(rtp::audio::kFrameSamples), total_samples - sent);
      const int16_t* frame = wav.samples.data() + sent;

      size_t payload_len;
      if (codec_name == "pcm") {
        std::memcpy(payload_buf, frame, frame_samples * sizeof(int16_t));
        payload_len = frame_samples * sizeof(int16_t);
      } else if (codec_name == "ulaw") {
        rtp::codec::ulaw_encode_frame(frame, frame_samples, payload_buf);
        payload_len = frame_samples;
      } else {
        // ADPCM: write the state as it stood *before* this frame, then
        // encode (which advances state to the end of this frame, ready for
        // the next packet's header).
        rtp::codec::pack_adpcm_state(adpcm_state, payload_buf);
        rtp::codec::adpcm_encode_frame(frame, frame_samples, adpcm_state,
                                        payload_buf + rtp::codec::kAdpcmStateHeaderBytes);
        payload_len = rtp::codec::kAdpcmStateHeaderBytes + (frame_samples + 1) / 2;
      }

      header.marker = first_packet;
      first_packet = false;

      size_t packet_len = rtp::packet::serialize(header, payload_buf, payload_len, out_buffer,
                                                   sizeof(out_buffer));
      if (packet_len == 0) {
        throw std::runtime_error("frame too large for one RTP/UDP datagram");
      }

      socket.send_to(out_buffer, packet_len, host, port);

      header.sequence_number = static_cast<uint16_t>(header.sequence_number + 1);
      // RTP timestamp advances by samples actually sent in this packet, not
      // by milliseconds (PLAN.md §9) -- ordinarily kFrameSamples, but the
      // final short frame of a file advances by less.
      header.timestamp += static_cast<uint32_t>(frame_samples);
      sent += frame_samples;
    }

    std::fprintf(stderr, "sent %zu samples (codec=%s) to %s:%u\n", total_samples,
                 codec_name.c_str(), host.c_str(), port);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "%s: %s\n", argv[0], e.what());
    return 1;
  }

  return 0;
}
