# TalkMe Custom Voice Stack Architecture

A custom voice pipeline built from modular C++ libraries gives full control over memory and CPU. This doc describes the architecture and how TalkMe implements it.

---

## Implementation status

- **Client:** Opus (AudioEngine), miniaudio, VoiceTransport. Voice is sent over TCP always; UDP is used when the server sends `prefer_udp` and the client has UDP running (same Opus payload on both). Echo live (Echo_Request/Response) reflects latency/loss on the TCP path when UDP is not used.
- **Server:** Relays `Voice_Data_Opus` over TCP to all channel members and, when the client has a UDP binding, over UDP (port 5556). No WebRTC/SFU; `Voice_Config` does not include `webrtc_ws_url`.

---

## 1. Audio Codec: **libopus (Opus)**

Same codec WebRTC uses for audio: open-source, fast, royalty-free. Handles narrow-band speech through full-band stereo.

**TalkMe:** `src/audio/AudioEngine.cpp`
- **Encode:** `OpusEncoderWrapper` — 48 kHz mono, VOIP preset, 32 kbps, in-band FEC, VBR. Frames are 480 samples (10 ms at 48 kHz).
- **Decode:** `OpusDecoderWrapper` — decode normal frames and **loss frames** (null input → PLC). Bitrate and packet-loss percent are adapted from telemetry.

---

## 2. Audio Capture / Playback

Low-latency access to microphone and speakers. Common choices: **RtAudio** or native **Windows WASAPI**.

**TalkMe:** **miniaudio** (`vendor/miniaudio.h`)
- Single duplex device: capture + playback in one callback.
- On Windows, miniaudio can use WASAPI for low-latency.
- **Capture:** PCM (float) → ring buffer → encoder consumes 480-sample chunks → Opus frames.
- **Playback:** Per-remote ring buffers → decode Opus → mix → output. Adaptive jitter buffer per track.

So capture/playback is already a lightweight, low-latency path (miniaudio instead of RtAudio, same idea).

---

## 3. Networking: **UDP for voice**

Voice must use **UDP** for real-time audio; TCP’s reliability and head-of-line blocking add latency that is unacceptable for live voice.

**TalkMe:**
- **UDP:** `src/network/VoiceTransport.cpp` — Boost.Asio UDP socket. Sends Opus payloads (with small header) to the server; receives voice from server. Used when `prefer_udp` and UDP are available.
- **TCP fallback:** Main TalkMe connection carries `Voice_Data_Opus` so voice still works when UDP is blocked or unavailable. Server forwards to channel members.

So: **UDP when possible, TCP as fallback.** Both carry the same Opus payloads; no custom codec on the wire.

---

## 4. What WebRTC Does Automatically vs What You Implement

| Feature | WebRTC | TalkMe custom stack |
|--------|--------|----------------------|
| **Codec** | Opus | ✓ libopus (same) |
| **Capture/playback** | Browser APIs | ✓ miniaudio (WASAPI-capable) |
| **Transport** | SRTP over UDP (or TURN) | ✓ UDP (Asio) + TCP fallback |
| **NAT traversal** | STUN / TURN built-in | ❌ Server-relayed only (no direct P2P UDP through NAT) |
| **Jitter buffer** | Built-in | ✓ Adaptive per-track buffer (`adaptiveBufferLevel`, min/max ms) |
| **Packet loss concealment** | Built-in | ✓ Opus in-band FEC + `opus_decode(..., null, 0, ...)` for loss frames |
| **Echo cancellation** | Often built-in | Optional / app-level (e.g. Echo Live) |

So the main **tradeoff** of the custom stack is:

- **NAT traversal:** Today, voice goes **client → server → other clients**. There is no STUN/TURN for **direct** UDP between two clients behind NAT. Adding that would mean either:
  - A small STUN/TURN-style relay (e.g. server or dedicated process) that forwards UDP when direct path fails, or
  - Reusing WebRTC only for signaling + TURN and sending raw UDP when the path is known to be direct.

---

## 5. Summary

- **Codec:** libopus (Opus) — same as WebRTC, full control.
- **I/O:** miniaudio (low-latency, WASAPI on Windows) instead of RtAudio; same role.
- **Networking:** UDP (Boost.Asio) for voice when possible; TCP fallback for reliability.
- **Jitter buffer & PLC:** Implemented in the engine; Opus FEC + loss decoding.
- **Missing for “direct P2P” through NAT:** STUN/TURN or another form of NAT traversal for UDP (or a relay that forwards UDP between peers when needed).

This is the custom voice pipeline; the only big piece not implemented end-to-end is NAT traversal for direct peer-to-peer UDP.

---

## Deployment

- **Ports:** 5555/tcp (main app), 5556/udp (voice). No WebRTC ports (8080, 50000:60000) are required.
- **Control panel:** Use **1. Setup server** (deps, PM2, ufw 5555/tcp and 5556/udp), then **2. Deploy TalkMe server** (upload server.cpp + Protocol.h, compile, pm2 start). Optionally **0. Clean VPS and redeploy** to wipe the remote folder and re-run setup + deploy. See the control panel README for SSH config (host, username, password, remote_path, local_path).

---

## NAT traversal (current design)

Voice is **server-relayed only**: client → server → other clients. There is no STUN/TURN for direct UDP between two clients behind NAT. Adding a small STUN/TURN-style relay or UDP relay is possible as a future enhancement; the current stack is designed to work without it.
