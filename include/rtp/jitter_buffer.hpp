#pragma once

#include "rtp/audio_format.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace rtp::jitter {

// Fixed-capacity ring keyed on sequence number -- PLAN.md forbids allocation
// on the audio path after startup. 32 slots comfortably covers the adaptive
// target depth's whole range (kMaxTargetMs / 20ms per frame = 10 frames)
// plus headroom for reordering and jitter spikes.
constexpr size_t kCapacity = 32;

// 100ms (5 frames) rather than a tighter value: this pipeline's sender
// paces itself with std::this_thread::sleep_until rather than a hard
// real-time clock, so the floor needs enough slack to absorb ordinary OS
// scheduling jitter (especially under WSL2) on top of actual network
// jitter, or a perfectly healthy link could still shed frames early.
constexpr int kMinTargetMs = 100;
constexpr int kMaxTargetMs = 200;  // cap added latency to stay conversational

constexpr int kMaxConsecutiveConcealments = 3;  // PLAN.md §4: fade for 3, then silence
constexpr double kFadeDbPerConcealedFrame = -6.0;

struct Counts {
  uint64_t received = 0;
  uint64_t lost = 0;          // frame's deadline arrived with nothing buffered for it
  uint64_t late_dropped = 0;  // packet arrived after its slot had already played, or
                               // farther ahead than the buffer's fixed capacity allows
  uint64_t reordered = 0;     // arrived behind the highest sequence number already seen
  uint64_t concealed = 0;     // PLC frames produced (faded repeat or silence)
  uint64_t duplicate = 0;     // sequence number already occupies its (unplayed) slot
};

// Reorders, times, and conceals a live RTP stream into a steady 20ms-cadence
// PCM output. This is the centrepiece of the pipeline: everything upstream
// (sockets, codecs) exists to feed it, and everything downstream (the WAV
// writer, eventually a sound card) just consumes what it produces.
class JitterBuffer {
 public:
  JitterBuffer();

  // Feeds one received, parsed RTP packet in. `arrival_samples` is the
  // receiver's local clock at the moment of receipt, expressed in RTP
  // timestamp units (8 kHz samples) -- i.e. wall-clock time scaled to the
  // media clock -- so RFC 3550 §6.4.1's D = (Rj-Ri)-(Sj-Si) can be computed
  // directly against the packet's (sender-clock) RTP timestamp.
  void push(uint16_t sequence_number, uint32_t rtp_timestamp, uint8_t payload_type,
            const uint8_t* payload, size_t payload_len, uint32_t arrival_samples);

  // If the frame at the current playout cursor is due (now_samples has
  // reached its deadline), produces it -- decoding a buffered real packet,
  // or concealing a missing one -- into `out`, advances the cursor by one
  // frame, and returns true. Returns false if nothing is due yet; the
  // caller should wait for more time to pass and/or more packets to arrive
  // before calling again.
  bool try_pull_due_frame(uint32_t now_samples, int16_t out[rtp::audio::kFrameSamples]);

  // Call once no more packets are coming (e.g. the socket's own idle
  // timeout fired). After this, try_pull_due_frame() stops waiting on
  // real-time deadlines and drains every remaining frame immediately, so
  // the stream flushes to completion instead of blocking on deadlines a
  // dead clock will never reach.
  void mark_stream_ended();

  // True once mark_stream_ended() has been called and every frame through
  // the highest sequence number ever received has been produced.
  bool finished() const;

  int target_depth_ms() const { return target_depth_ms_; }
  const Counts& counts() const { return counts_; }

 private:
  struct Slot {
    bool occupied = false;
    uint16_t sequence_number = 0;
    uint8_t payload_type = 0;
    uint16_t payload_len = 0;
    uint8_t payload[512];  // largest real payload is 320B raw PCM; generous headroom
  };

  void update_jitter_estimate(uint32_t rtp_timestamp, uint32_t arrival_samples);
  void decode_slot(const Slot& slot, int16_t out[rtp::audio::kFrameSamples]);
  void conceal(int16_t out[rtp::audio::kFrameSamples]);

  std::array<Slot, kCapacity> slots_;

  bool started_ = false;
  uint16_t base_sequence_ = 0;  // sequence number of the first packet ever received
  uint32_t first_arrival_samples_ = 0;
  uint32_t frames_played_ = 0;  // playout cursor, as a frame offset from base_sequence_

  bool have_highest_ = false;
  uint16_t highest_sequence_seen_ = 0;
  int32_t highest_frame_index_seen_ = -1;

  bool have_prev_arrival_ = false;
  uint32_t prev_rtp_timestamp_ = 0;
  uint32_t prev_arrival_samples_ = 0;
  double jitter_estimate_samples_ = 0.0;  // RFC 3550 §6.4.1 "J"

  int target_depth_ms_ = kMinTargetMs;

  bool have_last_real_frame_ = false;
  int16_t last_real_frame_[rtp::audio::kFrameSamples] = {};
  int consecutive_concealments_ = 0;

  bool stream_ended_ = false;

  Counts counts_;
};

}  // namespace rtp::jitter
