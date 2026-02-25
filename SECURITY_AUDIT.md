# TalkMe Security & Bug Audit Report

**Date:** 2026-02-25
**Scope:** Client-side C++ voice messenger — 11 files audited
**Auditor:** Automated static analysis (Claude)

---

## Summary

| Severity | Count |
|----------|-------|
| CRITICAL | 5     |
| HIGH     | 12    |
| MEDIUM   | 11    |
| LOW      | 5     |
| INFO     | 3     |
| **Total**| **36**|

---

## CRITICAL Findings

### C-01: Systemic race conditions between audio callback and API threads

**Files:** `src/audio/AudioEngine.cpp` (lines 222–329, 443–563, 570–621, 623–676)
**Severity:** CRITICAL

The miniaudio `DataCallback` executes on a dedicated real-time audio thread and accesses nearly every field in `AudioInternal` without synchronization. Meanwhile, `PushIncomingAudioWithSequence`, `ClearRemoteTracks`, `RemoveUserTrack`, `Update`, `ApplyConfig`, `OnVoiceStateUpdate`, and `OnNetworkConditions` run on the main/network thread and mutate the same fields. Only `gain` (atomic) and `selfDeafened` (atomic) are properly synchronized.

**Unprotected shared state includes:**

| Field | Audio thread access | Main thread access |
|-------|--------------------|--------------------|
| `tracks[i].active` | Read (line 263) | Write (lines 504, 582, 609) |
| `tracks[i].isBuffering` | Read/Write (lines 267, 277, 300) | Write (lines 507, 585) |
| `tracks[i].coldStart` | Read/Write (lines 269, 301) | Write (lines 508, 586) |
| `tracks[i].smoothedBufferLevelMs` | Read/Write (lines 278, 281, 283, 288, 293) | Write (lines 589) |
| `tracks[i].userId` | Read (line 494) | Write (lines 503, 583, 610) |
| `adaptiveBufferLevel` | Read (line 265), Write (line 303) | Write (lines 625, 673, 694, 716) |
| `bufferUnderruns` | Write (line 299) | Read via `GetTelemetry()` |
| `captureRMS` | Write (line 232) | Read via `GetMicActivity()` (line 651) |
| `currentGain` / `targetGain` | Write (lines 325–326) | — |
| `callbackInvocations` | Write (line 227) | — |
| `maxJitterSpikeMs` | — | Write (lines 410, 482, 706) |

**Impact:** Data races cause undefined behavior in C++. Practical consequences include corrupted audio playback, phantom "buffering" states that never recover, torn reads of `adaptiveBufferLevel` causing jitter buffer miscalculation, and potentially crashing the audio thread.

**Suggested fix:** Protect all shared state with a mutex (preferred: a single `std::mutex` acquired in `DataCallback` and all API methods), or convert all shared primitive fields to `std::atomic` with appropriate memory ordering. For compound operations (e.g., setting `active` + `userId` + `decoder` together), a mutex is required.

---

### C-02: Detached thread captures `this` — use-after-free on Application destruction

**File:** `src/app/Application.cpp` (lines 228–320)
**Severity:** CRITICAL

`StartLinkProbe()` spawns a `std::thread` that captures `this` (the `Application` object), then immediately `.detach()`s it. The thread runs an infinite loop (`while (true)`) that sleeps for 8+ seconds between iterations and accesses `m_VoiceTransport`, `m_AudioEngine`, `m_NetClient`, `m_ProbeRunning`, and `m_ProbeEchoMutex`.

If the `Application` is destroyed (e.g., user closes the window), the detached thread continues running and accesses freed members — a textbook use-after-free.

The `m_ProbeRunning` atomic flag is set to `false` at line 312, but the thread sleeps for `kRepeatIntervalSec` (8 seconds) after that. During the sleep, if the Application destructor runs, the thread wakes up to find destroyed objects.

**Impact:** Crash (access violation), heap corruption, or silent memory corruption.

**Suggested fix:** Store the thread as a member (`std::thread m_ProbeThread`), set a `std::atomic<bool> m_ShuttingDown` flag in the destructor, and join the thread. Replace `std::this_thread::sleep_for` with a condition-variable wait that can be interrupted.

---

### C-03: Hardcoded production server IP address

**File:** `src/app/Application.h` (line 85)
**Severity:** CRITICAL

```cpp
std::string m_ServerIP = "94.26.90.11";
```

A real IP address is hardcoded in source code committed to the repository. This:
1. Exposes the server's IP to anyone with access to the binary or source (enabling targeted DDoS, port scanning, etc.)
2. Makes the server address impossible to change without recompilation
3. Leaks infrastructure topology

**Suggested fix:** Load the server address from a configuration file, environment variable, or command-line argument. Remove the hardcoded IP from source control entirely.

---

### C-04: Plaintext password retained in memory indefinitely

**Files:** `src/app/Application.h` (lines 151–153), `src/app/Application.cpp` (lines 563–564, 781, 794)
**Severity:** CRITICAL

Passwords are stored in plain `char` arrays (`m_PasswordBuf[128]`, `m_PasswordRepeatBuf[128]`) and the `UserSession::password` string. These are **never zeroed** after use — the password remains in process memory from login until application exit.

Additionally, `ConfigManager::SaveSession` (ConfigManager.h line 52) creates a temporary `std::string plain` containing the JSON-encoded password, which is never securely wiped. When `plain` goes out of scope, `std::string`'s destructor frees the memory without zeroing it, leaving the password in the heap.

**Impact:** Any memory-reading attack (memory dump, crash dump analysis, another process with `ReadProcessMemory`, cold boot attack) can extract the user's password.

**Suggested fix:**
- Use `SecureZeroMemory(m_PasswordBuf, sizeof(m_PasswordBuf))` after the password is consumed for authentication.
- Implement a secure string class that zeros memory on destruction.
- Do not store the raw password in `UserSession`; store a session token instead.

---

### C-05: XOR "encryption" fallback provides zero security for stored credentials

**File:** `src/core/ConfigManager.h` (lines 168–175)
**Severity:** CRITICAL

The `Obfuscate` method used as a fallback when DPAPI fails is a single-byte XOR with a hardcoded key `0x5A`:

```cpp
char key = 0x5A;
for (size_t i = 0; i < output.size(); ++i) {
    output[i] ^= key;
}
```

This is trivially reversible by anyone who reads the source or binary (the key is a constant). XOR with a single repeating byte is not encryption — it's obfuscation that provides no meaningful confidentiality.

The same function is also used for decryption in `LoadSession` (line 35), confirming it's a symmetric XOR.

**Impact:** Stored credentials (email + password) are effectively stored in plaintext in `%LOCALAPPDATA%\TalkMe\session.dat` whenever DPAPI is unavailable.

**Suggested fix:** Remove the XOR fallback entirely. If DPAPI is unavailable, do not persist the session — require the user to re-enter credentials. Alternatively, use a proper encryption library (e.g., libsodium's `crypto_secretbox`).

---

## HIGH Findings

### H-01: Division by zero in `CalculateRMS`

**File:** `src/audio/AudioEngine.cpp` (line 207)
**Severity:** HIGH

```cpp
inline float CalculateRMS(const float* samples, ma_uint32 count) {
    float sum = 0.0f;
    for (ma_uint32 i = 0; i < count; ++i) sum += samples[i] * samples[i];
    return std::sqrt(sum / count);  // division by zero if count == 0
}
```

`CalculateRMS` is called from `DataCallback` (line 231) with `frameCount` and from the mix path (line 324). While miniaudio typically passes non-zero `frameCount`, the function has no guard.

**Suggested fix:** `if (count == 0) return 0.0f;`

---

### H-02: Null pointer dereference in `OpusEncoderWrapper::AdjustBitrate` and `SetPacketLossPercentage`

**File:** `src/audio/AudioEngine.cpp` (lines 65–84)
**Severity:** HIGH

`AdjustBitrate()` (line 73) and `SetPacketLossPercentage()` (line 83) call `opus_encoder_ctl(encoder, ...)` without checking if `encoder` is null. If `opus_encoder_create` fails in the constructor (line 37), `encoder` remains `nullptr`.

Compare with `Encode()` (line 58) and `SetTargetBitrate()` (line 87), which do check for null.

**Suggested fix:** Add `if (!encoder) return;` at the start of both methods.

---

### H-03: Sequence number wraparound causes packet rejection

**File:** `src/audio/AudioEngine.cpp` (lines 455–460)
**Severity:** HIGH

```cpp
if (seqNum == last) { /* duplicate */ return; }
if (seqNum < last) return;  // <-- rejects valid post-wraparound packets
```

When the `uint32_t` sequence number wraps from `0xFFFFFFFF` to `0`, all subsequent packets will have `seqNum < last` and be silently dropped, causing a complete audio outage until the user reconnects.

**Suggested fix:** Use signed difference comparison: `int32_t diff = (int32_t)(seqNum - last); if (diff == 0) return; if (diff < 0) return;`. This handles wraparound correctly for gaps up to 2^31.

---

### H-04: Ring buffer leak on partial initialization failure

**File:** `src/audio/AudioEngine.cpp` (lines 341–351)
**Severity:** HIGH

In `InitializeWithSequence`, if `ma_pcm_rb_init` fails for track ring buffer `i` (line 347), the function returns `false` without uninitializing the capture ring buffer (line 342) or the `i-1` track ring buffers that succeeded.

**Suggested fix:** Add cleanup on failure: uninitialize all successfully-initialized ring buffers and the capture ring buffer before returning false.

---

### H-05: Non-ASCII username corruption in overlay rendering

**File:** `src/overlay/GameOverlay.cpp` (line 195)
**Severity:** HIGH

```cpp
std::wstring wname(m.name.begin(), m.name.end());
```

This copies each `char` byte to a `wchar_t`, which is not a proper UTF-8 to UTF-16 conversion. Any non-ASCII characters (accented Latin, Cyrillic, CJK, emoji) will be garbled in the overlay.

**Suggested fix:** Use `MultiByteToWideChar(CP_UTF8, ...)` for proper conversion:
```cpp
int len = MultiByteToWideChar(CP_UTF8, 0, m.name.c_str(), -1, nullptr, 0);
std::wstring wname(len - 1, L'\0');
MultiByteToWideChar(CP_UTF8, 0, m.name.c_str(), -1, &wname[0], len);
```

---

### H-06: Non-ASCII exe path corruption in `GetExeDirectory`

**File:** `src/app/Application.cpp` (lines 103–110)
**Severity:** HIGH

```cpp
std::wstring w(path);
std::string s(w.begin(), w.end());  // truncates non-ASCII
```

Same issue as H-05. If the executable is installed in a path with non-ASCII characters (e.g., a user's home directory with an accented name), the path is corrupted, causing log files and config files to be written to wrong locations.

**Suggested fix:** Use `WideCharToMultiByte(CP_UTF8, ...)` or stay in the wide-string domain throughout.

---

### H-07: DPAPI without additional entropy — any same-user process can decrypt credentials

**File:** `src/core/ConfigManager.h` (lines 177–186)
**Severity:** HIGH

```cpp
CryptProtectData(&in, L"TalkMeSession", nullptr, nullptr, nullptr, 0, &outBlob);
```

The `pOptionalEntropy` parameter is `nullptr`. Any process running under the same Windows user account can call `CryptUnprotectData` on the `session.dat` file and retrieve the plaintext credentials.

**Impact:** Malware or any other application running as the current user can steal TalkMe credentials without elevated privileges.

**Suggested fix:** Pass a non-null `pOptionalEntropy` parameter derived from a machine-specific or application-specific secret (e.g., a hash of the install path or a randomly generated key stored in a separate protected location).

---

### H-08: Race condition on `m_UseUdpVoice` flag

**File:** `src/app/Application.cpp` (lines 519, 878, 432–437, 664–667)
**Severity:** HIGH

`m_UseUdpVoice` is a plain `bool` read and written from:
- Main thread: `Initialize()` (line 519), `ProcessNetworkMessages()` (line 878)
- Audio capture callback (indirectly via the lambda at line 424 that reads `m_UseUdpVoice` at line 434)
- Probe thread (line 316)

No synchronization protects these accesses.

**Suggested fix:** Change to `std::atomic<bool>`.

---

### H-09: `VoiceSendPacer` thread not joined when `m_UseUdpVoice` changes to false

**File:** `src/app/Application.cpp` (lines 1077–1079)
**Severity:** HIGH

```cpp
if (m_UseUdpVoice) {
    m_SendPacer.Stop();
    m_VoiceTransport.Stop();
}
```

The send pacer is only stopped during cleanup if `m_UseUdpVoice` is true. If a `Voice_Config` packet from the server sets `preferUdp = false` (line 876–878), `m_UseUdpVoice` becomes false, but the pacer thread keeps running. On destruction, the thread is never joined, and accesses `m_VoiceTransport` after it's destroyed.

**Suggested fix:** Always call `m_SendPacer.Stop()` in `Cleanup()`, regardless of `m_UseUdpVoice`.

---

### H-10: Config files created without restrictive ACLs

**File:** `src/core/ConfigManager.h` (lines 60–65, 75, 92, 129)
**Severity:** HIGH

All configuration files (`session.dat`, `theme.cfg`, `keybinds.cfg`, `overlay.cfg`, `user_volumes.json`) are created with default inherited permissions. On multi-user Windows machines, other local users may be able to read the encrypted session file and attempt offline attacks.

**Suggested fix:** Use `CreateFileA` with a `SECURITY_ATTRIBUTES` that restricts access to the current user only, or call `SetFileSecurity` / `SetNamedSecurityInfoA` after creation.

---

### H-11: Global `g_AppInstance` pointer without synchronization

**File:** `src/app/Application.cpp` (line 60)
**Severity:** HIGH

```cpp
static TalkMe::Application* g_AppInstance = nullptr;
```

Set in the constructor (line 380) and cleared in the destructor (line 381). Read from `WndProc` → `HandleResize` (line 113). While all accesses likely happen on the same thread in practice, this is formally a data race if any window message is processed during construction or destruction.

**Suggested fix:** Use `std::atomic<TalkMe::Application*>`.

---

### H-12: Stack buffer sizes in real-time audio callback

**File:** `src/audio/AudioEngine.cpp` (lines 258, 308)
**Severity:** HIGH

```cpp
float mixBuffer[4096];   // 16 KB
float trackBuffer[4096]; // 16 KB (per track iteration, same stack frame)
```

The audio callback allocates 32+ KB on the stack. Audio threads on some platforms (particularly WASAPI exclusive mode) may have reduced stack sizes. The guard `if (frameCount > 4096) return;` prevents buffer overflow but doesn't prevent stack overflow.

**Suggested fix:** Allocate these buffers as members of `AudioInternal` instead of on the stack.

---

## MEDIUM Findings

### M-01: `ClearRemoteTracks` and `RemoveUserTrack` modify ring buffer state while audio callback reads it

**File:** `src/audio/AudioEngine.cpp` (lines 570–621)
**Severity:** MEDIUM

These methods drain ring buffers and reset track state while the audio callback (`DataCallback`) may be concurrently reading from the same ring buffers and track fields. The `ma_pcm_rb` ring buffer in miniaudio is designed for single-producer/single-consumer — calling `acquire_read`/`commit_read` from two threads simultaneously is undefined.

**Suggested fix:** Stop the audio device, clear tracks, then restart; or use a mutex shared with the callback.

---

### M-02: Opus PLC loop can fill ring buffer to capacity under sustained packet loss

**File:** `src/audio/AudioEngine.cpp` (lines 529–541)
**Severity:** MEDIUM

For each missing packet (up to 9 per burst), a full `OPUS_FRAME_SIZE` (480 samples) PLC frame is written to the ring buffer. Under sustained packet loss, PLC data can accumulate. While the overflow guard at line 551 caps the buffer at 96000 frames, the sudden seek-forward on overflow creates an audible glitch.

**Suggested fix:** Limit PLC generation to a maximum of 3 frames and let the jitter buffer handle longer gaps via underrun/rebuffering.

---

### M-03: `m_SelfMuted` and `m_SelfDeafened` in `AudioEngine` are not atomic

**File:** `src/audio/AudioEngine.h` (lines 90–92)
**Severity:** MEDIUM

`m_SelfMuted` and `m_SelfDeafened` are plain `bool`s set from the main thread (`SetSelfMuted`, `SetSelfDeafened`) and read from the audio callback (indirectly via `Update()` which runs on the main thread). `m_SelfDeafened` is correctly mirrored to an atomic in `AudioInternal::selfDeafened`, but `m_SelfMuted` is not — and it's read in `Update()` (line 433) on the main thread, which is probably fine. However, `SetCaptureEnabled` (line 29) and `IsCaptureEnabled` (line 30) access `m_CaptureEnabled` which could be set from the main thread while checked from the audio capture lambda.

**Suggested fix:** Make `m_CaptureEnabled`, `m_SelfMuted`, `m_SelfDeafened` all `std::atomic<bool>`.

---

### M-04: Silent exception swallowing in `ProcessNetworkMessages`

**File:** `src/app/Application.cpp` (line 919)
**Severity:** MEDIUM

```cpp
catch (...) {}
```

All exceptions from JSON parsing and message processing are silently swallowed. This hides potential bugs: malformed server responses, type mismatches, out-of-range values, and even `std::bad_alloc`.

**Suggested fix:** At minimum, log the exception: `catch (const std::exception& e) { LOG_ERROR(std::string("ProcessNetworkMessages: ") + e.what()); } catch (...) { LOG_ERROR("ProcessNetworkMessages: unknown exception"); }`

---

### M-05: `GdiplusStartup`/`Shutdown` reference counting is not thread-safe

**File:** `src/overlay/GameOverlay.cpp` (lines 9–24)
**Severity:** MEDIUM

`s_GdipRefCount` and `s_GdipToken` are static globals modified without synchronization in `InitGdiPlus()` and `ShutdownGdiPlus()`. If multiple GameOverlay instances are created or destroyed concurrently, the reference count can become corrupted, potentially shutting down GDI+ prematurely.

**Suggested fix:** Protect with a `static std::mutex` or use `std::call_once`.

---

### M-06: No bounds validation on overlay corner value

**File:** `src/overlay/GameOverlay.cpp` (lines 87–89, 121–126)
**Severity:** MEDIUM

`SetCorner(int corner)` stores any integer value without validating it is in range [0, 3]. In `Reposition()`, values outside 0–3 fall through the `switch` without setting `x` or `y`, leaving them at their default of 0, which places the overlay at the top-left regardless.

**Suggested fix:** `m_Corner = std::clamp(corner, 0, 3);`

---

### M-07: `static` device list cache in `RenderMainApp` is never refreshed

**File:** `src/app/Application.cpp` (lines 962–969)
**Severity:** MEDIUM

```cpp
static std::vector<TalkMe::AudioDeviceInfo> s_InputDevs;
static std::vector<TalkMe::AudioDeviceInfo> s_OutputDevs;
static bool s_DevsLoaded = false;
```

Audio device lists are loaded once and never refreshed. If the user plugs in a headset or USB microphone after opening settings, the new device won't appear.

**Suggested fix:** Refresh the device list each time the settings panel is opened, or add a refresh button. Remove the `static` qualifier.

---

### M-08: Plaintext password stored in `UserSession::password`

**File:** `src/core/ConfigManager.h` (line 18), `src/app/Application.cpp` (lines 564–565)
**Severity:** MEDIUM

The `UserSession` struct carries the raw password string in memory. After a successful login, the password is no longer needed (a session token should be used instead), but it persists for the lifetime of the application.

**Suggested fix:** Clear `m_CurrentUser.password` immediately after sending the login/register request. Implement token-based session management.

---

### M-09: `freopen_s` result ignored in debug console setup

**File:** `src/app/main.cpp` (lines 15–16)
**Severity:** MEDIUM

```cpp
FILE* fp;
freopen_s(&fp, "CONOUT$", "w", stdout);
freopen_s(&fp, "CONOUT$", "w", stderr);
```

The `fp` variable is assigned but the return value of `freopen_s` is not checked. If the console handle cannot be opened, subsequent `std::cout`/`std::cerr` writes will fail silently. Also, the `fp` from the first `freopen_s` is immediately overwritten by the second, leaking the reference.

**Suggested fix:** Check return values. Use separate variables if both are needed.

---

### M-10: `VoiceSendPacer::Start` does not handle double-start

**File:** `src/app/Application.cpp` (lines 65–88)
**Severity:** MEDIUM

If `Start` is called twice, the existing thread is overwritten by assignment (`m_Thread = std::thread(...)`) without joining the first. This is undefined behavior per the C++ standard (`std::thread::operator=` calls `std::terminate()` if the thread is joinable).

**Suggested fix:** Add `if (m_Running.exchange(true)) return;` at the top of `Start()`, or call `Stop()` first.

---

### M-11: Integer overflow potential in jitter buffer threshold calculation

**File:** `src/audio/AudioEngine.cpp` (line 271)
**Severity:** MEDIUM

```cpp
ma_uint32 requiredFrames = (ma_uint32)((thresholdMs * SAMPLE_RATE) / 1000);
```

`thresholdMs` is `int` and `SAMPLE_RATE` is `int` (48000). If `maxBufferMs` is configured to a large value (e.g., > 44,739 via `ApplyConfig`), the multiplication overflows `int` (2,147,483,647 / 48000 ≈ 44,739). The result would wrap to a negative value, then cast to a very large `ma_uint32`.

**Suggested fix:** Cast to `int64_t` before multiplication: `(ma_uint32)((int64_t)thresholdMs * SAMPLE_RATE / 1000)`.

---

## LOW Findings

### L-01: Duplicate `#define LOG_ERROR` macro

**File:** `src/core/Logger.h` (lines 179, 183)
**Severity:** LOW

`LOG_ERROR` is defined twice with identical definitions. While this compiles (the preprocessor silently replaces the first), it indicates a copy-paste error and may confuse maintainers.

**Suggested fix:** Remove the duplicate on line 183.

---

### L-02: Endianness detection via type punning (strict aliasing violation)

**File:** `src/shared/Protocol.h` (lines 14–15)
**Severity:** LOW

```cpp
uint32_t test = 1;
return (*reinterpret_cast<uint8_t*>(&test) == 1) ? Swap32(v) : v;
```

`reinterpret_cast<uint8_t*>(&test)` is technically well-defined in C++ (aliasing to `char`/`unsigned char`/`std::byte` is allowed), but the pattern is fragile. A compiler may evaluate this at compile time given that the target architecture is known.

**Suggested fix:** Use `std::endian` (C++20) or `#if __BYTE_ORDER__` preprocessor checks for compile-time endianness detection, or use `std::bit_cast`.

---

### L-03: Potential deadlock in `Logger::Shutdown()`

**File:** `src/core/Logger.h` (lines 102–107)
**Severity:** LOW

```cpp
void Shutdown() {
    std::lock_guard<std::mutex> lock(m_Mutex);       // acquires m_Mutex
    // ...
    { std::lock_guard<std::mutex> l2(m_VoiceTraceMutex); ... }  // acquires m_VoiceTraceMutex
}
```

This establishes a lock ordering of `m_Mutex` → `m_VoiceTraceMutex`. If any code path acquires them in the reverse order, deadlock results. Currently no code does this, but it's a latent risk as the codebase grows.

**Suggested fix:** Avoid nesting lock acquisitions, or document and enforce a strict lock ordering.

---

### L-04: `const_cast` in `ConfigManager::UnprotectData`

**File:** `src/core/ConfigManager.h` (line 189)
**Severity:** LOW

```cpp
inBlob.pbData = (BYTE*)const_cast<char*>(in.data());
```

`const_cast` is used to pass a `const std::string&` to `CryptUnprotectData`, which takes a non-const `DATA_BLOB*`. While DPAPI does not modify the input, the cast is technically undefined behavior if the caller's original data was const.

**Suggested fix:** Copy the input to a mutable buffer, or use a `std::vector<BYTE>` intermediary.

---

### L-05: `AudioEngine::Shutdown()` does not reset `deviceStarted` flag

**File:** `src/audio/AudioEngine.cpp` (lines 654–664)
**Severity:** LOW

After `ma_device_uninit`, `deviceStarted` is never set to `false`. If `Shutdown()` is called twice (e.g., from `~AudioEngine` after an explicit shutdown), the second call would attempt to uninit an already-uninitialized device.

**Suggested fix:** Set `m_Internal->deviceStarted = false;` after `ma_device_uninit`.

---

## INFO Findings

### I-01: Hardcoded ports in `Protocol.h`

**File:** `src/shared/Protocol.h` (lines 22–23)
**Severity:** INFO

```cpp
constexpr uint16_t SERVER_PORT = 5555;
constexpr uint16_t VOICE_PORT = 5556;
```

Server and voice ports are hardcoded. This is common for simple protocols but prevents deployment flexibility.

---

### I-02: `Globals.h` constants not enforced

**File:** `src/core/Globals.h` (lines 16–18)
**Severity:** INFO

`MAX_USERNAME_LEN`, `MAX_CHANNEL_NAME_LEN`, and `MAX_MESSAGE_LEN` are defined but do not appear to be used for input validation in the audited files. Buffer sizes in `Application.h` (128 for username, 1024 for chat input) don't correspond to these constants.

---

### I-03: Voice trace logging enabled by marker file in release builds

**File:** `src/app/Application.cpp` (lines 402–406)
**Severity:** INFO

```cpp
std::string marker = exeDir + "\\talkme_voice_trace.enable";
std::ifstream f(marker);
voiceTraceEnable = f.good();
```

In release builds, anyone who creates a file named `talkme_voice_trace.enable` next to the executable can enable verbose voice pipeline logging, which writes detailed timing, sequence numbers, and sender names to disk. This could be used for surveillance if an attacker has write access to the install directory.

---

## Recommendations Summary

### Immediate Actions (CRITICAL)
1. **Add thread synchronization** to `AudioInternal` shared state (C-01)
2. **Replace detached probe thread** with a joinable, properly-lifetime-managed thread (C-02)
3. **Remove hardcoded server IP** from source; load from config (C-03)
4. **Securely wipe password buffers** after use; implement token-based sessions (C-04)
5. **Remove XOR fallback encryption**; fail gracefully instead (C-05)

### Short-Term (HIGH)
6. Add null checks to `AdjustBitrate`/`SetPacketLossPercentage` (H-02)
7. Fix sequence number wraparound handling (H-03)
8. Cleanup ring buffers on partial init failure (H-04)
9. Use proper UTF-8↔UTF-16 conversion (H-05, H-06)
10. Add entropy to DPAPI calls (H-07)
11. Make `m_UseUdpVoice` atomic (H-08)
12. Always stop `VoiceSendPacer` in `Cleanup()` (H-09)
13. Set restrictive ACLs on config files (H-10)

### Medium-Term (MEDIUM)
14. Add locking to ring buffer access patterns (M-01)
15. Limit PLC frame generation (M-02)
16. Make boolean flags atomic (M-03)
17. Log swallowed exceptions (M-04)
18. Thread-safe GDI+ ref counting (M-05)
19. Refresh audio device lists (M-07)
