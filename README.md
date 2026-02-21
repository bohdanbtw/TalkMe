<p align="center">
  <img src="https://img.shields.io/badge/language-C%2B%2B-blue?style=flat-square" alt="C++">
  <img src="https://img.shields.io/badge/platform-Windows-0078D6?style=flat-square&logo=windows" alt="Windows">
  <img src="https://img.shields.io/badge/renderer-DirectX%2011-black?style=flat-square" alt="DirectX 11">
  <img src="https://img.shields.io/badge/audio-Opus%20%2B%20miniaudio-orange?style=flat-square" alt="Opus + miniaudio">
  <img src="https://img.shields.io/badge/license-MIT-green?style=flat-square" alt="MIT License">
</p>

<h1 align="center">TalkMe</h1>

<p align="center">
  <b>Lightweight, zero-bloat voice messenger built entirely in C++.</b><br>
  Designed for users who value speed, simplicity, and minimal resource footprint.
</p>

---

## Why TalkMe?

Most voice apps ship hundreds of megabytes of Electron/Chromium runtime just to render a chat window. TalkMe takes the opposite approach: **native C++, native GPU rendering, native audio**. The result is an app that launches instantly, idles under **15 MB of RAM**, and never spins your CPU when you're not talking.

| Metric | TalkMe | Typical Electron App |
|--------|--------|----------------------|
| Cold start | **< 1 s** | 3 – 8 s |
| Idle RAM | **~12 – 15 MB** | 200 – 400 MB |
| Installer size | **~3 MB** | 80 – 150 MB |
| CPU idle | **< 0.1 %** | 1 – 3 % |
| Voice latency | **< 30 ms** | 50 – 120 ms |

---

## Features

### Voice Communication
- **Opus codec** — studio-grade compression at 32 kbps for clear audio with minimal bandwidth.
- **Adaptive jitter buffer** — 80–300 ms range to smooth network variance without noticeable delay.
- **Self mute & deafen** — one-click or custom hotkey; status visible to all participants.
- **Per-user volume control** — right-click any participant to adjust their volume; settings persist across restarts (`user_volumes.json` in config directory).
- **Voice Call Info (INF)** — realtime average ping, last ping, packet loss %, and a live ping graph (UDP RTT).
- **Join / leave sounds** — lightweight cues generated at startup (no external sound files).

### Servers & Channels
- **Create or join servers** with invite codes.
- **Text & voice channels** per server.
- Instant voice state sync when switching channels; remote track state is cleared when users leave so reconnects work correctly.

### In-Game Overlay
- **Anti-cheat safe** — Win32 layered window + GDI+; no DLL injection or hooking.
- **Always on top, click-through, transparent** — see who’s talking without leaving the game.
- Configurable corner (TL/TR/BL/BR) and opacity.
- Minimal GPU use; only redraws when the member list changes.

### Customization
- **Theme engine** — built-in themes and accent colors.
- **Hotkeys** — multi-key shortcuts (e.g. Ctrl+Shift+M) for mute and deafen.
- **Audio device selection** — change mic/speaker at runtime.
- Persistent settings with DPAPI-encrypted session tokens; config stored under `%LocalAppData%\TalkMe`.

---

## Tech Stack

| Layer | Technology |
|-------|------------|
| UI | [Dear ImGui](https://github.com/ocornut/imgui) on DirectX 11 |
| Audio I/O | [miniaudio](https://miniaud.io/) (single-header) |
| Voice codec | [Opus](https://opus-codec.org/) (libopus) |
| Networking | [Asio](https://think-async.com/Asio/) (standalone, non-Boost) |
| Overlay | Win32 + GDI+ |
| Server DB | SQLite 3 |

Dependencies are header-only or statically linked. No runtime installer or background service.

---

## Building

### Requirements
- **Windows 10+** (x64)
- **Visual Studio 2022+** with “Desktop development with C++”
- **vcpkg** (for Opus, nlohmann-json, etc.) — install from [vcpkg](https://vcpkg.io/) and ensure it’s in your PATH, or use VS’s built-in vcpkg support
- DirectX 11 (Windows SDK)

### Steps

```bash
git clone https://github.com/bohdanbtw/TalkMe.git
cd TalkMe
```

1. Open **`TalkMe.slnx`** in Visual Studio (or use **`TalkMe.vcxproj`**).
2. Restore vcpkg dependencies (if needed): **Project → Manage NuGet Packages** or run `vcpkg install` from the project root with manifest mode.
3. Select **Release | x64**, then build (`Ctrl+Shift+B`).

From a **Developer Command Prompt**:

```bash
MSBuild TalkMe.vcxproj /p:Configuration=Release /p:Platform=x64
```

Output: **`x64/Release/TalkMe.exe`**. Build artifacts (`x64/`, `vcpkg_installed/`, logs) are gitignored.

### Server

The C++ server in `server/` uses Asio and SQLite:

```bash
g++ -std=c++17 -O2 -o talkme-server server/server.cpp -lsqlite3 -lpthread
./talkme-server
```

Default port: **5555** (TCP); voice UDP on **5556**.

---

## Project Structure

```
TalkMe/
├── src/
│   ├── app/          # Application, main loop, state
│   ├── audio/        # AudioEngine (miniaudio + Opus)
│   ├── core/         # ConfigManager, Logger
│   ├── network/      # TCP client, UDP voice transport, protocol
│   ├── overlay/      # In-game overlay (Win32 + GDI+)
│   ├── shared/       # Protocol (shared client/server)
│   └── ui/           # ImGui views, styles, theme
├── server/           # Standalone C++ server (Asio + SQLite)
├── vendor/           # Third-party headers (ImGui, miniaudio, etc.)
├── docs/             # Architecture and specs
├── TalkMe.vcxproj
├── TalkMe.slnx
└── vcpkg.json        # Manifest dependencies
```

---

## Contributing

Contributions are welcome. Fork the repo, use a feature branch, and open a pull request. Keep changes focused and avoid heavy new dependencies.

---

## License

[MIT License](LICENSE).
