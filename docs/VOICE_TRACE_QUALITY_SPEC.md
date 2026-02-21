# Voice pipeline quality tracking – files and log format

Goal: at each step **(capture → encode → send → recv [server] → decode → buffer → play)** write **one log line** with a **quality-like metric** (e.g. `quality_pct` or `good/total*100`) so you can see where quality drops.

Use the **same trace file** as today: client `talkme_voice_trace.log` (via `Logger::Instance().LogVoiceTrace(...)`), server `voice_trace.log` (via `VoiceTrace::log(...)`). Keep **one line per step** (per frame or per N-th frame to avoid huge logs). All numbers below can be sampled (e.g. every 50th frame) like current trace.

---

## 1. Capture

**File to modify:** `src/audio/AudioEngine.cpp`

**Where:** In the capture path where PCM is read from the capture ring buffer and passed to the encoder (same place as current `step=capture`). You have:
- `framesRead` / `OPUS_FRAME_SIZE` (e.g. 480) = what was requested
- Actual frames read from `ma_pcm_rb_acquire_read` (may be less if underrun)

**Log line (one per N-th frame):**
- `step=capture requested=480 got=<framesRead> quality_pct=<framesRead*100/480> seq=<seq>`
- Or: `step=capture pcm_samples=<framesRead> expected=480 quality_pct=<framesRead*100/480> seq=<seq>`

So: **quality_pct = (what_captured / what_requested) * 100**. If capture buffer was empty, `got` < 480 and quality_pct < 100.

---

## 2. Encode

**File to modify:** `src/audio/AudioEngine.cpp`

**Where:** Right after `opus_encode_float()` / `encoder->Encode()` (same place as current `step=encode`). You have:
- Input: 480 PCM samples
- Output: `packet.size()` bytes (0 if encode failed)

**Log line:**
- `step=encode pcm_samples=480 opus_bytes=<packet.size()> quality_pct=<encode_ok ? 100 : 0> seq=<seq>`
- Or define quality as: 100 if bytes > 0, else 0; optionally cap by max expected bytes.

So: **quality_pct = 100 if encode produced bytes, else 0** (or a finer metric if you add one).

---

## 3. Send

**File to modify:** `src/app/Application.cpp`

**Where:** Right after sending (UDP or TCP) in the voice callback, where you already log `step=send`. You have:
- `payload.size()` / `opusData.size()` (what was handed to transport)
- Send is fire-and-forget; you don’t get a per-packet ack. So “quality” here = “we handed this much to the stack”.

**Log line:**
- `step=send path=udp|tcp seq=<seq> opus_bytes=<opusData.size()> payload_bytes=<payload.size()> handed_pct=100`
- Or: same as now, add `handed_pct=100` (meaning 100% of the encoded payload was given to send). If you later add “send failed” handling, set `handed_pct=0` on failure.

So: **quality_pct / handed_pct = 100** when the payload was successfully passed to send; 0 if send failed (if you add that check).

---

## 4. Server receive

**File to modify:** `server/server.cpp`

**Where:** In `HandleVoiceUdpPacket`, after parsing the voice payload:
- When you have `parsed.valid` and log `step=udp_voice_recv`: this packet was “received and parsed”.
- When you later **relay** or **drop**: you can log a second line with the outcome.

**Log lines (one per packet or per N-th):**
- After parse success: `step=server_recv sender=<parsed.sender> payload_bytes=<voicePayload.size()> parse_ok=1`
- After relay: `step=server_relay sender=... relay_tcp=<n> relay_udp=<m> payload_bytes=... relay_ok=1`
- After drop: `step=server_drop sender=... reason=... relay_ok=0`

So: **quality_pct = 100 when relay_ok=1, 0 when relay_ok=0**. You can aggregate on server: (packets relayed / packets received) * 100 over a window if you want a periodic “server_recv_quality_pct” line.

---

## 5. Client receive (after network)

**File to modify:** `src/app/Application.cpp`

**Where:** In the TCP and UDP voice callbacks, where you already log `step=recv`. You have:
- `parsed.opusData.size()` = bytes received for this packet
- You don’t have “expected” bytes per packet; you have sequence numbers. So “receive quality” can be: this packet was received (100) or duplicate/late (could log 0 or skip).

**Log line:**
- `step=recv path=udp|tcp sender=... seq=... opus_bytes=... recv_ok=1`
- For duplicates/late you could log `recv_ok=0` or not log a quality line.

So: **quality_pct = 100 for each packet accepted into the pipeline** (the ones that reach `pushVoiceIfNew` and are pushed to the engine). Optionally add a periodic line: (packets accepted / packets received from network) * 100.

---

## 6. Decode

**File to modify:** `src/audio/AudioEngine.cpp`

**Where:** In `PushIncomingAudioWithSequence`, after `track.decoder->DecodeWithDiagnostics(opusData.data(), opusData.size(), pcm)` (and PLC path). You have:
- `opusData.size()` = bytes in
- `samples` = samples out (OPUS_FRAME_SIZE = 480 on success)
- PLC: “decoded” a loss frame (no bytes in).

**Log line:**
- `step=decode sender=... opus_bytes=... samples_out=<samples> expected=480 quality_pct=<samples*100/480> seq=...`
- For PLC frames: `step=decode sender=... opus_bytes=0 samples_out=480 plc=1 quality_pct=100` (PLC counts as “reconstructed”, so 100 for that frame).

So: **quality_pct = (samples_out / 480) * 100** (100 when decode or PLC produced a full frame).

---

## 7. Buffer

**File to modify:** `src/audio/AudioEngine.cpp`

**Where:** Right after writing decoded PCM to the track ring buffer (`ma_pcm_rb_commit_write`). You have:
- Requested write: OPUS_FRAME_SIZE (480)
- Actual written: `frames` from `ma_pcm_rb_acquire_write` (may be less if buffer full)
- Buffer state: `ma_pcm_rb_available_read(&track.rb)` after write

**Log line:**
- `step=buffer sender=... requested=480 written=<frames> buffer_frames=<bufFrames> buffer_ms=... quality_pct=<written*100/480>`
- If written < 480, buffer was full (overflow); quality_pct < 100.

So: **quality_pct = (written / 480) * 100**. 100 = full frame written; < 100 = overflow/drop.

---

## 8. Play

**File to modify:** `src/audio/AudioEngine.cpp`

**Where:** In the miniaudio **playback callback** where you read from each track’s ring buffer and mix. You have:
- `frameCount` = frames requested by the device
- Per track: `available` = frames in buffer; you read `read` frames (min(available, frameCount)); if `available < frameCount` you get an **underrun** (you already increment `bufferUnderruns`).

**Log line (e.g. every 250th callback as now):**
- `step=play frames_requested=<frameCount> frames_read=<total_read_from_all_tracks> underruns=<internal->bufferUnderruns> quality_pct=<(frames_read/frames_requested)*100>` or similar.
- Or: `step=play frames=<frameCount> active_tracks=... underruns=... quality_pct=<100 - underrun_penalty>` where underrun_penalty is derived from underruns in the last period.

Simpler: **quality_pct = 100** when no underrun in this callback, **0** (or reduced) when underrun. So one line per N-th callback: `step=play frames=... active_tracks=... underruns_total=... had_underrun=0|1 quality_pct=100|0`.

So: **quality_pct = 100 if enough data was read for this callback, else 0** (or a percentage of frames actually read vs requested).

---

## Summary: which files to modify

| Step            | File                          | Where in file                                                                 |
|-----------------|-------------------------------|-------------------------------------------------------------------------------|
| 1. Capture      | `src/audio/AudioEngine.cpp`   | Capture path: after reading from capture ring buffer (near `step=capture`).   |
| 2. Encode       | `src/audio/AudioEngine.cpp`   | After `encoder->Encode()` (near `step=encode`).                               |
| 3. Send         | `src/app/Application.cpp`     | Voice send callback (near existing `step=send`).                              |
| 4. Server recv  | `server/server.cpp`           | `HandleVoiceUdpPacket`: after parse, and at relay/drop.                       |
| 5. Client recv  | `src/app/Application.cpp`     | TCP and UDP voice callbacks (near existing `step=recv`).                      |
| 6. Decode       | `src/audio/AudioEngine.cpp`   | `PushIncomingAudioWithSequence`: after decode (and PLC).                      |
| 7. Buffer       | `src/audio/AudioEngine.cpp`   | After `ma_pcm_rb_commit_write` for the track (near `step=buffer`).            |
| 8. Play         | `src/audio/AudioEngine.cpp`   | Playback callback, after mixing (near existing `step=play`).                  |

**Helper:** `src/core/Logger.h` – only `LogVoiceTrace(const std::string&)` is needed; no signature change required. Server already has `VoiceTrace::log(const std::string&)`.

---

## Log format convention

Use a single line per event, with space-separated key=value pairs, for example:

- `step=<name> ... quality_pct=<0-100> ...`
- Or `step=<name> ... <numerator>_<denominator>_pct=<n>*100/<d>` so you can compute (what_captured/what_saved)*100 etc. in a script.

Sampling: keep logging every N-th frame (e.g. 50) for capture/encode/decode/buffer, and every N-th callback for play, to avoid huge logs. Optionally add a **periodic summary** line (e.g. every 5 s) with average quality_pct per step over the last window.

---

## Optional: periodic “pipeline quality” summary

In **Application.cpp** or **AudioEngine.cpp**, every 5 seconds (reuse the telemetry timer), log one line per step with **average quality over the last 5 s**:

- `step=summary_capture avg_quality_pct=...`
- `step=summary_encode avg_quality_pct=...`
- … same for send, server_recv, recv, decode, buffer, play.

That requires maintaining running counters (e.g. total_frames, total_ok_frames) per step and writing one summary line per step in the same trace file.

You can send this spec plus the listed files to Claude and ask to implement the exact log lines and, if desired, the periodic summary.
