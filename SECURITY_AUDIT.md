# TalkMe Security Audit Report

**Date:** 2026-02-25  
**Scope:** Network layer files — `NetworkClient`, `VoiceTransport`, `PacketHandler`, `Protocol.h`, plus related credential-handling surfaces (`ConfigManager.h`, `LoginView.cpp`, `Application.h`)  
**Auditor:** Automated static analysis

---

## Summary

| Severity | Count |
|----------|-------|
| CRITICAL | 3     |
| HIGH     | 6     |
| MEDIUM   | 8     |
| LOW      | 5     |
| INFO     | 2     |
| **Total** | **24** |

---

## Findings

### CRITICAL-01: Plaintext TCP — No TLS on Control Connection

| Field | Value |
|-------|-------|
| **Severity** | CRITICAL |
| **File** | `src/network/NetworkClient.cpp` |
| **Lines** | 68–72 |
| **CWE** | CWE-319 (Cleartext Transmission of Sensitive Information) |

**Description:**  
The control connection uses a raw `asio::ip::tcp::socket` with no TLS wrapper. All data — including login credentials (`Login_Request` with email/password JSON), chat messages, and server metadata — is transmitted in plaintext. Any on-path observer or network-adjacent attacker can passively sniff all traffic.

**Evidence:**
```cpp
m_Impl->m_Socket = asio::ip::tcp::socket(m_Impl->m_Context);
asio::ip::tcp::resolver resolver(m_Impl->m_Context);
auto endpoints = resolver.resolve(host, std::to_string(port));
asio::connect(m_Impl->m_Socket, endpoints);
```

**Suggested Fix:**  
Replace `asio::ip::tcp::socket` with `asio::ssl::stream<asio::ip::tcp::socket>` using a properly configured `asio::ssl::context`. Enforce TLS 1.2+ and pin or verify the server certificate.

---

### CRITICAL-02: Plaintext UDP — No Encryption on Voice Transport

| Field | Value |
|-------|-------|
| **Severity** | CRITICAL |
| **File** | `src/network/VoiceTransport.cpp` |
| **Lines** | 85–100 (SendVoicePacket), 168–202 (DoReceive) |
| **CWE** | CWE-319 (Cleartext Transmission of Sensitive Information) |

**Description:**  
Voice audio data (raw PCM and Opus) is sent over UDP with no encryption, authentication, or integrity protection. Any on-path observer can capture and reconstruct voice conversations in real-time. There is no DTLS, SRTP, or equivalent mechanism.

**Suggested Fix:**  
Implement DTLS 1.2/1.3 or SRTP with key exchange over the TLS control channel. At minimum, use an authenticated encryption scheme (e.g., AES-GCM) with per-session keys derived during the TLS handshake.

---

### CRITICAL-03: Credentials Transmitted in Cleartext

| Field | Value |
|-------|-------|
| **Severity** | CRITICAL |
| **Files** | `src/network/PacketHandler.h` (lines 11–16, 18–24), `src/ui/views/LoginView.cpp` (lines 86–89, 96–97) |
| **CWE** | CWE-523 (Unprotected Transport of Credentials) |

**Description:**  
Login and registration payloads carry raw passwords in JSON (`{"e":"user@example.com","p":"hunter2"}`) over the unencrypted TCP connection (CRITICAL-01). Combined, this means passwords are trivially interceptable on any intermediate network hop.

**Evidence (PacketHandler.h:11–16):**
```cpp
static std::string CreateLoginPayload(const std::string& email, const std::string& password) {
    nlohmann::json j;
    j["e"] = email;
    j["p"] = password;
    return j.dump();
}
```

**Suggested Fix:**  
1. Enable TLS (CRITICAL-01 fix) to encrypt the transport.
2. Never transmit raw passwords — use a challenge-response protocol (SRP) or send only a salted hash; or use OAuth/OIDC tokens.
3. Implement server-side password hashing (bcrypt/argon2) if not already done.

---

### HIGH-01: No Source Validation on Incoming UDP Packets

| Field | Value |
|-------|-------|
| **Severity** | HIGH |
| **File** | `src/network/VoiceTransport.cpp` |
| **Lines** | 168–177 |
| **CWE** | CWE-346 (Origin Validation Error) |

**Description:**  
`DoReceive()` accepts UDP datagrams from **any** sender IP/port. The `sender` endpoint is captured but never compared against `m_RemoteEndpoint`. An attacker on the network can inject arbitrary voice data or pong packets from any IP address, causing audio injection or RTT manipulation.

**Evidence:**
```cpp
asio::ip::udp::endpoint sender;
size_t n = m_Socket.receive_from(asio::buffer(m_RecvBuffer), sender);
// 'sender' is never checked against m_RemoteEndpoint
if (n > 1) {
    uint8_t packetKind = m_RecvBuffer[0];
    if (packetKind == kVoicePacket) {
        // ... processes data from ANY source
```

**Suggested Fix:**  
Validate that `sender == m_RemoteEndpoint` before processing any received packet. Drop packets from unknown endpoints.

---

### HIGH-02: Username Length Truncation in Voice Payload

| Field | Value |
|-------|-------|
| **Severity** | HIGH |
| **File** | `src/network/PacketHandler.h` |
| **Lines** | 107–108 (CreateVoicePayloadOpus), 184 (CreateVoicePayload) |
| **CWE** | CWE-681 (Incorrect Conversion between Numeric Types) |

**Description:**  
`uint8_t ulen = (uint8_t)username.size()` silently truncates usernames longer than 255 bytes. The 1-byte length field then points to a wrong amount of data, causing the receiver to parse the payload incorrectly — reading part of the username as opus audio data, or vice versa. This leads to corrupted audio playback and could cause crashes in downstream decoders.

**Evidence:**
```cpp
uint8_t ulen = (uint8_t)username.size();  // wraps at 256
payload[offset] = ulen;
std::memcpy(payload.data() + offset, username.c_str(), username.size()); // writes full username
```

If `username.size() == 300`, `ulen == 44`, but 300 bytes are copied. The receiver reads only 44 bytes as the username, treating the remaining 256 bytes as opus data.

**Suggested Fix:**  
Validate `username.size() <= 255` at the call site and reject/truncate before serialization. Add an assertion or early return with error status:
```cpp
if (username.size() > 255) return {}; // or throw
```

---

### HIGH-03: Hardcoded Production Server IP

| Field | Value |
|-------|-------|
| **Severity** | HIGH |
| **File** | `src/app/Application.h` |
| **Line** | 85 |
| **CWE** | CWE-798 (Use of Hard-coded Credentials) |

**Description:**  
The production server IP `94.26.90.11` is hardcoded in the source code. This exposes the server infrastructure to targeted attacks, makes IP rotation impossible without recompilation, and leaks server location to anyone with the binary.

```cpp
std::string m_ServerIP = "94.26.90.11";
```

**Suggested Fix:**  
Load the server address from a configuration file, environment variable, or DNS-based service discovery (e.g., `voice.talkme.example.com`). Use DNS so the server IP can be rotated without client updates.

---

### HIGH-04: Plaintext Password Stored on Disk

| Field | Value |
|-------|-------|
| **Severity** | HIGH |
| **File** | `src/core/ConfigManager.h` |
| **Lines** | 48–66 |
| **CWE** | CWE-256 (Plaintext Storage of a Password) |

**Description:**  
`SaveSession()` persists the raw password to `%LOCALAPPDATA%\TalkMe\session.dat`. While the primary protection mechanism is Windows DPAPI (`CryptProtectData`), there is a fallback to single-byte XOR obfuscation (see HIGH-05), which is trivially reversible. Even with DPAPI, storing the raw password rather than a session token is a design flaw — if the file is exfiltrated, the password is recovered.

**Suggested Fix:**  
Never store the raw password. Implement server-issued session tokens (JWT or opaque) with expiration and refresh. Store only the session token locally. Use DPAPI for the token storage and remove the XOR fallback entirely.

---

### HIGH-05: Trivially Reversible XOR "Encryption" Fallback

| Field | Value |
|-------|-------|
| **Severity** | HIGH |
| **File** | `src/core/ConfigManager.h` |
| **Lines** | 168–175 |
| **CWE** | CWE-327 (Use of a Broken or Risky Cryptographic Algorithm) |

**Description:**  
The `Obfuscate()` function uses single-byte XOR with a hardcoded key `0x5A`. This is symmetric — applying it twice yields the original plaintext. It provides zero cryptographic protection and creates a false sense of security. This is the fallback when DPAPI fails.

```cpp
std::string Obfuscate(const std::string& input) {
    std::string output = input;
    char key = 0x5A;
    for (size_t i = 0; i < output.size(); ++i) {
        output[i] ^= key;
    }
    return output;
}
```

**Suggested Fix:**  
Remove the XOR fallback entirely. If DPAPI is unavailable, refuse to save credentials and require the user to re-enter them on each launch. Alternatively, use a proper authenticated encryption scheme (AES-256-GCM) with a key derived from a user-provided passphrase.

---

### HIGH-06: No Packet Integrity / Authentication on Wire Protocol

| Field | Value |
|-------|-------|
| **Severity** | HIGH |
| **File** | `src/shared/Protocol.h` |
| **Lines** | 69–76 |
| **CWE** | CWE-345 (Insufficient Verification of Data Authenticity) |

**Description:**  
`PacketHeader` contains only `type` and `size` — there is no HMAC, session ID, or sequence number. Without TLS, an attacker can:
- Forge arbitrary packets
- Replay captured packets
- Modify packets in transit
- Inject packets into an existing session

Even with TLS, the lack of an application-layer session binding means a compromised server can relay packets between sessions.

**Suggested Fix:**  
1. First priority: enable TLS (CRITICAL-01), which provides integrity and authentication at the transport layer.
2. Add a session token or HMAC field to `PacketHeader` for defense-in-depth.
3. Add monotonic sequence numbers to detect replays at the application layer.

---

### MEDIUM-01: Use-After-Free Risk — Detached Thread Captures `this`

| Field | Value |
|-------|-------|
| **Severity** | MEDIUM |
| **File** | `src/network/NetworkClient.cpp` |
| **Lines** | 60–102 |
| **CWE** | CWE-416 (Use After Free) |

**Description:**  
`ConnectAsync()` spawns a detached thread (`}).detach()`) that captures `this`. If the `NetworkClient` object is destroyed before the detached thread completes (e.g., user closes the app during a connection attempt), the thread accesses freed memory through the dangling `this` pointer, leading to undefined behavior and potential code execution.

**Evidence:**
```cpp
std::thread([this, host, port, onResult]() {
    // ... accesses m_Impl->m_Context, m_Impl->m_Socket, etc.
    // All through 'this' which may be destroyed
}).detach();
```

**Suggested Fix:**  
Use `std::shared_ptr` + weak reference pattern, or store the thread and join in the destructor:
```cpp
// Option 1: Store the connect thread and join in destructor
m_ConnectThread = std::thread([this, ...]() { ... });

// Option 2: Use shared_ptr/weak_ptr
auto impl = m_Impl; // shared_ptr
std::thread([impl, host, port, onResult]() { ... }).detach();
```

---

### MEDIUM-02: Data Race on `m_VoiceCallback`

| Field | Value |
|-------|-------|
| **Severity** | MEDIUM |
| **File** | `src/network/NetworkClient.cpp` |
| **Lines** | 38–43 (SetVoiceCallback), 209–210 (ReadBody) |
| **CWE** | CWE-362 (Concurrent Execution Using Shared Resource with Improper Synchronization) |

**Description:**  
`SetVoiceCallback()` acquires `m_QueueMutex` when writing `m_VoiceCallback`, but `ReadBody()` reads `m_VoiceCallback` on the ASIO thread **without** holding the mutex. This is a data race on `std::function` which is not thread-safe — concurrent read/write can corrupt internal state.

**Evidence:**
```cpp
// SetVoiceCallback (holds mutex):
std::lock_guard<std::mutex> lock(m_Impl->m_QueueMutex);
m_Impl->m_VoiceCallback = callback;

// ReadBody (NO mutex):
if (m_Impl->m_VoiceCallback)
    m_Impl->m_VoiceCallback(m_Impl->m_InBody);
```

**Suggested Fix:**  
Protect the callback read with the same mutex, or use `std::atomic<std::shared_ptr<...>>` pattern, or copy the callback under lock before invoking:
```cpp
std::function<void(const std::vector<uint8_t>&)> cb;
{
    std::lock_guard<std::mutex> lock(m_Impl->m_QueueMutex);
    cb = m_Impl->m_VoiceCallback;
}
if (cb) cb(m_Impl->m_InBody);
```

---

### MEDIUM-03: Data Race on `m_Callback` in VoiceTransport

| Field | Value |
|-------|-------|
| **Severity** | MEDIUM |
| **File** | `src/network/VoiceTransport.h` (line 30), `src/network/VoiceTransport.cpp` (line 177) |
| **CWE** | CWE-362 |

**Description:**  
`SetReceiveCallback()` sets `m_Callback` inline with no synchronization. `DoReceive()` reads `m_Callback` on a different thread. This is a data race with undefined behavior.

```cpp
void SetReceiveCallback(...) { m_Callback = cb; }  // UI thread, no lock
// ...
if (m_Callback) m_Callback(packet);  // worker thread, no lock
```

**Suggested Fix:**  
Protect with a mutex, or require `SetReceiveCallback` to be called before `Start()` and document it as not thread-safe.

---

### MEDIUM-04: 10 MB Max Packet — Excessive Allocation

| Field | Value |
|-------|-------|
| **Severity** | MEDIUM |
| **File** | `src/network/NetworkClient.cpp` |
| **Line** | 189 |
| **CWE** | CWE-770 (Allocation of Resources Without Limits) |

**Description:**  
The maximum allowed packet body is 10 MB (`10 * 1024 * 1024`). For a messaging application, this is excessive. A malicious server (or MITM, since there's no TLS) can force the client to allocate 10 MB repeatedly by sending packets just under the limit, exhausting memory. A reasonable limit for text chat and voice control packets would be 64 KB–256 KB.

**Suggested Fix:**  
Reduce to a type-appropriate limit. For example:
```cpp
constexpr uint32_t kMaxTextPacketSize = 64 * 1024;     // 64 KB
constexpr uint32_t kMaxVoicePacketSize = 256 * 1024;    // 256 KB
// Select limit based on packet type
```

---

### MEDIUM-05: No PacketType Validation After Deserialization

| Field | Value |
|-------|-------|
| **Severity** | MEDIUM |
| **File** | `src/network/NetworkClient.cpp` |
| **Line** | 188 |
| **CWE** | CWE-20 (Improper Input Validation) |

**Description:**  
After `m_InHeader.ToHost()`, the `type` field (a `uint8_t` cast to `PacketType` enum) is not validated against the valid enum range. The enum has values 0–26, but a malicious packet can contain any value 0–255. This feeds an invalid enum value into downstream `switch` statements, potentially causing undefined behavior.

**Suggested Fix:**  
Add a validation check immediately after `ToHost()`:
```cpp
if (static_cast<uint8_t>(m_Impl->m_InHeader.type) > static_cast<uint8_t>(PacketType::Echo_Response)) {
    CloseSocket();
    return;
}
```

---

### MEDIUM-06: Legacy Format Ambiguity in Voice Packet Parsing

| Field | Value |
|-------|-------|
| **Severity** | MEDIUM |
| **File** | `src/network/PacketHandler.h` |
| **Lines** | 161–176 |
| **CWE** | CWE-436 (Interpretation Conflict) |

**Description:**  
`ParseVoicePayloadOpus()` falls back to a legacy format when the new format yields an empty sender or invalid result. This creates a parsing ambiguity: a carefully crafted new-format packet where the "sequence number" bytes happen to encode a valid legacy username length can be reinterpreted in a completely different way. An attacker can exploit this to inject voice data attributed to an arbitrary username.

**Evidence:**
```cpp
if (!result.valid || result.sender.empty()) {
    ParsedVoicePacket legacy;
    // Reinterprets entire payload from byte 0 as legacy format
```

**Suggested Fix:**  
Add a version byte at the start of the payload to disambiguate formats, or remove legacy support after migration. Never fall through from one format parser to another on the same data.

---

### MEDIUM-07: No Replay Protection on Voice Packets

| Field | Value |
|-------|-------|
| **Severity** | MEDIUM |
| **File** | `src/network/PacketHandler.h` (lines 93–119), `src/shared/Protocol.h` (lines 78–90) |
| **CWE** | CWE-294 (Authentication Bypass by Capture-replay) |

**Description:**  
Voice packets include a `sequenceNumber` but it is not cryptographically bound to the session or authenticated. An attacker can capture and replay voice packets, or forge packets with arbitrary sequence numbers. The application-level dedup set (`m_VoiceDedupeSet` in `Application.h`) only prevents exact duplicates within a small window, not targeted replay attacks.

**Suggested Fix:**  
Implement SRTP or a similar scheme where sequence numbers are authenticated. Combine with DTLS key exchange for per-session keys.

---

### MEDIUM-08: Unbounded Incoming Message Queue

| Field | Value |
|-------|-------|
| **Severity** | MEDIUM |
| **File** | `src/network/NetworkClient.cpp` |
| **Line** | 216 |
| **CWE** | CWE-770 (Allocation of Resources Without Limits) |

**Description:**  
`m_IncomingQueue.push_back(msg)` has no size limit. A malicious server can flood the client with thousands of small messages, growing the queue until the client runs out of memory. If the UI thread doesn't call `FetchMessages()` quickly enough (e.g., during a frame hitch), the queue grows unboundedly.

**Suggested Fix:**  
Cap the queue size and drop oldest messages when full:
```cpp
if (m_IncomingQueue.size() < kMaxQueueSize) {
    m_IncomingQueue.push_back(msg);
} else {
    m_IncomingQueue.pop_front();
    m_IncomingQueue.push_back(msg);
}
```

---

### LOW-01: Integer Truncation on Large Payloads

| Field | Value |
|-------|-------|
| **Severity** | LOW |
| **File** | `src/network/NetworkClient.cpp` |
| **Lines** | 136, 157 |
| **CWE** | CWE-190 (Integer Overflow) |

**Description:**  
`uint32_t size = static_cast<uint32_t>(data.size())` truncates silently if `data.size()` exceeds `UINT32_MAX` (4 GB). On 64-bit systems, a `std::string` or `std::vector` can exceed this limit. The header would then advertise a smaller size than the data actually written, causing a buffer overread on the receiver.

**Suggested Fix:**  
Check before truncation:
```cpp
if (data.size() > std::numeric_limits<uint32_t>::max()) {
    // Log error and return
    return;
}
```

---

### LOW-02: Endianness Detection via Type Punning (Strict Aliasing)

| Field | Value |
|-------|-------|
| **Severity** | LOW |
| **File** | `src/shared/Protocol.h` |
| **Lines** | 13–15 |
| **CWE** | CWE-704 (Incorrect Type Conversion) |

**Description:**  
```cpp
uint32_t test = 1;
return (*reinterpret_cast<uint8_t*>(&test) == 1) ? Swap32(v) : v;
```
Technically a strict aliasing violation, though `char`/`uint8_t` aliasing is permitted by most compilers. May produce unexpected results under aggressive optimization with unusual compilers.

**Suggested Fix:**  
Use `std::endian` (C++20) or `memcpy`-based detection:
```cpp
#if __cplusplus >= 202002L
inline uint32_t HostToNet32(uint32_t v) {
    if constexpr (std::endian::native == std::endian::little) return Swap32(v);
    return v;
}
#endif
```

---

### LOW-03: Password Not Zeroed From Memory

| Field | Value |
|-------|-------|
| **Severity** | LOW |
| **File** | `src/core/ConfigManager.h` (line 18), `src/app/Application.h` (lines 153–154) |
| **CWE** | CWE-316 (Cleartext Storage of Sensitive Information in Memory) |

**Description:**  
`UserSession::password` is a `std::string` — when it's destroyed, the memory is freed but not zeroed. Memory dumps, crash dumps, or swap files can reveal the password. Similarly, `m_PasswordBuf[128]` in `Application.h` is a plain char array.

**Suggested Fix:**  
Use a secure string class that zeroes memory on destruction. For the char buffers, call `SecureZeroMemory(m_PasswordBuf, sizeof(m_PasswordBuf))` after use.

---

### LOW-04: Blocking Synchronous UDP Receive Loop

| Field | Value |
|-------|-------|
| **Severity** | LOW |
| **File** | `src/network/VoiceTransport.cpp` |
| **Lines** | 168–202 |
| **CWE** | CWE-400 (Uncontrolled Resource Consumption) |

**Description:**  
`DoReceive()` uses synchronous `receive_from()` in a tight `while` loop. While this is functional, it means:
1. The thread cannot be cleanly interrupted (relies on `m_Socket.close()` to unblock).
2. Under a packet flood, the thread will spin processing every packet with no rate limiting.

**Suggested Fix:**  
Use asio's `async_receive_from` for cooperative cancellation, or add rate limiting (e.g., max packets per time window).

---

### LOW-05: `UserSession` Stores Raw Password

| Field | Value |
|-------|-------|
| **Severity** | LOW |
| **File** | `src/core/ConfigManager.h` |
| **Lines** | 14–19 |

**Description:**  
The `UserSession` struct stores `std::string password` in plaintext in memory for the lifetime of the session. This is passed around by value and may be copied to multiple locations in memory.

**Suggested Fix:**  
Redesign to use server-issued tokens instead of keeping the password in memory after initial authentication.

---

### INFO-01: Hardcoded Default Ports

| Field | Value |
|-------|-------|
| **Severity** | INFO |
| **File** | `src/shared/Protocol.h` |
| **Lines** | 22–23 |

**Description:**  
```cpp
constexpr uint16_t SERVER_PORT = 5555;
constexpr uint16_t VOICE_PORT = 5556;
```
While not a vulnerability per se, hardcoded ports make it harder to deploy behind firewalls or change ports for operational reasons. Attackers can fingerprint the service by scanning for these specific ports.

---

### INFO-02: Exception Details Swallowed in Network Operations

| Field | Value |
|-------|-------|
| **Severity** | INFO |
| **File** | `src/network/NetworkClient.cpp` |
| **Lines** | 78–83, 97–100 |

**Description:**  
Several `catch` blocks discard the exception entirely (`catch (const std::exception&) {}`), making debugging network issues extremely difficult. While this isn't a direct security vulnerability, silent failures can mask attack attempts.

**Suggested Fix:**  
Log exceptions at minimum to a diagnostic log (respecting user privacy):
```cpp
catch (const std::exception& e) {
    Logger::Instance().LogError("Network error: " + std::string(e.what()));
}
```

---

## Priority Remediation Roadmap

| Priority | Action | Findings Addressed |
|----------|--------|--------------------|
| **P0** | Implement TLS on the TCP control connection | CRITICAL-01, CRITICAL-03, HIGH-06 |
| **P0** | Implement DTLS/SRTP on UDP voice transport | CRITICAL-02, MEDIUM-07 |
| **P1** | Replace raw password storage with session tokens | HIGH-04, HIGH-05, LOW-03, LOW-05 |
| **P1** | Validate UDP packet source address | HIGH-01 |
| **P1** | Fix username length truncation | HIGH-02 |
| **P1** | Move server address to DNS/configuration | HIGH-03 |
| **P2** | Fix data races on callbacks | MEDIUM-02, MEDIUM-03 |
| **P2** | Fix detached thread use-after-free | MEDIUM-01 |
| **P2** | Reduce max packet size, add queue bounds | MEDIUM-04, MEDIUM-08 |
| **P2** | Add PacketType validation | MEDIUM-05 |
| **P2** | Remove legacy packet format ambiguity | MEDIUM-06 |
| **P3** | Address remaining LOW/INFO items | LOW-01 through LOW-05, INFO-01, INFO-02 |
