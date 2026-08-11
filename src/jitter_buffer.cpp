#include "rtp/jitter_buffer.hpp"

#include "rtp/codec.hpp"
#include "rtp/rtp_packet.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace rtp::jitter {

JitterBuffer::JitterBuffer(Options options) : options_(options) {}

void JitterBuffer::update_jitter_estimate(uint32_t rtp_timestamp, uint32_t arrival_samples) {
  if (have_prev_arrival_) {
    // RFC 3550 §6.4.1: D(i,j) = (Rj - Ri) - (Sj - Si), updated on every
    // arrival using the immediately previous packet (not necessarily the
    // previous *sequence* number -- this matches the RFC's own reference
    // algorithm in Appendix A.8).
    int64_t d_arrival =
        static_cast<int64_t>(arrival_samples) - static_cast<int64_t>(prev_arrival_samples_);
    int64_t d_timestamp =
        static_cast<int64_t>(rtp_timestamp) - static_cast<int64_t>(prev_rtp_timestamp_);
    double d = static_cast<double>(d_arrival - d_timestamp);
    jitter_estimate_samples_ += (std::fabs(d) - jitter_estimate_samples_) / 16.0;

    double jitter_ms = jitter_estimate_samples_ * 1000.0 / rtp::audio::kSampleRateHz;
    // 4x the raw jitter estimate is a common rule-of-thumb safety margin,
    // clamped so a quiet link still gets at least kMinTargetMs of slack and
    // a very jittery one can't balloon latency past kMaxTargetMs.
    int target = static_cast<int>(4.0 * jitter_ms);
    target_depth_ms_ = std::clamp(target, kMinTargetMs, kMaxTargetMs);
    if (options_.collect_stats) {
      target_depth_history_ms_.push_back(target_depth_ms_);
    }
  }
  prev_rtp_timestamp_ = rtp_timestamp;
  prev_arrival_samples_ = arrival_samples;
  have_prev_arrival_ = true;
}

void JitterBuffer::push(uint16_t sequence_number, uint32_t rtp_timestamp, uint8_t payload_type,
                         const uint8_t* payload, size_t payload_len, uint32_t arrival_samples) {
  counts_.received += 1;
  update_jitter_estimate(rtp_timestamp, arrival_samples);

  if (!started_) {
    started_ = true;
    base_sequence_ = sequence_number;
    first_arrival_samples_ = arrival_samples;
  }

  // Signed 16-bit delta orders sequence numbers correctly across the
  // 65535 -> 0 wrap, same technique as SequenceTracker.
  int16_t frame_index_16 = static_cast<int16_t>(sequence_number - base_sequence_);
  int32_t frame_index = frame_index_16;

  if (!have_highest_) {
    have_highest_ = true;
    highest_sequence_seen_ = sequence_number;
  } else {
    int16_t delta_from_highest = static_cast<int16_t>(sequence_number - highest_sequence_seen_);
    if (delta_from_highest > 0) {
      highest_sequence_seen_ = sequence_number;
    } else if (delta_from_highest < 0) {
      counts_.reordered += 1;
    }
    // delta_from_highest == 0 would mean this sequence number was already
    // the most recent -- a duplicate, caught by the slot check below rather
    // than double-counted here.
  }
  highest_frame_index_seen_ = std::max(highest_frame_index_seen_, frame_index);

  if (frame_index < static_cast<int32_t>(frames_played_)) {
    counts_.late_dropped += 1;  // its slot already played
    return;
  }
  if (frame_index - static_cast<int32_t>(frames_played_) >= static_cast<int32_t>(kCapacity)) {
    counts_.late_dropped += 1;  // farther ahead than the fixed-capacity ring can hold
    return;
  }

  size_t slot_index = static_cast<size_t>(sequence_number) % kCapacity;
  Slot& slot = slots_[slot_index];
  if (slot.occupied && slot.sequence_number == sequence_number) {
    counts_.duplicate += 1;
    return;
  }

  slot.occupied = true;
  slot.sequence_number = sequence_number;
  slot.payload_type = payload_type;
  slot.payload_len = static_cast<uint16_t>(std::min(payload_len, sizeof(slot.payload)));
  slot.arrival_samples = arrival_samples;
  std::memcpy(slot.payload, payload, slot.payload_len);
}

void JitterBuffer::decode_slot(const Slot& slot, int16_t out[rtp::audio::kFrameSamples]) {
  if (slot.payload_type == rtp::packet::kPayloadTypePcm) {
    size_t num_samples =
        std::min<size_t>(slot.payload_len / sizeof(int16_t), rtp::audio::kFrameSamples);
    for (size_t i = 0; i < num_samples; ++i) {
      int16_t sample;
      std::memcpy(&sample, slot.payload + i * sizeof(int16_t), sizeof(sample));
      out[i] = sample;
    }
    for (size_t i = num_samples; i < rtp::audio::kFrameSamples; ++i) {
      out[i] = 0;
    }
  } else if (slot.payload_type == rtp::packet::kPayloadTypePcmu) {
    size_t num_samples = std::min<size_t>(slot.payload_len, rtp::audio::kFrameSamples);
    codec::ulaw_decode_frame(slot.payload, num_samples, out);
    for (size_t i = num_samples; i < rtp::audio::kFrameSamples; ++i) {
      out[i] = 0;
    }
  } else if (slot.payload_type == rtp::packet::kPayloadTypeAdpcm &&
             slot.payload_len >= codec::kAdpcmStateHeaderBytes) {
    codec::AdpcmState state = codec::unpack_adpcm_state(slot.payload);
    size_t packed_bytes = slot.payload_len - codec::kAdpcmStateHeaderBytes;
    size_t num_samples = std::min<size_t>(packed_bytes * 2, rtp::audio::kFrameSamples);
    codec::adpcm_decode_frame(slot.payload + codec::kAdpcmStateHeaderBytes, num_samples, state,
                               out);
    for (size_t i = num_samples; i < rtp::audio::kFrameSamples; ++i) {
      out[i] = 0;
    }
  } else {
    std::fill(out, out + rtp::audio::kFrameSamples, static_cast<int16_t>(0));
  }
}

void JitterBuffer::conceal(int16_t out[rtp::audio::kFrameSamples]) {
  counts_.concealed += 1;

  if (options_.disable_plc_fade || !have_last_real_frame_ ||
      consecutive_concealments_ >= kMaxConsecutiveConcealments) {
    std::fill(out, out + rtp::audio::kFrameSamples, static_cast<int16_t>(0));
    return;
  }

  // Fade is computed fresh from the last *real* frame each time (not
  // compounded onto the previous concealed output), so quantization from
  // repeated int16 rounding doesn't accumulate across concealed frames.
  consecutive_concealments_ += 1;
  double fade_db = kFadeDbPerConcealedFrame * consecutive_concealments_;
  double gain = std::pow(10.0, fade_db / 20.0);
  for (size_t i = 0; i < rtp::audio::kFrameSamples; ++i) {
    out[i] = static_cast<int16_t>(last_real_frame_[i] * gain);
  }
}

bool JitterBuffer::try_pull_due_frame(uint32_t now_samples, int16_t out[rtp::audio::kFrameSamples]) {
  if (!started_ || finished()) {
    return false;
  }

  if (!stream_ended_) {
    // Never speculate past the frontier of what's actually been witnessed:
    // a real-time deadline alone can't distinguish "this frame was lost"
    // from "this frame hasn't been sent yet, transmission is still in
    // progress" -- only a later-arriving packet (or mark_stream_ended())
    // can tell those apart. Without this check, once real time catches up
    // to frame_index's nominal deadline it would conceal forever, racing
    // arbitrarily far ahead of what any packet has actually proven exists.
    if (static_cast<int32_t>(frames_played_) > highest_frame_index_seen_) {
      return false;
    }

    uint32_t target_depth_samples =
        static_cast<uint32_t>(target_depth_ms_) * rtp::audio::kSampleRateHz / 1000;
    uint32_t deadline = first_arrival_samples_ +
                         frames_played_ * static_cast<uint32_t>(rtp::audio::kFrameSamples) +
                         target_depth_samples;
    if (now_samples < deadline) {
      return false;  // not due yet
    }
  }

  uint16_t seq = static_cast<uint16_t>(base_sequence_ + frames_played_);
  size_t slot_index = static_cast<size_t>(seq) % kCapacity;
  Slot& slot = slots_[slot_index];

  if (slot.occupied && slot.sequence_number == seq) {
    decode_slot(slot, out);
    if (options_.collect_stats) {
      uint32_t latency_samples = now_samples - slot.arrival_samples;
      latency_samples_ms_.push_back(latency_samples * 1000 / rtp::audio::kSampleRateHz);
    }
    slot.occupied = false;
    consecutive_concealments_ = 0;
    have_last_real_frame_ = true;
    std::memcpy(last_real_frame_, out, sizeof(last_real_frame_));
  } else {
    counts_.lost += 1;
    conceal(out);
  }

  frames_played_ += 1;
  return true;
}

void JitterBuffer::mark_stream_ended() { stream_ended_ = true; }

bool JitterBuffer::finished() const {
  if (!started_) {
    return stream_ended_;
  }
  if (!stream_ended_) {
    return false;
  }
  return static_cast<int32_t>(frames_played_) > highest_frame_index_seen_;
}

}  // namespace rtp::jitter
