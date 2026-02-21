# Voice pipeline debug and trace

Use these logs to see where the voice path fails (capture → encode → send → server → relay → receive → decode → play).

## Ports

- **5555/tcp** — Main app (auth, channels, voice over TCP fallback). Control panel overview shows established connections.
- **5556/udp** — Voice UDP. Server listens here; clients send Hello and voice here. Overview shows "listening" when the server has the socket bound.

If 5556 shows "not listening", the server is not bound to UDP (wrong binary, or port in use). Ensure firewall allows 5556/udp (control panel "1. Setup server" opens it).

## Client trace (talkme_voice_trace.log)

- **Where:** Next to the executable (e.g. `D:\...\x64\Release\talkme_voice_trace.log`). When trace is enabled, the app prints `Voice trace log: <path>` to the console on startup.
- **In Debug builds:** Trace is always enabled.
- **In Release:** Enable in one of two ways:
  1. Set env `TALKME_VOICE_TRACE=1` (or `y`/`Y`) before starting (e.g. in Visual Studio: Project → Debugging → Environment), **or**
  2. Create an empty file named `talkme_voice_trace.enable` in the same folder as the .exe, then run the exe (no env needed). Delete the file to disable.

**Pipeline steps (each line starts with `step=` so you can tell where in the chain things are):**

| step | Meaning | Key fields |
|------|--------|------------|
| `step=capture` | Mic captured one frame (sampled every 50th) | pcm_samples=480, seq= |
| `step=encode` | Opus encoded that frame (sampled every 50th) | pcm_samples=480, opus_bytes=, seq= |
| `step=send` | Packet sent to network (first 5 + every 20th) | path=udp\|tcp, seq=, opus_bytes=, payload_bytes= |
| `step=recv` | Packet received from network | path=udp\|tcp, sender=, seq=, opus_bytes= |
| `step=decode` | Opus decoded to PCM (sampled every 50th) | sender=, opus_bytes=, samples_out=480 |
| `step=plc` | Packet loss concealment used (gap in sequence) | sender=, missing_frames=, expected_seq=, got_seq= |
| `step=buffer` | Decoded frame pushed to jitter buffer (every 100th) | sender=, buffer_frames=, buffer_ms= |
| `step=play` | Playback callback: frames sent to device (every 250th) | frames=, active_tracks= |
| `step=telemetry` | Periodic health (every ~5s) | recv=, loss=, loss_pct=, jitter_ms=, buffer_ms=, underruns=, overflows=, bitrate_kbps= |

Other client lines (no step): `udp_start ok|fail`, `hello_sent user=... channel=...`.

**How to use:** Compare `opus_bytes` from encode → send → (server) → recv → decode to see if the codec/transport changed the payload. Use `step=plc` and `step=telemetry` (loss_pct, underruns) to see if the problem is transport (loss) vs playback (buffer underruns). If you only see `step=recv path=tcp`, the server is not sending you voice over UDP.

## Server trace (voice_trace.log)

- **Where:** Server working directory (e.g. `remote_path` on the VPS), file: `voice_trace.log`.
- **Enable:** Start the server with `VOICE_TRACE=1` in the environment. With PM2 you can do:
  - `VOICE_TRACE=1 pm2 start ./talkme_server --name talkme ...`  
  or add `VOICE_TRACE: '1'` to the app’s `env` in your ecosystem file and restart.

**Server pipeline steps (each line starts with `step=`):**

| step | Meaning | Key fields |
|------|--------|------------|
| `step=udp_voice_recv` | Server received UDP voice packet (parsed OK) | sender=, payload_bytes= |
| `step=udp_hello_ok` | Server accepted UDP Hello; client is bound for UDP voice | user=, cid=, bindings=N |
| `step=udp_hello_drop` | Hello rejected | reason=invalid_cid_or_user\|session_not_found\|channel_mismatch |
| `step=udp_voice_relay` | Voice packet relayed to clients | sender=, cid=, relay_tcp=N, relay_udp=M, payload_bytes= |
| `step=udp_voice_drop` | Voice packet dropped (not relayed) | reason=parse_fail\|sender_not_bound\|endpoint_mismatch |

**Interpretation:** If you see `step=udp_voice_drop reason=sender_not_bound` often, the client’s UDP Hello wasn’t accepted or expired (bindings=0). If `step=udp_voice_relay` shows relay_udp=0, no other client had a UDP binding, so they get voice only over TCP.

## Control panel

- **Overview:** Shows 5555/tcp connection count and whether 5556/udp is listening.
- **Deploy → "Download server voice trace":** Downloads `voice_trace.log` from the server to `local_path/voice_trace_server.log` (create the file first by running the server with `VOICE_TRACE=1`).

## Checklist when voice is bad (loss, stutter, path=TCP)

1. **Overview:** Is 5556/udp "listening"? If not, fix server/firewall so the server binds to 5556.
2. **Client trace:** Do you see `udp_start ok`? If not, client is on TCP only.
3. **Client trace:** Do you see `hello_sent` after joining a voice channel? If not, client isn’t sending Hello (check that you’re in a channel and UDP started).
4. **Server trace:** Do you see `udp_hello ok` for your user? If not, server isn’t registering your UDP (wrong user/channel or Hello not reaching server).
5. **Server trace:** For your voice, do you see `udp_voice relay ... relay_udp=...` with relay_udp ≥ 1? If relay_udp is always 0, other clients aren’t bound over UDP.
6. **Client trace:** Do you see `recv path=udp`? If only `recv path=tcp`, server isn’t sending you voice over UDP (often because your Hello wasn’t accepted or firewall blocks 5556/udp to the client).
