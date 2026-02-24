# Changelog

All notable changes to the **TalkMe client** are documented here. The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project uses [semantic versioning](https://semver.org/) (major.minor.patch).

---

## [1.3.0] — 2026-02-24

### Added

- **Exe version and metadata** — `TalkMe.exe` now has proper Windows properties (Details tab): product name **TalkMe**, file/product version **1.3.0**, description *"TalkMe - Lightweight voice messenger"*, and copyright **Copyright (C) 2026 bohdanbtw**. Version is defined in a single place (`Version.h`) and shown both in the exe and in Settings (e.g. *"TalkMe v1.3.0"*).
- **Noise suppression persistence** — The noise suppression (on/off) setting is saved to config and restored on startup; it is also respected when using “Reset to defaults” (default off).
- **GPL-3.0 license** — The client is now explicitly licensed under GPL-3.0. See [LICENSE](LICENSE). README badge updated to GPL-3.0.

### Changed

- **Application exit and shutdown** — Exit behavior was made reliable: the main loop now waits on a dedicated quit event signaled from the window procedure (e.g. on close), uses a short wait timeout so the app doesn’t hang, and cleans up in a safe order (D3D/ImGui released before joining threads or shutting down the logger).
- **Logger shutdown** — Logger shutdown was fixed to avoid deadlock: the logging mutex is released before the logging thread is joined, so the main thread no longer blocks indefinitely on exit.
- **README** — Rewritten to be client-only: download link to Releases, accurate feature list, correct project structure (no server in repo), and correct license (GPL-3.0). Server is documented as separate and not included in this repository.
- **Changelog** — This file was added so users can see what changed between releases.

### Fixed

- **Voice track state on user leave** — When a remote user leaves a channel, their voice track state (decoder, sequence, jitter buffer) is now cleared. Rejoining no longer causes their packets to be dropped due to stale sequence state.
- **INF panel TCP echo loss** — Echo request/response sequence numbers use the correct byte order; TCP echo loss is no longer stuck at 100%.
- **INF panel: realtime ping instead of TCP loss graph** — The Voice Call Info (INF) panel now shows:
  - **Average ping** — Mean of recent UDP RTT samples (ms).
  - **Last ping** — Most recent UDP ping–pong RTT (ms).
  - **Loss of packets** — Voice packet loss from telemetry (%).
  - **Realtime ping graph** — Plot of UDP RTT over the last ~60 samples.

### Technical

- **Per-user volume persistence** — User volume sliders are saved to `user_volumes.json` in the config directory (`%LocalAppData%\TalkMe`) and restored on startup; gains are applied in the audio engine at init.
- **UDP ping history** — Voice transport keeps a rolling history of RTT samples for the INF ping graph and average; APIs exposed for the UI (`GetLastRttMs()`, `GetPingHistory()`).
- **Versioning rule** — Version bumps: patch (x.x.X+1) for small bug fixes, minor (x.X+1.0) for new features/medium work, major (X+1.0.0) for big or breaking changes. Single source of truth: `Version.h`.

---

## [1.0.0] — Initial release

### Added

- **Native C++ client** — Single executable, no Electron or web stack. DirectX 11 + ImGui for UI, miniaudio + Opus for audio, Asio for networking.
- **Voice communication** — Opus codec, adaptive jitter buffer, mute/deafen, per-user volume (in-session), voice state sync across channels.
- **Servers and channels** — Create/join servers via invite codes; text and voice channels; instant voice state when switching channels.
- **In-game overlay** — Win32 layered window + GDI+ (no injection). Always on top, click-through, configurable corner and opacity; redraws only when member list changes.
- **Theming and hotkeys** — Built-in themes and accent colors; configurable hotkeys (e.g. Ctrl+Shift+M for mute/deafen).
- **Audio device selection** — Choose microphone and speaker at runtime.
- **Config and session** — Settings and DPAPI-encrypted session tokens under `%LocalAppData%\TalkMe`.
- **Voice Call Info (INF) panel** — Basic visibility into connection quality (evolved in 1.3.0 with UDP ping and graph).

### Technical

- **Build** — Visual Studio 2022+, vcpkg for Opus, nlohmann-json, etc. Release x64 output: `x64/Release/TalkMe.exe`.
- **Repo** — Client-only; `server/` is not part of the repository (in `.gitignore`). Build artifacts and vcpkg output are gitignored.

---

## Version numbering

- **Patch (1.3.X)** — Minor bug fixes, small tweaks, no new features.
- **Minor (1.X.0)** — New features, medium refactors, notable improvements.
- **Major (X.0.0)** — Large updates, breaking changes, major redesign.

[1.3.0]: https://github.com/bohdanbtw/TalkMe/releases/tag/v1.3.0
[1.0.0]: https://github.com/bohdanbtw/TalkMe/releases/tag/v1.0.0
