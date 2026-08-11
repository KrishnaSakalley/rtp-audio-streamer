# Design Walkthrough

A component-by-component reference for anyone reading or extending this code. Each
section covers one piece: what it does, the algorithm and its spec reference, why this
design over the obvious alternative, the trade-off accepted, and the failure mode if the
component is wrong.

## Architecture

```
WAV in
   | encode (G.711 mu-law / IMA ADPCM / raw PCM)
   | RTP packetize (RFC 3550, 12-byte header)
   | UDP  --> [impair: loss / reorder / jitter / dup] -->
   | adaptive jitter buffer (reorder + packet-loss concealment)
   | lock-free SPSC ring buffer
   | decode --> 20 ms real-time playout clock
WAV out
```

Three binaries: `rtp_sender`, `rtp_receiver`, `impair`. File-to-file, not a live
microphone/speaker — the whole pipeline works with no sound card present, and
correctness is `cmp`-able rather than judged by ear. Every constant that matters
(sample rate, frame size, buffer capacities, timing thresholds) lives in a named
`constexpr` with a one-line justification, not inlined.

## WAV I/O ([wav.hpp](../include/rtp/wav.hpp), [wav.cpp](../src/wav.cpp))

**What it does.** Reads and writes mono, 16-bit PCM RIFF/WAVE files.

**Algorithm.** RIFF is a chunked format: a 12-byte `RIFF....WAVE` header followed by a
sequence of `<4-byte tag><4-byte little-endian size><payload>` chunks. The reader walks
chunks by tag rather than trusting a fixed 44-byte offset, because real-world WAV files
often carry an optional `LIST`/`INFO` chunk before `data`. All multi-byte fields are
packed/unpacked by hand with explicit little-endian shifts — WAV is little-endian
regardless of host byte order, the same reasoning RTP's header code uses for
*big*-endian, just the opposite direction.

**Why this design.** A hand-rolled chunk walker is barely more code than trusting a
fixed offset, and it's the difference between "works on files this program wrote" and
"works on WAV files in general."

**Trade-off.** No support for compressed WAV formats (ADPCM-in-WAV, etc.) or multi-channel
audio — enforced by rejecting anything that isn't mono 16-bit PCM with a clear error,
rather than silently mis-decoding it.

**Failure mode.** A corrupt or truncated file produces a clear `std::runtime_error` with
the specific reason (missing chunk, wrong bit depth, etc.), not a garbage read or a
crash. This is the one place in the codebase where exceptions are used freely: file
open/parse happens once at startup, not on the per-packet audio path.

## UDP transport ([udp_socket.hpp](../include/rtp/udp_socket.hpp))

**What it does.** A thin RAII wrapper over a POSIX `SOCK_DGRAM` socket: bind, send,
receive-with-timeout.

**Why this design.** No abstraction beyond lifetime management and clear error
reporting. A networking framework would hide exactly the details (MTU sizing,
`SO_RCVTIMEO`, `recvfrom` semantics) that this project exists to make visible.

**Trade-off.** `recvfrom` silently truncates a datagram larger than the caller's
buffer, so every receive buffer is sized to `kMaxDatagramBytes` (1500, the Ethernet
MTU) rather than something merely "large enough for now."

**Failure mode.** An oversized receive buffer would silently drop the tail of large
packets with no error — this doesn't happen here because every payload in this
pipeline (max 320 B raw PCM) is far under the MTU, checked once at the boundary.

## RTP packetiser ([rtp_packet.hpp](../include/rtp/rtp_packet.hpp))

**What it does.** Packs and parses the 12-byte RFC 3550 §5.1 fixed RTP header
(version, marker, payload type, sequence number, timestamp, SSRC) around a payload.

**Algorithm.** `V=2, P=0, X=0, CC=0` packed into one byte via bit shifts (these are
sub-byte fields, no single library call handles them); the 16- and 32-bit fields use
`htons`/`htonl` for RFC 3550's mandated network byte order, with the converted value
`memcpy`'d into the flat wire buffer — not the header *struct* memcpy'd wholesale, which
would carry the struct's padding and host-endian multi-byte layout onto the wire.
Sequence number and timestamp start at random values (RFC 3550 §5.1) rather than zero.

**Why this design.** Byte-for-byte manual packing is the only way to guarantee the wire
format matches the spec regardless of compiler, platform, or struct layout.

**Trade-off.** `parse()` returns `std::optional` instead of throwing, because parsing
runs on the receive path where the "no exceptions on the audio path" constraint
applies — a malformed packet is a routine occurrence on a network, not an exceptional
one.

**Failure mode.** Get the byte order wrong and every implementation that talks to a
*different* RTP stack breaks, while this project's own sender/receiver pair (making the
same mistake symmetrically) would still work — exactly the kind of bug that survives
testing against yourself and fails in the field. `htons`/`htonl` on every field removes
the possibility.

## G.711 mu-law codec ([codec_ulaw.cpp](../src/codec_ulaw.cpp))

**What it does.** Companded 8-bit logarithmic encoding of 16-bit linear PCM (ITU-T
G.711, RFC 3551 static payload type 0).

**Algorithm.** Each sample is: sign bit, then biased and clipped, then a 3-bit segment
(exponent) found by locating the highest set bit of the biased magnitude, then a 4-bit
mantissa, then the whole byte bitwise-inverted (a hardware-codec-era quirk that
persists as the wire format). `BIAS = 132` here operates directly on the 16-bit
magnitude; the "bias 33" some references quote is the same algorithm applied to a
pre-shifted 14-bit magnitude — 132 = 33 x 4.

**Why this design.** A bit-scan for the segment (`for (mask = 0x4000; ...)` walking down
the 8 possible segment boundaries) is derived directly from the biased-magnitude range
math rather than copied from a specific reference implementation's lookup table —
same standard algorithm, original code.

**Trade-off.** ~8:1 dynamic range compression for 2:1 size compression. Good enough for
telephony-quality speech (measured 33.83 dB SNR on a 1 kHz test tone, comfortably above
the 30 dB gate), not intended for music or wide dynamic range material.

**Failure mode.** An off-by-one in the segment/mantissa bit math produces audible
quantization noise or clipping that gets *worse* at specific amplitude ranges (segment
boundaries), rather than a uniform SNR hit — which is why the SNR gate tests a
full-amplitude sine rather than just checking "does it compile."

## IMA ADPCM codec ([codec_adpcm.cpp](../src/codec_adpcm.cpp))

**What it does.** 4-bit adaptive differential PCM (Interactive Multimedia Association
spec): each sample is encoded as a 4-bit code representing how far it moved from a
running *predictor*, using a *step size* that itself adapts based on recent codes.

**Algorithm.** An 89-entry step-size table and 16-entry index-adjustment table (fixed
specification constants — any compliant IMA ADPCM codec uses these exact values) drive
a predictor that's updated every sample: encode computes the quantized difference
between the sample and the predictor, packs it into a 4-bit code (sign + 3 magnitude
bits), and updates both the predictor and the step index for the next sample. Decode
runs the identical state machine in reverse.

**Why this design — the state header.** Because each sample's code depends on the
previous sample's predictor and step index, a single lost *packet* would ordinarily
desynchronize every packet after it for the rest of the stream. This pipeline sends the
4-byte state (predictor + step index) explicitly in every packet's payload header
(`AdpcmState`, `pack_adpcm_state`/`unpack_adpcm_state`), re-derived by the *encoder* at
each frame boundary. The decoder never carries state across packets in memory — it
re-initializes from each packet's own header. A test proves this directly: decoding
packet 2 using only its own header state produces bit-identical output to decoding it
in a continuous session that also received packet 1.

**Trade-off.** 4 B of header overhead per 80 B of audio (~5%) buys complete loss
isolation. Without it, ADPCM would be unusable on any lossy network — one dropped
packet would garble everything downstream.

**Failure mode.** Forget to clamp a corrupt or malicious packet's step index before
indexing the 89-entry table, and it's an out-of-bounds read (`unpack_adpcm_state`
clamps to `[0, 88]` specifically for this reason — this is the one codec function that
processes attacker-influenced input directly).

## Network impairment simulator ([impair.cpp](../tools/impair.cpp))

**What it does.** A UDP middlebox: receives on one port, forwards to another, applying
seeded-random loss, reordering, jitter (delay), and duplication.

**Algorithm.** A `std::mt19937` seeded from `--seed` (default fixed, for reproducible
runs) decides per-packet: drop it (loss), hold it an extra fixed delay on top of any
jitter (reorder — a deliberate, independent knob rather than relying on jitter to
reorder only incidentally), and/or send it twice (dup). Zero-delay packets are
forwarded *synchronously* on the receiving thread; only genuinely delayed packets get
their own background thread.

**Why this design — and what broke.** The first version spawned a thread for every
forwarded packet, including zero-delay ones. The OS thread scheduler doesn't preserve
creation order, so this reordered packets nondeterministically even with every
impairment disabled — failing the `--loss 0.0` passthrough gate. The fix (skip the
thread entirely when there's nothing to wait for) is the difference between "lock-free
doesn't imply thread-free" and a subtle, hard-to-reproduce ordering bug.

**Trade-off.** Thread-per-delayed-packet is not how a production middlebox would scale
(a timer wheel or delay queue would), but this is a test/dev tool moving at most a few
hundred packets/second, not the real-time audio path — simplicity wins.

**Failure mode.** A reordering bug in the impairment tool itself would masquerade as a
bug in the *receiver's* reordering handling, which is exactly what happened during
development — the tool under test can be the thing that's actually broken.

## Adaptive jitter buffer ([jitter_buffer.cpp](../src/jitter_buffer.cpp))

**What it does.** The centrepiece: converts an unreliable, out-of-order, variably-timed
packet stream into a steady 20 ms-cadence PCM output, concealing loss along the way.

**Algorithm.**
- **Buffering:** a fixed 32-slot ring keyed on sequence number (`seq % 32`), sized to
  comfortably exceed the adaptive target depth's whole range.
- **Jitter estimate:** RFC 3550 §6.4.1's `D(i,j) = (Rj-Ri) - (Sj-Si)`, updated on every
  arrival against the *immediately previous* arrival (not necessarily the previous
  sequence number — this matches the RFC's own reference algorithm), then
  `J += (|D|-J)/16`.
- **Adaptive target depth:** `clamp(4 * J_ms, 100ms, 200ms)`. The floor is higher than
  the "2 frames" a textbook might suggest, specifically because this pipeline's sender
  paces itself with `sleep_until` rather than a hard real-time clock — the extra slack
  absorbs ordinary OS scheduling jitter (especially under WSL2) on top of genuine
  network jitter.
- **Playout deadline:** each frame's position gets a deadline
  (`first_arrival + frame_index*160 + target_depth_samples`), computed once and never
  retroactively moved. A packet arriving after its slot's deadline has already passed
  is dropped, not played — this is what makes it a real-time buffer rather than a plain
  reorder buffer.

**Why this design — the bug that mattered most.** Early on, `try_pull_due_frame` had no
guard against playing *past* the highest sequence number actually witnessed while the
stream was still active. Once real time caught up to a frame's nominal deadline, it
would conceal indefinitely far beyond the last packet ever sent — because only
`mark_stream_ended()` capped playout via `finished()`, and nothing stopped it before
that. A real-time deadline alone can't distinguish "this frame was lost" from "this
frame hasn't been transmitted yet"; only a later-arriving packet (proving the frame
should exist) or an explicit end-of-stream signal can. The fix — refuse to pull past
`highest_frame_index_seen_` until the stream is marked ended — has a regression test
locking it in.

**Trade-off.** Sequence-number-relative bookkeeping (rather than absolute RTP
timestamps) assumes a roughly 1:1 packet-to-frame mapping, which holds throughout this
project's short test clips but would need periodic rebasing for sessions longer than
~11 minutes (where the `int16_t` relative-index arithmetic starts to alias). Documented
rather than solved, since it's out of scope for a demo pipeline.

**Failure mode.** If the trailing packet(s) of a stream are lost, the buffer has no way
to know frames existed past the last one it saw — there's no length-of-stream signal in
this wire format (real systems solve this with RTCP BYE or a session-layer protocol).
The impairment integration test picks a duration where the seeded loss pattern doesn't
hit the last packet, rather than solving an out-of-scope session-teardown problem.

## Packet loss concealment (part of `JitterBuffer`)

**What it does.** When a frame's slot comes up empty at its deadline, synthesizes
output instead of a gap: repeats the last successfully decoded frame, fading it by 6 dB
per consecutive concealed frame, capped at 3 concealments, then silence.

**Why this design.** A repeated, fading frame is far less perceptually jarring than
either a hard gap (a click) or a wrong-guess interpolation. The fade is always computed
fresh from the *original* last real frame (`last_real_frame_`), not compounded onto the
previous concealed frame's output — the two approaches are mathematically equivalent
(`10^(-6n/20)` either way) but computing fresh avoids accumulating `int16_t` rounding
error across a run of concealments.

**Trade-off.** Simple and cheap (no pitch tracking, no waveform-similarity search like
production PLC algorithms use), which is appropriate for occasional, isolated losses
but produces audibly repetitive output under sustained loss — acceptable for a
demonstration pipeline, not for a production VoIP codec.

**Failure mode.** Fail to cap consecutive concealments and a long loss burst repeats
the same frame at full volume indefinitely — the 3-frame cap and fade specifically
prevent that.

## Lock-free SPSC ring buffer ([ring_buffer.hpp](../include/rtp/ring_buffer.hpp))

**What it does.** Hands decoded audio frames from the real-time playout thread to the
main thread's (allocating, potentially blocking) WAV writer without either thread ever
locking.

**Algorithm.** Two `alignas(64)` `std::atomic<size_t>` indices (head, tail) over a
fixed, power-of-two-sized array. `push()` acquire-loads `tail_` (must see every slot
the consumer has freed, or it could overwrite unread data) and release-stores the new
`head_` (publishes the just-written slot before the consumer can observe it). `pop()`
is the mirror image. One slot is always left empty — the classic trick for
distinguishing "full" from "empty" without a separate counter.

**Why this design.** `alignas(64)` padding between the two atomics is the detail that
makes "lock-free" actually fast rather than merely lock-free: without it, `head_` and
`tail_` would very likely share a 64-byte cache line, and every push/pop would bounce
that line between the producer's and consumer's cores (false sharing) even though the
two threads never touch the same logical data.

**Trade-off.** Fixed capacity (1024 slots, ~20 s of audio) means a genuinely stalled
consumer eventually causes `push()` to fail (counted as an "underrun," even though it's
technically the opposite — the *producer* backing up) rather than growing unboundedly.
Correct for a real-time system: unbounded growth on a stall is worse than a bounded,
observable failure.

**Failure mode.** Get the acquire/release pairing wrong (e.g. relaxed loads on the
cross-thread index) and the bug is a data race that may never manifest on x86's
comparatively strong memory model but reproduces on ARM — this is exactly the class of
bug a ThreadSanitizer run is designed to catch regardless of the memory model the CI
machine happens to run on.

## Threading model and playout clock (`rtp_receiver`, [receiver.cpp](../src/receiver.cpp))

**What it does.** Splits the receiver into three execution contexts: a receive thread
(socket -> jitter buffer), a playout thread (jitter buffer -> ring, on a fixed clock),
and the main thread (ring -> WAV).

**Algorithm.** The playout thread ticks on `clock_nanosleep(CLOCK_MONOTONIC,
TIMER_ABSTIME, &deadline, ...)` against an absolute, monotonically-advancing deadline
incremented by exactly 20 ms each iteration — not `sleep_for(20ms)`, which drifts,
because each call's own wake-up latency adds to the next call's start time. An absolute
deadline means a late wake-up on one tick doesn't push the next tick later.

**Why this design.** `JitterBuffer` itself has no internal synchronization (it's
exercised single-threaded by its own unit tests) — a plain `std::mutex` guards it
between the receive and playout threads. This is a deliberate choice, not an oversight:
the real-time constraint that justifies a lock-free structure applies to the playout
thread's *downstream* handoff, where a blocked producer would break the 20 ms cadence.
`JitterBuffer`'s per-call work is cheap enough that mutex contention is a non-issue,
confirmed by a ThreadSanitizer run of the full threaded pipeline reporting no races.

**Trade-off.** The receive thread polls the socket with a 5 ms timeout rather than
blocking indefinitely, purely so it stays responsive enough to notice idle timeouts and
shutdown signals promptly — a small, constant CPU cost (measured ~1% for the whole
three-thread receiver) in exchange for bounded shutdown latency.

**Failure mode.** Use `sleep_for` instead of an absolute-deadline `clock_nanosleep` and
the playout clock drifts against wall-clock time over a long run — each frame arrives
fractionally later than the last, until the jitter buffer's target depth can no longer
absorb the accumulated skew and playback starts dropping frames that were never
actually late over the network, only late relative to a clock that had drifted.

## Metrics harness ([generate_metrics.sh](../scripts/generate_metrics.sh))

**What it does.** Drives the real pipeline (not synthetic numbers) across codecs and
loss rates to populate `docs/METRICS.md` and generate the demo WAVs in `docs/audio/`.

**Why this design.** Every number in `docs/METRICS.md` comes from an actual run: codec
SNR from `snr_harness`, latency and jitter-buffer depth from
`JitterBuffer::Options::collect_stats` (an opt-in flag specifically so the default,
zero-allocation hot path is unaffected by metrics collection), max survivable loss from
the `wav_snr` tool sweeping loss rates until reconstructed SNR drops below a 10 dB
threshold, and CPU usage from wrapping the receiver (not the sender, which mostly
sleeps between paced sends and reads near 0% regardless of pipeline health) in
`/usr/bin/time -v`.

**Trade-off.** `collect_stats` uses a growable `std::vector` for latency/depth history —
not allocation-free, and deliberately not enabled by default. Real systems often draw
exactly this line: the hot path stays lean, and a separate diagnostics mode accepts
different constraints to get richer data.

**Failure mode.** A metrics script that fabricates or hard-codes numbers instead of
measuring them would pass its own review trivially and be actively misleading in a
portfolio context — the whole point of routing every figure through a real pipeline run
is that a regression in, say, the jitter buffer's latency would change the reported
number the next time the script runs, rather than silently going unnoticed.
