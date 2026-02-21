# Release Notes

## Latest (Current)

### Voice & Audio
- **Voice track cleanup on leave** — When a remote user leaves a channel, their track state (decoder, sequence, buffer) is cleared. Rejoining no longer drops their packets due to stale `lastSeq`.
- **Per-user volume persistence** — User volume sliders are saved to `user_volumes.json` in the config directory (`%LocalAppData%\TalkMe`) and restored on startup. Gains are applied to the audio engine at init.

### Voice Call Info (INF Panel)
- **TCP echo loss fix** — Echo request/response sequence numbers use correct byte order; TCP echo loss no longer stuck at 100%.
- **Realtime ping instead of TCP loss graph** — INF panel now shows:
  - **Average ping: X ms** — Mean of recent UDP RTT samples.
  - **Last ping: X ms** — Most recent UDP ping–pong RTT.
  - **Loss of packets: X %** — Voice packet loss from telemetry.
  - **Realtime ping graph** — Plot of UDP RTT over time (last 60 samples, ~2 min at 2 s ping interval).

### Networking & Protocol
- **UDP ping history** — Voice transport keeps a rolling history of RTT samples for the ping graph and average; `GetLastRttMs()` and `GetPingHistory()` exposed for the UI.

### Build & Repo
- **.gitignore** — Excludes `vcpkg_installed/`, `user_volumes.json`, `logs.log`, `talkme_voice_trace.log`, and other runtime/log files so the repo stays clean.
- **README** — Updated with current features, INF panel, volume persistence, vcpkg, and project layout.

---

## Summary for GitHub Release

**Voice:** Track state cleared when users leave so reconnects work. Per-user volumes saved to `user_volumes.json`.

**INF panel:** TCP echo loss fixed (byte order). Replaced TCP loss graph with realtime ping: average ping, last ping, packet loss %, and ping graph.

**Repo:** README and .gitignore updated; runtime and log files excluded.
