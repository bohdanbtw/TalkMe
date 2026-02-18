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
|---|---|---|
| Cold start | **< 1 s** | 3 -- 8 s |
| Idle RAM | **~12 -- 15 MB** | 200 -- 400 MB |
| Installer size | **~3 MB** | 80 -- 150 MB |
| CPU idle | **< 0.1 %** | 1 -- 3 % |
| Voice latency | **< 30 ms** | 50 -- 120 ms |

---

## Features

### Voice Communication
- **Opus codec** -- studio-grade compression at 32 kbps delivers crystal clear audio while consuming almost no bandwidth.
- **Adaptive jitter buffer** -- dynamically adjusts between 80 ms and 300 ms to smooth out network variance without adding perceptible delay.
- **Self mute & deafen** -- toggle your mic or headset with a single click or a custom keyboard shortcut. Status icons are visible to all participants.
- **Per-user volume control** -- right-click any participant to raise or lower their volume independently.
- **Live voice statistics** -- real-time telemetry overlay showing ping, packet loss, jitter, buffer depth, and bitrate.
- **Join / leave sounds** -- lightweight audio cues generated at startup (no external sound files needed).

### Servers & Channels
- **Create or join servers** with invite codes.
- **Text & voice channels** within each server.
- Seamless switching between channels with instant voice state synchronization.

### In-Game Overlay
- **Anti-cheat safe** -- uses a standard Win32 layered window with GDI+ rendering. No DLL injection, no hooking, no memory patching.
- **Always on top, click-through, transparent** -- see who's talking without leaving your game.
- Configurable corner position (TL / TR / BL / BR) and opacity slider.
- Near-zero GPU impact -- renders only when the member list changes.

### Customization
- **Theme engine** -- switch between built-in themes or tweak accent colors.
- **Hotkey combinations** -- bind multi-key shortcuts (e.g. `Ctrl+Shift+M`) for mute and deafen.
- **Audio device selection** -- pick your preferred microphone and speaker at runtime; no restart required.
- Persistent settings saved locally with DPAPI-encrypted session tokens.

---

## Tech Stack

| Layer | Technology |
|---|---|
| UI | [Dear ImGui](https://github.com/ocornut/imgui) on DirectX 11 |
| Audio I/O | [miniaudio](https://miniaud.io/) (single-header, zero dependencies) |
| Voice Codec | [Opus](https://opus-codec.org/) (libopus) |
| Networking | [Asio](https://think-async.com/Asio/) (standalone, non-Boost) |
| Overlay | Win32 + GDI+ (no injection) |
| Database (server) | SQLite 3 |

Every dependency is either header-only or statically linked. There is **no runtime installer**, no framework prerequisite, and no background service.

---

## Building

### Requirements

- **Windows 10+** (x64)
- **Visual Studio 2022+** with the "Desktop development with C++" workload
- DirectX 11 SDK (included with the Windows SDK)

### Steps

```
git clone https://github.com/bohdanbtw/TalkMe.git
cd TalkMe
```

Open **`TalkMe.slnx`** in Visual Studio, select **Release | x64**, and build (`Ctrl+Shift+B`).

Or from a Developer Command Prompt:

```
MSBuild TalkMe.vcxproj /p:Configuration=Release /p:Platform=x64
```

The output binary is placed in `x64/Release/TalkMe.exe`.

### Server

The self-hosted server lives in the `server/` directory. Compile it on any platform with a C++17 compiler, Asio, and SQLite:

```
g++ -std=c++17 -O2 -o talkme-server server.cpp -lsqlite3 -lpthread
./talkme-server
```

The server listens on port **5555** by default.

---

## Project Structure

```
TalkMe/
├── src/
│   ├── app/            # Application entry point, main loop, state machine
│   ├── audio/          # AudioEngine (miniaudio + Opus encode/decode)
│   ├── core/           # ConfigManager, Logger
│   ├── network/        # TCP client, UDP voice transport, packet protocol
│   ├── overlay/        # In-game overlay (Win32 + GDI+)
│   └── ui/             # ImGui views, styles, theming
├── server/             # Standalone C++ server (Asio + SQLite)
├── vendor/             # Third-party headers (ImGui, miniaudio, Asio, Opus, etc.)
├── TalkMe.vcxproj      # Visual Studio project
└── TalkMe.slnx         # Visual Studio solution
```

---

## Contributing

Contributions are welcome. Fork the repo, create a feature branch, and open a pull request. Please keep changes focused and avoid introducing heavy external dependencies.

---

## License

This project is released under the [MIT License](LICENSE).
