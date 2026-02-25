# AGENTS.md

## Cursor Cloud specific instructions

### Project overview

TalkMe is a **Windows-only native C++ voice messenger client** using DirectX 11 + Dear ImGui for rendering, miniaudio + Opus for audio, and Asio for networking. The codebase is the **client only**; the server is not included. See `README.md` for full architecture and feature details.

### Platform constraint

This project **cannot be compiled or run on Linux**. It requires:
- Windows 10+ (x64)
- Visual Studio 2022+ with the C++ desktop workload (PlatformToolset `v145`)
- DirectX 11 SDK (included in Windows SDK)

The entry point (`src/app/main.cpp`) uses `WinMain`, and virtually all source files depend on Win32 APIs (`<Windows.h>`, `<d3d11.h>`, GDI+, DPAPI, etc.).

### Build system

- **MSBuild** via `TalkMe.vcxproj` / `TalkMe.slnx` (Visual Studio solution)
- **vcpkg** in manifest mode (`vcpkg.json`) manages: `asio`, `opus`, `nlohmann-json`
- PowerShell build script: `build.ps1` (wraps MSBuild for Release x64)
- Target config for release: `Release | x64` with triplet `x64-windows-static`

### What works on the Linux Cloud VM

Since the project is Windows-only, Cloud VM development is limited to:

| Task | Command | Notes |
|------|---------|-------|
| **Static analysis** | `cppcheck --suppress=missingInclude --suppress=missingIncludeSystem --std=c++20 --language=c++ --force --max-configs=1 -I src -I vendor src/` | Runs all 15 source files |
| **Format check** | `clang-format --dry-run --Werror <file>` | No `.clang-format` config in repo; uses default style which differs from project conventions |
| **Dependency resolution** | `vcpkg install --triplet x64-linux` (from repo root) | Installs cross-platform deps (asio, opus, nlohmann-json) for code intelligence |

### Tests

There are **no automated tests** (no test framework, no test files) in this codebase.

### Key gotchas

- The `vcpkg_installed/` directory is gitignored; regenerate it with `vcpkg install` in the repo root.
- vcpkg requires the `VCPKG_ROOT` env var pointing to the vcpkg installation (e.g. `/opt/vcpkg`) and that location on `PATH`.
- On the Cloud VM, you may need `sudo ln -sf /usr/lib/x86_64-linux-gnu/libstdc++.so.6 /usr/lib/x86_64-linux-gnu/libstdc++.so` for vcpkg's CMake compiler detection to succeed (the symlink may be missing).
- `Logger.cpp` exists in `src/core/` but is **not** listed in `TalkMe.vcxproj` — `Logger.h` is header-only with all implementation inline.
- The server runs on TCP port 5555 (signaling/text) and UDP port 5556 (voice). The hardcoded server IP is in `Application.h`.
