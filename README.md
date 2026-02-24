# TalkMe

<p align="center">
  <img src="https://img.shields.io/badge/language-C%2B%2B-blue?style=flat-square" alt="C++">
  <img src="https://img.shields.io/badge/platform-Windows-0078D6?style=flat-square&logo=windows" alt="Windows">
  <img src="https://img.shields.io/badge/renderer-DirectX%2011-black?style=flat-square" alt="DirectX 11">
  <img src="https://img.shields.io/badge/audio-Opus%20%2B%20miniaudio-orange?style=flat-square" alt="Opus + miniaudio">
  <img src="https://img.shields.io/badge/license-GPL--3.0-blue?style=flat-square" alt="GPL-3.0 License">
</p>

**Lightweight, zero-bloat voice messenger client** — native C++, native GPU rendering, native audio. No Electron, no Chromium. Designed for speed, simplicity, and minimal resource use.

---

## Download

**[Releases](https://github.com/bohdanbtw/TalkMe/releases)** — download the latest `TalkMe.exe` (Windows x64). No installer; run and connect to a TalkMe server.

---

## Why TalkMe?

Most voice apps ship hundreds of megabytes of runtime just to show a chat window. TalkMe is a **single native executable**: it launches in under a second, idles at ~12–15 MB RAM, and keeps CPU near zero when you’re not talking.

| Metric        | TalkMe           | Typical Electron app |
|---------------|------------------|------------------------|
| Cold start    | **&lt; 1 s**     | 3–8 s                  |
| Idle RAM      | **~12–15 MB**    | 200–400 MB             |
| Size          | **~3 MB**        | 80–150 MB              |
| CPU idle      | **&lt; 0.1%**    | 1–3%                   |
| Voice latency | **&lt; 30 ms**   | 50–120 ms              |

---

## Features

### Voice
- **Opus codec** — low-bitrate clear audio (e.g. 32 kbps).
- **Adaptive jitter buffer** — smooths network variance (e.g. 80–300 ms).
- **Self mute & deafen** — UI and hotkeys; status synced to server.
- **Per-user volume** — right-click a user to adjust; saved in config.
- **Noise suppression** — optional; setting persisted across restarts.
- **Voice call info (INF)** — average/last ping (UDP RTT), packet loss %, live ping graph.

### Servers & channels
- **Create or join servers** via invite codes.
- **Text and voice channels** per server.
- Voice state and track cleanup when users leave so reconnects work correctly.

### In-game overlay
- **Anti-cheat friendly** — Win32 layered window + GDI+; no injection.
- Always on top, click-through, configurable corner and opacity.
- Redraws only when the member list changes.

### Customization
- **Themes** — built-in themes and accent colors.
- **Hotkeys** — e.g. Ctrl+Shift+M for mute/deafen.
- **Audio devices** — select mic/speaker at runtime.
- **Config** — under `%LocalAppData%\TalkMe`; session tokens DPAPI-encrypted.

---

## Tech stack

| Layer   | Technology |
|--------|------------|
| UI     | [Dear ImGui](https://github.com/ocornut/imgui) on DirectX 11 |
| Audio  | [miniaudio](https://miniaud.io/) + Opus (libopus) |
| Net    | [Asio](https://think-async.com/Asio/) (standalone) |
| Overlay| Win32 + GDI+ |

Dependencies are vcpkg-managed and statically linked. No extra runtime or service.

---

## Building (client)

**Requirements:** Windows 10+ (x64), Visual Studio 2022+ with C++ desktop workload, [vcpkg](https://vcpkg.io/) (or VS vcpkg integration).

```bash
git clone https://github.com/bohdanbtw/TalkMe.git
cd TalkMe
```

1. Open **TalkMe.slnx** (or **TalkMe.vcxproj**).
2. Restore vcpkg: **Project → Manage NuGet Packages** or `vcpkg install` in project root (manifest mode).
3. Build **Release | x64**.

Or from a **Developer Command Prompt**:

```bash
MSBuild TalkMe.vcxproj /p:Configuration=Release /p:Platform=x64
```

Output: **x64/Release/TalkMe.exe**. Build dirs and vcpkg output are gitignored.

**Note:** The **server** is not part of this repository (`server/` is in `.gitignore`). This repo is the **client only**. You need a compatible TalkMe server to connect to.

---

## Project structure

```
TalkMe/
├── src/
│   ├── app/       # Main loop, window, state
│   ├── audio/     # AudioEngine (miniaudio + Opus)
│   ├── core/      # ConfigManager, Logger
│   ├── network/   # TCP client, UDP voice, protocol
│   ├── overlay/   # In-game overlay (Win32 + GDI+)
│   ├── shared/    # Protocol (client/server)
│   └── ui/        # ImGui views, styles, theme
├── vendor/        # Third-party (ImGui, miniaudio, qrcodegen, etc.)
├── TalkMe.rc      # Icons + version info
├── Version.h      # Single version (exe + Settings UI)
├── TalkMe.vcxproj
├── TalkMe.slnx
└── vcpkg.json     # Dependencies
```

---

## Changelog

See **[CHANGELOG.md](CHANGELOG.md)** for version history and what changed between releases.

---

## License

[GPL-3.0](LICENSE) — open source; derivatives must stay under GPL. Copyright (C) 2026 bohdanbtw.
