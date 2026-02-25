# TalkMe UI Security & Bug Audit Report

**Scope:** 16 UI-related source files in `src/ui/`  
**Date:** 2026-02-25  
**Auditor:** Automated Code Audit

---

## Summary

| Severity | Count |
|----------|-------|
| CRITICAL | 2     |
| HIGH     | 3     |
| MEDIUM   | 6     |
| LOW      | 7     |
| INFO     | 5     |

---

## CRITICAL Findings

### C1. Race Condition — Async callback writes to UI buffer without synchronization

**Files:**
- `src/ui/views/LoginView.cpp` lines 85–93
- `src/ui/views/RegisterView.cpp` lines 105–113

**Description:**  
The `ConnectAsync` callback captures the `statusMessage` pointer (a raw `char*` pointing to `m_StatusMessage[256]`) by value and writes to it via `strcpy_s` on the network thread. Simultaneously, the UI thread reads `statusMessage` on every frame (lines 117–123 in LoginView.cpp, lines 140–147 in RegisterView.cpp) via `strlen()` and `ImGui::Text()`. This is a textbook data race — undefined behavior per the C++ standard — and can produce torn reads, garbage display, or crashes.

**Affected code (LoginView.cpp:85–93):**
```cpp
netClient.ConnectAsync(serverIP, serverPort,
    [&netClient, statusMessage, emailStr, passStr](bool success) {
        if (success) {
            strcpy_s(statusMessage, 256, "Authenticating...");  // WRITE on network thread
        } else {
            strcpy_s(statusMessage, 256, "Server offline.");    // WRITE on network thread
        }
    });
```

**Suggested fix:**  
Queue status updates to the UI thread instead of writing from the callback. Use a thread-safe message queue or `std::atomic` flag checked each frame. Alternatively, post status updates through the existing `NetworkClient` message queue (which `ProcessNetworkMessages` already drains on the UI thread).

---

### C2. Credentials transmitted in plaintext over the network

**Files:**
- `src/ui/views/LoginView.cpp` lines 88–89
- `src/ui/views/RegisterView.cpp` lines 108–109
- `src/app/Application.h` line 85 (`m_ServerIP = "94.26.90.11"`, port 5555)

**Description:**  
Login and registration payloads containing email and password are sent via `netClient.Send(PacketType::Login_Request, ...)` over a raw TCP socket to a hardcoded IP. The `NetworkClient` implementation shows no TLS/SSL layer. Passwords and credentials travel in cleartext, making them trivially interceptable via packet sniffing on any network hop.

**Suggested fix:**  
Implement TLS on the TCP connection (e.g., OpenSSL/BoringSSL wrapper around the socket), or at minimum hash passwords client-side with a server-provided nonce (challenge-response). Ideally use TLS 1.3 for the entire transport.

---

## HIGH Findings

### H1. Passwords stored in plaintext in memory and persisted to disk

**Files:**
- `src/app/Application.h` line 153 (`char m_PasswordBuf[128]`)
- `src/app/Application.cpp` lines 563–564, 781

**Description:**  
On successful login, the password is:
1. Kept in `m_PasswordBuf[128]` for the entire application lifetime (Application.h:153)
2. Saved to disk via `ConfigManager::Get().SaveSession(m_EmailBuf, m_PasswordBuf)` (Application.cpp:781)
3. Reloaded on next launch into the password buffer (Application.cpp:563–564)

The password is never cleared from memory, never encrypted, and persisted in plaintext to the config file.

**Suggested fix:**  
- After authentication completes, zero the password buffer with `SecureZeroMemory(m_PasswordBuf, sizeof(m_PasswordBuf))`.
- Do NOT persist plaintext passwords. Store a session token or refresh token instead.
- Use `SecureZeroMemory` (Windows) or `explicit_bzero` instead of `memset` for sensitive data (compilers can optimize away `memset` on dead stores).

---

### H2. Password buffers never cleared on logout or state transition

**Files:**
- `src/ui/views/LoginView.cpp` lines 111–113
- `src/ui/views/RegisterView.cpp` lines 134–136
- `src/ui/views/SettingsView.cpp` lines 329–331 (logout button)

**Description:**  
When the user transitions between Login ↔ Register, only `statusMessage` is cleared. The password buffers (`m_PasswordBuf`, `m_PasswordRepeatBuf`) retain their content. On logout (`SettingsView.cpp:329–331`), the `onLogout` callback is invoked, but there is no evidence that password buffers are zeroed.

This means passwords linger in process memory indefinitely, vulnerable to memory dumps, crash dumps, or malware that scans process memory.

**Suggested fix:**  
Clear all credential buffers on every state transition and on logout:
```cpp
SecureZeroMemory(passwordBuf, 128);
SecureZeroMemory(passwordRepeatBuf, 128);
```

---

### H3. Password copied to `std::string` without secure erasure

**Files:**
- `src/ui/views/LoginView.cpp` line 81
- `src/ui/views/RegisterView.cpp` line 101

**Description:**  
```cpp
std::string passStr(passwordBuf);  // heap-allocated copy of password
```
The password is copied into a `std::string` which is then captured by the async lambda. When the `std::string` destructor runs, the heap memory is freed but not zeroed. The password data persists in freed heap memory until overwritten by a subsequent allocation.

**Suggested fix:**  
Use a custom secure string class that zeros its buffer on destruction, or manually zero the string's internal buffer before destruction:
```cpp
if (!passStr.empty()) {
    SecureZeroMemory(&passStr[0], passStr.size());
}
```

---

## MEDIUM Findings

### M1. Out-of-bounds array access on `overlayCorner`

**File:** `src/ui/views/SettingsView.cpp` line 286

**Description:**  
```cpp
const char* corners[] = { "Top Left", "Top Right", "Bottom Left", "Bottom Right" };
if (ImGui::BeginCombo("##overlay_pos", corners[ctx.overlayCorner])) {
```
If `ctx.overlayCorner` is loaded from a corrupted config file with a value outside 0–3, this is an out-of-bounds read on a stack array, causing undefined behavior (likely crash or garbage string).

**Suggested fix:**  
Clamp the value before use:
```cpp
int corner = std::clamp(ctx.overlayCorner, 0, 3);
if (ImGui::BeginCombo("##overlay_pos", corners[corner])) {
```

---

### M2. Out-of-bounds array access on `settingsTab`

**File:** `src/ui/views/SettingsView.cpp` line 401

**Description:**  
```cpp
const char* tabs[] = { "Appearance", "Voice", "Keybinds", "Overlay", "Account" };
// ...
ImGui::Text("%s", tabs[ctx.settingsTab]);
```
If `settingsTab` is ever out of range [0,4] (e.g., from memory corruption), this is undefined behavior. While the UI buttons constrain it to 0–4 in normal operation, no defensive check exists.

**Suggested fix:**  
Add bounds clamping:
```cpp
int tab = std::clamp(ctx.settingsTab, 0, 4);
ImGui::Text("%s", tabs[tab]);
```

---

### M3. No input sanitization before network transmission

**Files:**
- `src/ui/views/LoginView.cpp` lines 88–89
- `src/ui/views/RegisterView.cpp` lines 108–109
- `src/ui/views/ChatView.cpp` line 317
- `src/ui/views/SidebarView.cpp` lines 156, 353

**Description:**  
User input from all text fields (email, username, password, chat messages, server names, channel names) is passed directly to `PacketHandler::Create*Payload()` without any sanitization. Control characters (null bytes, newlines, tabs), Unicode direction overrides (U+202E), and extremely long strings (up to buffer max) pass through unchecked.

If the server-side parser has any injection vulnerabilities (SQL, JSON, command), malicious input from the client would exploit them directly.

**Suggested fix:**  
Add client-side input sanitization:
- Strip or reject control characters (0x00–0x1F except newline in chat)
- Enforce reasonable length limits per field type
- Validate email format on client side
- Strip Unicode bidirectional override characters

---

### M4. Message deletion authorization is client-side only

**File:** `src/ui/views/ChatView.cpp` lines 291–297

**Description:**  
```cpp
if (isMe && msg.id > 0) {
    if (ImGui::BeginPopupContextItem(("msg_" + std::to_string(msg.id)).c_str())) {
        if (ImGui::Selectable("Delete Message"))
            netClient.Send(PacketType::Delete_Message_Request,
                PacketHandler::CreateDeleteMessagePayload(msg.id, selectedChannelId, currentUser.username));
```
The delete option is only shown for the user's own messages (`isMe` check), but the delete payload includes `currentUser.username` as a parameter. A modified client could craft delete requests for any message by spoofing the username or message ID. The server must independently verify ownership.

**Suggested fix:**  
This is primarily a server-side fix. The server must verify that the requesting user owns the message. The username in the payload should not be trusted — use the authenticated session identity instead.

---

### M5. Hardcoded buffer sizes create maintenance hazard

**Files:**
- `src/ui/views/LoginView.cpp` lines 60, 71, 83, 87, 91, 95, 100, 113
- `src/ui/views/RegisterView.cpp` lines 56, 67, 78, 89, 103, 107, 111, 115, 120, 123, 136

**Description:**  
Buffer sizes are hardcoded as magic numbers throughout the view functions:
- `ImGui::InputText("##email", emailBuf, 128)` — hardcoded 128
- `strcpy_s(statusMessage, 256, ...)` — hardcoded 256
- `memset(statusMessage, 0, 256)` — hardcoded 256

The actual buffer sizes are defined in `Application.h` (e.g., `m_EmailBuf[128]`, `m_StatusMessage[256]`). If any buffer size changes in Application.h, every hardcoded literal must be updated, or buffer overflows / truncation bugs silently appear.

**Suggested fix:**  
Define named constants for buffer sizes and pass them alongside the buffers, or use `sizeof` where possible:
```cpp
static constexpr size_t kEmailBufSize = 128;
static constexpr size_t kStatusMessageSize = 256;
```

---

### M6. `std::string` temporaries from `substr` passed to `ImGui::Text`

**File:** `src/ui/views/ChatView.cpp` lines 273, 276

**Description:**  
```cpp
ImGui::Text("%s", msg.sender.substr(0, hp).c_str());
ImGui::SameLine(0, 0);
ImGui::Text("%s", msg.sender.substr(hp).c_str());
```
These create temporary `std::string` objects whose `.c_str()` pointers are valid only for the full expression. While this is technically valid C++ (temporaries live until the end of the full expression containing the function call), it is a fragile pattern. If the code were refactored to store `.c_str()` in a variable, it would become a dangling pointer.

**Suggested fix:**  
Store the substring in a named variable:
```cpp
std::string displayName = msg.sender.substr(0, hp);
ImGui::Text("%s", displayName.c_str());
```

---

## LOW Findings

### L1. No email format validation on login/registration

**Files:**
- `src/ui/views/LoginView.cpp` line 79
- `src/ui/views/RegisterView.cpp` line 97

**Description:**  
Only `strlen(emailBuf) > 0` is checked. No regex or basic format validation (e.g., contains `@` and `.`). Users can submit garbage strings as email addresses.

**Suggested fix:**  
Add basic email format validation (at minimum check for `@` character).

---

### L2. No password strength enforcement on registration

**File:** `src/ui/views/RegisterView.cpp` lines 97–98

**Description:**  
The only password checks are: non-empty and the two password fields match. No minimum length, complexity, or common-password-list checking.

**Suggested fix:**  
Enforce a minimum password length (e.g., 8 characters) and optionally check for mixed character classes.

---

### L3. Static buffer in `SingleKeyName` is not thread-safe

**File:** `src/ui/views/SettingsView.cpp` lines 20–27

**Description:**  
```cpp
static char buf[64];
```
`SingleKeyName` uses a function-local static buffer and returns a pointer to it. While ImGui is single-threaded and this is currently safe, the pattern is inherently non-reentrant and would break if called from multiple threads.

**Suggested fix:**  
Accept a caller-provided buffer, or return `std::string`.

---

### L4. Join code buffer not cleared after use

**File:** `src/ui/views/SidebarView.cpp` lines 365–372

**Description:**  
```cpp
static char joinCode[10] = "";
// ...
if (UI::AccentButton("Join", ImVec2(220, 30))) {
    netClient.Send(PacketType::Join_Server_Request, PacketHandler::JoinServerPayload(joinCode, currentUser.username));
    ImGui::CloseCurrentPopup();
    // BUG: joinCode is NOT cleared here
}
```
Unlike `newServerNameBuf` (cleared at line 354 with `memset`) and `newChannelNameBuf` (cleared at line 157), the `joinCode` buffer is never cleared after a join attempt. Old invite codes persist in the input field.

**Suggested fix:**  
Add `memset(joinCode, 0, sizeof(joinCode));` after sending the join request.

---

### L5. No client-side rate limiting on authentication attempts

**Files:**
- `src/ui/views/LoginView.cpp` line 78
- `src/ui/views/RegisterView.cpp` line 96

**Description:**  
The sign-in/register buttons can be clicked repeatedly without any delay or lockout. While the server should enforce rate limits, the client provides no throttling, enabling rapid-fire authentication attempts if the server is permissive.

**Suggested fix:**  
Add a cooldown timer (e.g., 2 seconds) after each authentication attempt. Disable the button while a request is in flight.

---

### L6. No maximum message length enforcement beyond buffer size

**File:** `src/ui/views/ChatView.cpp` line 311

**Description:**  
The chat input buffer is 1024 bytes, enforced by ImGui's `InputText`. But there's no application-level limit (e.g., 500 characters) to prevent abuse. Users can send the maximum buffer length on every message.

**Suggested fix:**  
Enforce a reasonable message length limit (e.g., 500 chars) and show remaining character count.

---

### L7. Config-loaded values not validated before use

**Files:**
- `src/ui/views/SettingsView.cpp` line 305 (`overlayOpacity`)
- `src/ui/Theme.cpp` lines 8–11 (theme ID from config)

**Description:**  
`Theme.cpp` does validate the loaded theme ID (lines 9–10), which is good. But `overlayOpacity` and `overlayCorner` loaded from config are not validated before use in the slider/combo widgets. While ImGui's slider clamps the value during interaction, the initial render with an out-of-range value could cause visual artifacts.

**Suggested fix:**  
Clamp config-loaded values immediately after loading:
```cpp
m_OverlayOpacity = std::clamp(m_OverlayOpacity, 0.2f, 1.0f);
m_OverlayCorner = std::clamp(m_OverlayCorner, 0, 3);
```

---

## INFO Findings

### I1. Invite code displayed to all users in text channel header

**File:** `src/ui/views/ChatView.cpp` line 248

**Description:**  
```cpp
ImGui::Text("Invite: %s", currentServer.inviteCode.c_str());
```
The server invite code is shown to every user in the text channel header. If invite codes are intended to be restricted (e.g., admin-only), this is an information disclosure. If they're intentionally public (like Discord), this is fine.

---

### I2. Static `s_volPopupMember` persists across context changes

**File:** `src/ui/views/ChatView.cpp` line 79

**Description:**  
`static std::string s_volPopupMember` retains the last right-clicked member name across server/channel switches. If the popup is opened in one server then the user switches servers, the stale member name could appear in the popup.

---

### I3. Static `type` variable in CreateChannelPopup persists between openings

**File:** `src/ui/views/SidebarView.cpp` line 143

**Description:**  
`static int type = 0;` means the channel type radio button selection persists between popup openings. If a user creates a voice channel, closes the popup, and reopens it, the voice type will still be selected, which may be unexpected.

---

### I4. Empty server name produces invisible button

**File:** `src/ui/views/SidebarView.cpp` line 399

**Description:**  
`server.name.substr(0, 2)` on an empty server name produces an empty string button label. While `substr` handles this without crashing, the button would be invisible/unlabeled.

---

### I5. Format string safety is correctly handled throughout

**Files:** All UI files

**Description:**  
All `ImGui::Text`, `ImGui::TextWrapped`, `ImGui::TextDisabled`, and `ImGui::SetTooltip` calls that render user-controlled data use `"%s"` format specifiers. No format string vulnerabilities were found. This is commendable defensive coding.

---

## Architecture Observations

1. **No CSRF-equivalent protection:** Network messages include the username in the payload rather than relying on a server-assigned session token. A malicious client could spoof any username.

2. **No message signing/integrity:** Chat messages and control packets have no HMAC or signature, so a MITM could tamper with messages in transit.

3. **Windows-only code paths:** `SettingsView.cpp` includes `<windows.h>` and uses `VK_*` constants, `GetAsyncKeyState`, `MapVirtualKeyA`, and `GetKeyNameTextA`. This entire settings view is non-portable.

4. **`std::function` passed by value:** `ChatView.h` line 20 passes `std::function<void(const std::string&, float)> setUserVolume` by value, causing a heap allocation on every frame for the function object copy. Should be `const std::function<...>&`.
