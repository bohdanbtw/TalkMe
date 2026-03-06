# Changelog

All notable changes to the **TalkMe client** are documented here. The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project uses [semantic versioning](https://semver.org/) (major.minor.patch).

---

## [1.4.2] — 2026-03-06

### Added

- **Media/GIF preview when attached** — When you attach an image (paste or drag-and-drop) or a GIF to the compose bar, a thumbnail preview (up to 96×96 px) is shown next to the "Image attached" / "GIF attached" label. GIFs animate in the preview; images show the pasted or dropped file. Preview is cleared when you remove the attachment or send the message.

### Changed

- **Drag-and-drop overlay** — The "Drop images here" overlay is back: when you drag files over the chat window, the overlay appears so you know where to drop. Dropping still attaches one image per drop to the compose bar (unchanged). Overlay is only shown while dragging (`IsDragOver()`), so it no longer blocks Send or reactions.

### Fixed

- **Ctrl+V paste image** — Pasting an image from the clipboard (Ctrl+V) then sending no longer showed "attachment failed to load". The BMP built from the clipboard DIB now uses the correct pixel data offset for 16-bit and 32-bit bitmaps with BI_BITFIELDS (three DWORD masks after the header), so the uploaded file is valid and decodes correctly when the message is displayed.

### Technical

- **Compose preview texture** — Application uploads the pending attached image to TextureManager as `compose_attached` when set (paste or drop); ChatView draws it as a thumbnail. Texture is removed on clear or send. GIF preview uses the same GetAnimatedSrv + ImageCache path as inline chat GIFs with texture id `compose_gif`.

---

## [1.4.1] — 2026-03-03

### Added

- **Game Mode** (Settings → Performance) — When turned on, the app runs in a chat-only, maximum-efficiency mode: no images, GIFs, or screen sharing in the UI; window runs at 1 FPS to minimize CPU. Toggling the setting triggers an invisible relaunch (window reopens in the same position/size) so the new mode applies cleanly without extra code paths.
- **Invisible relaunch** — Toggling Game Mode saves the setting, writes the current window rect to config, spawns the executable with `--relaunch-instead`, and exits. The new process waits for the old one to release the single-instance mutex, then starts with the updated config and restores the window position.

### Changed

- **GIF picker on close** — Closing the GIF panel (F4, click outside, or popup close) now always runs `OnClosed()`: all picker textures and ImageCache entries are cleared so RAM is freed. Reopening the panel shows the same grid as skeletons, then GIFs re-load from disk/network.
- **Chat GIF rendering** — Visible chat image URLs are collected and requested in a first pass before rendering, and protected from cache eviction, so inline GIFs in messages render even when they were not previously opened in the GIF picker.
- **Game mode: 1 FPS** — When Game Mode is on and the user is in the main app (not in a voice channel), the main loop uses a 1000 ms wait between frames for maximum efficiency.

### Fixed

- **Chat GIFs after turning Game Mode off** — Chat GIF state is now reset whenever the texture is missing (e.g. evicted during Game Mode), so GIFs in messages render again after disabling Game Mode instead of staying blank.
- **GIF memory** — Picker uses `GetImage` instead of `GetImageCopy` when uploading to the GPU to avoid doubling decoded frame memory; chat static image path calls `ReleaseGifPixels` after upload.

### Technical

- **Relaunch flow** — `main.cpp` parses `--relaunch-instead`; if the single-instance mutex is already held, the new process waits in a loop for it, then reads `relaunch_rect.txt` (x, y, w, h), creates `Application` with that size and restore position, and deletes the file. `Application::RequestRelaunch()` writes the rect, spawns the process with the flag, and calls `ExitProcess(0)`. `AppWindow::SetPosition(x, y)` restores the window position after Create.

---

## [1.4.0] — 2026-02-28

### Added

- **Friends button in server rail** — The Friends entry is now in the left server rail (same column as the "+" and server icons). It uses the app icon from the assets folder (`app_48x48.ico`) when available, with a "Fr" text fallback. Selection is shown with the same accent bar as the selected server.
- **GIF panel: click outside to close** — When the GIF/Emotions panel is open, clicking anywhere outside the panel (chat area, sidebar, etc.) closes it.
- **Secrets folder in .gitignore** — The entire `secret/` directory is ignored; `secret/secrets.example` remains trackable as a template.

### Changed

- **Sidebar layout** — Friends was removed from the channel list. The top-of-sidebar Friends button and the "+" button for adding channels were removed. The "+" next to "TEXT CHANNELS" and "VOICE CHANNELS" was removed; the server rail "+" (create/join server) is unchanged.
- **GIF panel size and position** — The GIF/Emotions panel no longer takes the full content area. It uses ~30% of the content width and is right-aligned; the chat area uses the remaining ~70% when the panel is open.
- **Emotions panel tabs** — The active tab (Emoji / Stickers / GIFs) uses accent styling and a fixed height (32px) for a clearer selected state.
- **Footer** — Long usernames are truncated with an ellipsis (full name in tooltip). Mic, Spk, and Info buttons use shorter labels and consistent padding so they are not cut off.
- **GIF picker UI** — The redundant "GIFs" heading under the tabs was removed; the tab label is sufficient.

### Fixed

- **GIF panel rendering when idle** — The GIF panel and animated GIFs now update continuously when the panel is open. The main loop uses a 16 ms wait when the GIF panel is visible so redraws occur without mouse movement.

### Technical

- **App icon for Friends** — The app icon is loaded from `%LocalAppData%\TalkMe\assets\app_48x48.ico` (or exe-relative `src/assets`) via GDI (`LoadImage`, `CreateDIBSection`, `DrawIconEx`), converted to RGBA, and registered in TextureManager as `friends_icon`. Loaded once after the D3D device is set.
- **.gitignore** — `secret/` added; `!secret/secrets.example` keeps the example file trackable.

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

[1.4.2]: https://github.com/bohdanbtw/TalkMe/releases/tag/v1.4.2
[1.4.1]: https://github.com/bohdanbtw/TalkMe/releases/tag/v1.4.1
[1.4.0]: https://github.com/bohdanbtw/TalkMe/releases/tag/v1.4.0
[1.3.0]: https://github.com/bohdanbtw/TalkMe/releases/tag/v1.3.0
[1.0.0]: https://github.com/bohdanbtw/TalkMe/releases/tag/v1.0.0
