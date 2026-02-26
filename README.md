# TalkMe

<p align="center">
  <img src="https://img.shields.io/badge/language-C%2B%2B20-blue?style=flat-square" alt="C++20">
  <img src="https://img.shields.io/badge/platform-Windows%2010%2B-0078D6?style=flat-square&logo=windows" alt="Windows">
  <img src="https://img.shields.io/badge/renderer-DirectX%2011-black?style=flat-square" alt="DirectX 11">
  <img src="https://img.shields.io/badge/audio-Opus%20%2B%20miniaudio-orange?style=flat-square" alt="Opus + miniaudio">
  <img src="https://img.shields.io/badge/UI-Dear%20ImGui-teal?style=flat-square" alt="Dear ImGui">
  <img src="https://img.shields.io/badge/license-GPL--3.0-blue?style=flat-square" alt="GPL-3.0 License">
</p>

**TalkMe** is a lightweight, native C++ voice and text messenger. No Electron, no Chromium, no bloat. A single executable that launches in under a second, idles at ~15 MB RAM, and delivers sub-30ms voice latency.

---

## Performance

| Metric | TalkMe | Typical Electron App |
|---|---|---|
| Cold start | **< 1 s** | 3-8 s |
| Idle RAM | **~15 MB** | 200-400 MB |
| Executable size | **~4 MB** | 80-150 MB |
| CPU idle | **< 0.1%** | 1-3% |
| Voice latency | **< 30 ms** | 50-120 ms |

---

## Features

### Voice Communication
- **Opus codec** at adaptive bitrate (24-64 kbps) with RTCP-Lite quality feedback
- **Adaptive jitter buffer** (80-300ms) that auto-tunes to network conditions
- **Noise suppression** — RNNoise, Speex DSP, and WebRTC APM options
- **Self mute & deafen** with keybind support (Ctrl+Shift+M / Ctrl+Shift+D)
- **Per-user volume control** — right-click any user in voice to adjust
- **Mute/deafen broadcast** — all users see who is muted or deafened
- **UDP voice transport** with TCP fallback for restricted networks
- **Voice activity detection** with speaking indicators (green glow)
- **Voice call info panel** (INF button) — ping, loss, jitter, bitrate, live graph

### Text Chat
- **Real-time messaging** with instant delivery to all channel members
- **Message editing** — right-click your message → Edit → inline editor with Save/Cancel
- **Message deletion** — right-click your message → Delete
- **Message replies** — right-click any message → Reply → quoted preview above your reply
- **Message pinning** — right-click → Pin/Unpin, pinned messages show [pinned] tag
- **@mentions** — type `@username`, `@username#0001`, or `@all` for notifications
- **Mention highlighting** — mentions render in blue, mentioned users hear a distinct sound
- **Emoji reactions** — right-click → React with +1, <3, :), eyes, fire, GG
- **Clickable URLs** — links render in blue, click to open in browser
- **Inline image rendering** — image URLs (png/jpg/gif/webp) download and display inline
- **YouTube preview** — YouTube links show video thumbnail with click-to-watch
- **Typing indicators** — "User is typing..." shown below the input bar
- **Unread message badges** — channel sidebar shows unread count
- **Message search** — search bar filters messages by content or sender
- **/commands** — messages starting with `/` are sent as bot commands

### Screen Sharing
- **DXGI Desktop Duplication** — GPU-accelerated capture at 5-16ms per frame
- **H.264 hardware encoding** — NVENC/AMF/QuickSync with automatic fallback
- **JPEG fallback** — works on all systems including VMs and Win10 Enterprise
- **GDI fallback** — guaranteed compatibility on all Windows versions
- **Configurable FPS** — 30, 60, or 120 frames per second
- **Quality settings** — Low, Medium, High compression quality
- **System audio capture** — WASAPI loopback captures everything you hear
- **Multi-stream support** — multiple users can share simultaneously
- **Stream switching** — tab buttons to switch between active streams
- **Fullscreen mode** — maximize button on bottom-right, Exit to restore
- **Resolution scaling** — auto-scales to max 1920x1080

### Friends & Direct Messages
- **Friend requests** — add by username#tag, accept/reject incoming requests
- **Friends tab** — full-width panel with online/offline sections and status
- **Direct messages** — private 1:1 messaging with full chat history
- **Custom status** — set a status message visible to all friends
- **Online presence** — green/gray dots showing who's online

### Voice Calls (1:1)
- **Call friends** — click Call on any online friend
- **Incoming call popup** — centered overlay with Accept (green), Snooze 10m, Decline (red)
- **Ring sound** — plays every 2 seconds while incoming call is active
- **Call state tracking** — calling, ringing, active, ended

### Servers & Channels
- **Create servers** — custom name, auto-generated invite code
- **Join servers** — paste invite code to join
- **Text channels** — create unlimited text channels per server
- **Voice channels** — create voice channels with optional user limits
- **Cinema channels** — synchronized video watching with playback controls and queue
- **Channel deletion** — right-click → Delete Channel (server owner only)
- **Server management** — rename, delete, leave server (right-click server icon)
- **Member list** — toggleable right panel with online/offline status dots
- **Server invite** — Copy Invite button in channel header

### Cinema / Watch Party
- **Cinema channel type** — dedicated channels for synchronized video watching
- **Video queue** — add URLs to a shared queue, auto-plays in order
- **Playback controls** — Play, Pause, -10s, +10s, Next (synced for all viewers)
- **Queue management** — add videos with title, remove by index
- **State sync** — new viewers join with current playback position

### Admin & Moderation
- **Server owner = automatic admin** — creator has full permissions
- **Right-click admin actions** on voice members:
  - Mute User / Deafen User
  - Disconnect from Voice
  - Move to Channel (submenu with all voice channels)
  - Chat Mute (10 min / 1 hour)
  - Grant Admin
- **Permission system** — Delete Messages, Pin Messages, Kick Users, Admin flags
- **Sanctions** — timed chat mutes with expiry, stored in database
- **Role creation** — named roles with custom permissions and colors

### Minigames
- **Chess** — full engine with legal move validation, check/checkmate/stalemate, en passant, castling, promotion. Legal move highlighting, Unicode piece symbols. Challenge any user in voice channel.
- **Car Racing** — top-down elliptical track, WASD/arrow controls, lap counting, multiplayer position sync. Physics: acceleration, braking, friction, speed-dependent steering.

### Bot API
- **Programmable bots** — register bots with unique tokens per server
- **/command routing** — messages starting with `/` sent as Bot_Command packets
- **Bot messages** — bots can send messages to channels
- **Foundation** for music bots, AI assistants, moderation bots

### Profile
- **Avatars** — upload PNG/JPG profile pictures (max 200KB)
- **Circular rendering** — avatars displayed as circles in voice grid
- **Initials fallback** — shows first 2 letters when no avatar uploaded
- **2FA / TOTP** — enable two-factor authentication with QR code
- **Trusted devices** — skip 2FA on remembered devices

### In-Game Overlay
- **Always-on-top** — Win32 layered window, no injection, anti-cheat safe
- **Voice activity** — green dot + glow for speaking users
- **Mute/deafen icons** — shows M/D indicators for muted/deafened users
- **Configurable** — 4 corner positions, opacity slider (20-100%)
- **Proper UTF-8** — MultiByteToWideChar conversion for non-ASCII names

### GIF Integration
- **Tenor GIF search** — powered by Google's Tenor API v2
- **Search & Trending** — search by keyword or browse trending GIFs
- **Click to send** — GIF URL sent as a chat message, renders as clickable link
- **F4 shortcut** or [+] → GIF button in chat

### Notification Settings
- **Volume control** — adjustable notification volume (0-100%)
- **Mute @mentions** — disable mention notification sound
- **Mute messages** — disable new message notifications
- **Mute join/leave** — disable voice channel join/leave sounds
- **Distinct sounds** — different tones for join, leave, message, and mention

### Customization
- **Themes** — multiple built-in themes with live preview
- **Keybinds** — configurable mute/deafen hotkeys (supports key combos)
- **Audio devices** — select mic and speaker at runtime
- **Config persistence** — all settings saved to `%LocalAppData%\TalkMe`
- **Session encryption** — DPAPI-encrypted session tokens

---

## Tech Stack

| Layer | Technology |
|---|---|
| UI | [Dear ImGui](https://github.com/ocornut/imgui) on DirectX 11 |
| Audio | [miniaudio](https://miniaud.io/) + [Opus](https://opus-codec.org/) |
| Networking | [Asio](https://think-async.com/Asio/) (standalone, non-Boost) |
| Screen Capture | DXGI Desktop Duplication + Media Foundation H.264 |
| System Audio | WASAPI Loopback Capture |
| Overlay | Win32 + GDI+ layered window |
| Image Loading | stb_image |
| JSON | [nlohmann/json](https://github.com/nlohmann/json) |
| GIF API | Tenor API v2 (WinHTTP) |
| Noise Suppression | RNNoise / Speex DSP / WebRTC APM |
| Build | MSBuild (Visual Studio 2022+) + vcpkg |

All dependencies managed via **vcpkg** (manifest mode) and statically linked.

---

## Building

**Requirements:** Windows 10+ (x64), Visual Studio 2022+ with C++ desktop workload, [vcpkg](https://vcpkg.io/).

```bash
git clone https://github.com/bohdanbtw/TalkMe.git
cd TalkMe
```

1. Open **TalkMe.slnx** in Visual Studio
2. vcpkg auto-restores dependencies on build (manifest mode enabled)
3. Build **Release | x64**
4. Output: `x64/Release/TalkMe.exe`

### vcpkg Dependencies

Automatically resolved from `vcpkg.json`:
- `imgui` (with dx11 + win32 bindings)
- `opus` — audio codec
- `asio` — async networking
- `nlohmann-json` — JSON parsing
- `speexdsp` — noise suppression
- `libvpx` — VP8/VP9 video codec (fallback)
- `stb` — image loading

---

## Project Structure

```
TalkMe/
├── src/
│   ├── app/           # Application core, main loop, state management
│   ├── audio/         # AudioEngine, Opus codec, noise suppression
│   ├── core/          # ConfigManager, Logger
│   ├── game/          # Chess engine, Racing game
│   ├── network/       # TCP client, UDP voice, Tenor API, ImageCache
│   ├── overlay/       # In-game overlay (Win32 + GDI+)
│   ├── screen/        # DXGI capture, H.264 encoder/decoder, AudioLoopback
│   ├── shared/        # Protocol definitions, PacketHandler
│   └── ui/            # ImGui views, styles, themes, TextureManager
├── server/
│   └── src/           # Server: ChatSession, TalkMeServer, Database, Crypto
├── vendor/            # miniaudio, qrcodegen
├── vcpkg.json         # Dependencies manifest
├── TalkMe.vcxproj     # Visual Studio project
└── TalkMe.slnx        # Visual Studio solution
```

---

## Server

The server is a standalone C++ application using Asio + SQLite. It handles:
- User authentication with salted password hashing + TOTP 2FA
- Server/channel/message CRUD with SQLite persistence
- Voice packet relay (TCP + UDP) with token-bucket rate limiting
- Screen share frame relay
- Cinema channel state management
- Friend system with DM routing
- Online presence broadcasting
- Admin actions and sanctions

**Deploy:** Compile with `g++ -std=c++20 -O2` and run with PM2 or systemd.

---

## Keyboard Shortcuts

| Key | Action |
|---|---|
| F1 | Keyboard shortcuts help |
| F2 | Toggle Friends panel |
| F4 | Toggle GIF picker |
| Ctrl+Shift+M | Toggle microphone mute |
| Ctrl+Shift+D | Toggle deafen |
| Enter | Send message |
| Right-click message | Reply, Edit, Delete, Pin, React |
| Right-click channel | Delete channel |
| Right-click server | Copy invite, Rename, Delete, Leave |
| Right-click voice user | Admin: mute, deafen, disconnect, move, sanction |

---

## License

[GPL-3.0](LICENSE) — open source; derivatives must stay under GPL.

Copyright (C) 2026 bohdanbtw.
