#include "Application.h"
#include <tchar.h>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <queue>
#include <thread>
#include <random>
#include <windows.h>
#include <mmsystem.h>
#include <shlobj.h>
#include <dxgi1_4.h>
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "shell32.lib")

namespace {

    static constexpr int kPaceIntervalMs = 10; // one Opus frame = 10ms

    // ---------------------------------------------------------------------------
    // VoiceLinkProbe
    // On startup, fires N small UDP probe packets to the server and receives the
    // echoes.  Computes per-path RTT, one-way loss percentage and inter-arrival
    // jitter.  The results are fed into AudioEngine::ApplyProbeResults() so the
    // jitter buffer starts at a well-calibrated level instead of a static guess.
    //
    // The probe runs in a background thread so it does not block the UI.
    // It automatically re-runs every kRepeatIntervalSec seconds to keep the
    // buffer calibrated as network conditions evolve.
    // ---------------------------------------------------------------------------
    struct ProbeResult {
        float rttMs = 0.f;
        float jitterMs = 0.f;
        float lossPct = 0.f;
        int   samplesUsed = 0;
    };

} // anonymous namespace

#include "../../vendor/imgui.h"
#include "../../vendor/imgui_impl_win32.h"
#include "../../vendor/imgui_impl_dx11.h"
#include "../ui/TextureManager.h"
#include <nlohmann/json.hpp>

#include "../ui/Theme.h"
#include "../ui/Styles.h"
#include "../ui/Components.h"
#include "../ui/views/LoginView.h"
#include "../ui/views/RegisterView.h"
#include "../ui/views/SidebarView.h"
#include "../ui/views/ChatView.h"
#include "../ui/views/SettingsView.h"
#include "../shared/PacketHandler.h"
#include "../core/Logger.h"

using json = nlohmann::json;

namespace TalkMe {

    static std::string GetExeDirectory() {
        wchar_t path[MAX_PATH] = {};
        if (::GetModuleFileNameW(nullptr, path, MAX_PATH) == 0) return {};
        const int len = ::WideCharToMultiByte(CP_UTF8, 0, path, -1, nullptr, 0, nullptr, nullptr);
        if (len <= 0) return {};
        std::string s(static_cast<size_t>(len), '\0');
        ::WideCharToMultiByte(CP_UTF8, 0, path, -1, s.data(), len, nullptr, nullptr);
        s.resize(static_cast<size_t>(len) - 1);  // drop null terminator
        const size_t last = s.find_last_of("\\/");
        return (last != std::string::npos) ? s.substr(0, last) : s;
    }

    // Returns the path ConfigManager uses for session.dat and all local data.
    // Using the same path guarantees 2fa_token.dat lives right next to session.dat.
    static std::string GetDeviceTokenPath() {
        return ConfigManager::GetConfigDirectory() + "\\2fa_token.dat";
    }

    // Reads the persisted 2FA device token from 2fa_token.dat.
    // If the file doesn't exist yet (first run before any 2FA), returns empty.
    static std::string LoadDeviceToken() {
        std::ifstream in(GetDeviceTokenPath());
        if (!in) return {};
        std::string token;
        std::getline(in, token);
        while (!token.empty() && (token.back() == '\r' || token.back() == '\n'))
            token.pop_back();
        return (token.size() >= 16) ? token : std::string{};
    }

    // Generates a new random token, saves it to 2fa_token.dat, returns it.
    static std::string GenerateAndSaveDeviceToken() {
        std::random_device rd;
        std::mt19937_64 rng(rd());
        std::uniform_int_distribution<uint64_t> dist;
        char buf[33];
        snprintf(buf, sizeof(buf), "%016llx%016llx",
            (unsigned long long)dist(rng), (unsigned long long)dist(rng));
        std::string token(buf);
        std::ofstream out(GetDeviceTokenPath());
        if (out) out << token << "\n";
        return token;
    }

    // Returns the device token for this machine.
    // On first call: returns empty (no 2FA set up yet - do NOT generate one here,
    //   we only create a token when the user actually enables and verifies 2FA).
    static std::string GetOrCreateDeviceId() {
        return LoadDeviceToken(); // empty string is fine - server skips trusted check
    }

    void Application::UpdateOverlay() {
        if (!m_OverlayEnabled || m_ActiveVoiceChannelId == -1) {
            m_Overlay.UpdateMembers({});
            return;
        }
        std::vector<OverlayMember> members;
        float now = (float)ImGui::GetTime();
        for (const auto& name : m_VoiceMembers) {
            OverlayMember om;
            om.name = name;
            size_t h = name.find('#');
            if (h != std::string::npos) om.name = name.substr(0, h);

            if (name == m_CurrentUser.username) {
                om.isMuted = m_SelfMuted;
                om.isDeafened = m_SelfDeafened;
            } else {
                auto sit = m_UserMuteStates.find(name);
                if (sit != m_UserMuteStates.end()) {
                    om.isMuted = sit->second.muted;
                    om.isDeafened = sit->second.deafened;
                }
            }

            auto it = m_SpeakingTimers.find(name);
            om.isSpeaking = (it != m_SpeakingTimers.end() && (now - it->second) < 0.5f);
            members.push_back(om);
        }
        m_Overlay.UpdateMembers(members);
    }

    Application::Application(const std::string& title, int width, int height) : m_Title(title), m_Width(width), m_Height(height) { }
    Application::~Application() { if (!m_CleanedUp) Cleanup(); }

    bool Application::Initialize() {
#ifdef _DEBUG
        if (AllocConsole()) {
            FILE* _ = nullptr;
            freopen_s(&_, "CONOUT$", "w", stdout);
            freopen_s(&_, "CONOUT$", "w", stderr);
        }
#endif
        TalkMe::Logger::Instance().Initialize("talkme_debug.log");
        TalkMe::Logger::Instance().InitializeStatsLog("talkme_voice_stats.log");
        std::string exeDir = GetExeDirectory();
        std::string tracePath = exeDir.empty() ? "talkme_voice_trace.log" : (exeDir + "\\talkme_voice_trace.log");
        bool voiceTraceEnable = false;
#ifdef _DEBUG
        voiceTraceEnable = true;
#else
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4996)
#endif
        const char* e = std::getenv("TALKME_VOICE_TRACE");
#ifdef _MSC_VER
#pragma warning(pop)
#endif
        voiceTraceEnable = (e && (e[0] == '1' || e[0] == 'y' || e[0] == 'Y'));
        if (!voiceTraceEnable) {
            std::string marker = exeDir + "\\talkme_voice_trace.enable";
            std::ifstream f(marker);
            voiceTraceEnable = f.good();
        }
#endif
        TalkMe::Logger::Instance().InitializeVoiceTrace(tracePath, voiceTraceEnable);
        m_Window.SetOnDestroy([this]() { if (m_QuitEvent) ::SetEvent(static_cast<HANDLE>(m_QuitEvent)); });
        m_Window.SetOnResize([this](UINT w, UINT h) { m_Graphics.OnResize(w, h); });
        if (!m_Window.Create(m_Width, m_Height, m_Title) || !m_Graphics.Init(m_Window.GetHwnd()) || !m_Graphics.InitImGui()) return false;
    m_NetworkWakeEvent = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    m_QuitEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (m_NetworkWakeEvent) {
        m_NetClient.SetWakeEvent(m_NetworkWakeEvent);
        m_VoiceTransport.SetWakeCallback([this]() {
            if (m_NetworkWakeEvent) ::SetEvent(static_cast<HANDLE>(m_NetworkWakeEvent));
        });
    }
    m_AudioEngine.SetReceiverReportCallback([this](const TalkMe::ReceiverReportPayload& report) {
        if (m_CurrentState == AppState::MainApp && m_NetClient.IsConnected() && m_ActiveVoiceChannelId != -1) {
            TalkMe::ReceiverReportPayload out = report;
            out.ToNetwork();
            std::vector<uint8_t> payload(sizeof(TalkMe::ReceiverReportPayload));
            std::memcpy(payload.data(), &out, sizeof(TalkMe::ReceiverReportPayload));
            m_NetClient.SendRaw(TalkMe::PacketType::Receiver_Report, payload);
        }
    });

    int noiseModeClamped = (std::max)(0, (std::min)(3, m_NoiseMode));
    m_AudioEngine.SetNoiseSuppressionMode(static_cast<TalkMe::NoiseSuppressionMode>(noiseModeClamped));
    m_AudioEngine.SetMicTestEnabled(m_TestMicEnabled);
    m_AudioEngine.InitializeWithSequence([this](const std::vector<uint8_t>& opusData, uint32_t seqNum) {
        try {
            if (m_CurrentState != AppState::MainApp || !m_NetClient.IsConnected() || m_ActiveVoiceChannelId == -1)
                return;
            // Strictly forbid TCP voice routing. Late audio causes jitter starvation.
            if (!m_UseUdpVoice)
                return;
            auto payload = PacketHandler::CreateVoicePayloadOpus(m_CurrentUser.username, opusData, seqNum);
            m_SendPacer.Enqueue(payload);
            char traceBuf[256];
            std::snprintf(traceBuf, sizeof(traceBuf),
                "step=send path=udp seq=%u opus_bytes=%zu payload_bytes=%zu",
                seqNum, opusData.size(), payload.size());
            TalkMe::Logger::Instance().LogVoiceTraceBufNonBlocking(traceBuf);
            if (m_VoiceRedundancyEnabled) {
                m_PendingVoiceRedundant = true;
                m_LastVoiceRedundantTime = std::chrono::steady_clock::now();
                m_LastVoiceRedundantPayload = payload;
                m_LastVoiceRedundantOpus = opusData;
                m_LastVoiceRedundantTimestamp = seqNum * 960;
            }
            if (m_LocalEcho) {
                auto parsed = PacketHandler::ParseVoicePayloadOpus(payload);
                if (parsed.valid)
                    m_AudioEngine.PushIncomingAudioWithSequence(parsed.sender, parsed.opusData, parsed.sequenceNumber);
            }
        }
        catch (...) {
        }
    });
    auto pushVoiceIfNew = [this](const std::string& sender, const std::vector<uint8_t>& opusData, uint32_t seqNum) {
        if (m_ActiveVoiceChannelIdForVoice.load(std::memory_order_relaxed) == -1) return;
        auto key = std::make_pair(sender, seqNum);
        {
            std::lock_guard<std::mutex> lock(m_VoiceDedupeMutex);
            if (m_VoiceDedupeSet.count(key)) return;
            m_VoiceDedupeSet.insert(key);
            m_VoiceDedupeQueue.push_back(key);
            while (m_VoiceDedupeQueue.size() > kMaxVoiceDedupe) {
                auto old = m_VoiceDedupeQueue.front();
                m_VoiceDedupeQueue.pop_front();
                m_VoiceDedupeSet.erase(old);
            }
        }
        m_AudioEngine.PushIncomingAudioWithSequence(sender, opusData, seqNum);
        std::lock_guard<std::mutex> lock(m_RecentSpeakersMutex);
        m_RecentSpeakers.push_back(sender);
    };
    m_NetClient.SetVoiceCallback([this, pushVoiceIfNew](const std::vector<uint8_t>& packetData) {
        auto parsed = PacketHandler::ParseVoicePayloadOpus(packetData);
        if (parsed.valid) {
            char traceBuf[256];
            std::snprintf(traceBuf, sizeof(traceBuf),
                "step=recv path=tcp sender=%s seq=%u opus_bytes=%zu",
                parsed.sender.c_str(), parsed.sequenceNumber, parsed.opusData.size());
            TalkMe::Logger::Instance().LogVoiceTraceBufNonBlocking(traceBuf);
            pushVoiceIfNew(parsed.sender, parsed.opusData, parsed.sequenceNumber);
        }
    });
    m_VoiceTransport.SetReceiveCallback([this, pushVoiceIfNew](const uint8_t* data, size_t length) {
        if (length == 13 && data[0] == 0xEE) {
            std::vector<uint8_t> pkt(data, data + length);
            HandleProbeEcho(pkt);
            return;
        }

        std::vector<uint8_t> pkt(data, data + length);
        auto parsed = PacketHandler::ParseVoicePayloadOpus(pkt);
        if (parsed.valid) {
            char traceBuf[256];
            std::snprintf(traceBuf, sizeof(traceBuf),
                "step=recv path=udp sender=%s seq=%u opus_bytes=%zu",
                parsed.sender.c_str(), parsed.sequenceNumber, parsed.opusData.size());
            TalkMe::Logger::Instance().LogVoiceTraceBufNonBlocking(traceBuf);
            pushVoiceIfNew(parsed.sender, parsed.opusData, parsed.sequenceNumber);
        }
    });
    if (m_VoiceTransport.Start(m_ServerIP, (uint16_t)VOICE_PORT)) {
        m_UseUdpVoice = true;
        m_LastUdpHelloTime = std::chrono::steady_clock::now();
        TalkMe::Logger::Instance().LogVoiceTrace("udp_start ok server=" + m_ServerIP + " port=" + std::to_string(VOICE_PORT));

        m_SendPacer.Start([this](const std::vector<uint8_t>& pkt) {
            m_VoiceTransport.SendVoicePacket(pkt);
        });
        StartLinkProbe();
    }
    else {
        TalkMe::Logger::Instance().LogVoiceTrace("udp_start fail server=" + m_ServerIP + " port=" + std::to_string(VOICE_PORT));
    }
    ConfigManager::Get().LoadKeybinds(m_KeyMuteMic, m_KeyDeafen);
    ConfigManager::Get().LoadOverlay(m_OverlayEnabled, m_OverlayCorner, m_OverlayOpacity);
    m_NoiseMode = ConfigManager::Get().LoadNoiseSuppressionMode(1);
    {
        int noiseModeClamped = (std::max)(0, (std::min)(3, m_NoiseMode));
        m_AudioEngine.SetNoiseSuppressionMode(static_cast<TalkMe::NoiseSuppressionMode>(noiseModeClamped));
    }
    std::string volPath = ConfigManager::GetConfigDirectory() + "\\user_volumes.json";
    std::ifstream volIn(volPath);
    if (volIn) {
        try {
            auto j = nlohmann::json::parse(volIn);
            for (auto it = j.begin(); it != j.end(); ++it)
                if (it.value().is_number())
                    m_UserVolumes[it.key()] = it.value().get<float>();
            for (const auto& [uid, gain] : m_UserVolumes)
                m_AudioEngine.SetUserGain(uid, gain);
        }
        catch (...) {}
    }
    m_Sounds.Generate();
    m_Overlay.Create(GetModuleHandle(nullptr));
    m_Overlay.SetCorner(m_OverlayCorner);
    m_Overlay.SetOpacity(m_OverlayOpacity);
    m_Overlay.SetEnabled(m_OverlayEnabled);
    m_DeviceId = ConfigManager::Get().GetDeviceId();
    UserSession session = ConfigManager::Get().LoadSession();
    if (session.isLoggedIn) {
        m_CurrentUser = session;
        strcpy_s(m_EmailBuf, sizeof(m_EmailBuf), session.email.c_str());
        strcpy_s(m_PasswordBuf, sizeof(m_PasswordBuf), session.password.c_str());
        m_ValidatingSession = true;
        auto email = session.email;
        auto pass = session.password;
        std::string hwid = m_DeviceId;
        m_NetClient.ConnectAsync(m_ServerIP, m_ServerPort,
            [this, email, pass, hwid](bool success) {
                if (success) {
                    m_NetClient.Send(PacketType::Login_Request,
                        PacketHandler::CreateLoginPayload(email, pass, hwid));
                }
                else {
                    m_ValidatingSession = false;
                    strcpy_s(m_StatusMessage, sizeof(m_StatusMessage), "Server offline. Please try again.");
                }
            });
    }
    return true;
    }

    // -------------------------------------------------------------------------
    // Main loop (Run)
    // Order: 1) Pump messages  2) Wait (quit/wake/timeout)  3) Network + echo
    //        4) Speaking indicators  5) Keybinds  6) Voice channel state
    //        7) Device check  8) ImGui render  9) Present
    // Voice send path: AudioEngine encode thread -> onMicData -> VoiceSendPacer -> UDP/TCP.
    // -------------------------------------------------------------------------
    void Application::Run() {
        try {
            bool done = false;
            HANDLE wakeHandle = static_cast<HANDLE>(m_NetworkWakeEvent);
            HANDLE quitHandle = static_cast<HANDLE>(m_QuitEvent);

            while (!done) {
                // STEP 1: Pump window messages
                MSG msg;
                while (::PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
                    ::TranslateMessage(&msg);
                    ::DispatchMessage(&msg);
                    if (msg.message == WM_QUIT) {
                        done = true;
                        break;
                    }
                }

                if (done || !m_Window.GetHwnd()) break;

                // STEP 2: Wait for quit, network wake, or any input (mouse, keyboard, paint).
                // OPTIMIZATION: Use INFINITE (0% CPU) when idle, but cap at 250ms when in
                // a voice channel to ensure background keepalives (Ping/Hello) don't starve.
                DWORD waitMs = INFINITE;
                if (m_ActiveVoiceChannelId != -1 && m_NetClient.IsConnected()) {
                    waitMs = 250;
                }

                if (quitHandle) {
                    HANDLE handles[2];
                    DWORD count = 0;
                    if (wakeHandle) handles[count++] = wakeHandle;
                    handles[count++] = quitHandle;
                    DWORD r = ::MsgWaitForMultipleObjectsEx(count, handles, waitMs, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
                    if (r >= WAIT_OBJECT_0 && r < WAIT_OBJECT_0 + count && r == WAIT_OBJECT_0 + count - 1)
                        done = true;
                }
                else if (wakeHandle) {
                    ::MsgWaitForMultipleObjectsEx(1, &wakeHandle, waitMs, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
                }
                else {
                    ::MsgWaitForMultipleObjectsEx(0, nullptr, waitMs, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
                }
                if (done || !m_Window.GetHwnd()) break;

                // STEP 3: Network (messages + echo live)
                if (m_ActiveVoiceChannelId != -1 && m_NetClient.IsConnected())
                    EnsureEchoLiveEnabled();
                else if (m_ActiveVoiceChannelId == -1)
                    m_EchoLiveEnabled = false;
                UpdateEchoLive();
                ProcessNetworkMessages();

                // STEP 4: Update speaking indicators
                {
                    std::lock_guard<std::mutex> lock(m_RecentSpeakersMutex);
                    float t = (float)ImGui::GetTime();
                    for (const auto& uid : m_RecentSpeakers) {
                        m_SpeakingTimers[uid] = t;
                        for (const auto& vm : m_VoiceMembers) {
                            if (vm == uid) break;
                            size_t h = vm.find('#');
                            if (h != std::string::npos && vm.substr(0, h) == uid) {
                                m_SpeakingTimers[vm] = t;
                                break;
                            }
                        }
                    }
                    m_RecentSpeakers.clear();
                }

                // Local speaking detection
                if (m_ActiveVoiceChannelId != -1 && m_AudioEngine.IsCaptureEnabled() && !m_SelfMuted) {
                    float micLevel = m_AudioEngine.GetMicActivity();
                    if (micLevel > 0.002f) {
                        m_SpeakingTimers[m_CurrentUser.username] = (float)ImGui::GetTime();
                    }
                }

                // Clear stale state when disconnected
                if (!m_NetClient.IsConnected() && m_CurrentState == AppState::MainApp) {
                    if (!m_VoiceMembers.empty()) m_VoiceMembers.clear();
                    if (m_ActiveVoiceChannelId != -1) m_ActiveVoiceChannelId = -1;
                    m_UserMuteStates.clear();
                    if (m_ScreenShare.iAmSharing) { m_ScreenCapture.Stop(); m_ScreenShare.iAmSharing = false; }
                    m_ScreenShare.someoneSharing = false;
                }

                // Remove self from voice members when leaving
                if (m_ActiveVoiceChannelId == -1 && !m_VoiceMembers.empty()) {
                    auto it = std::find(m_VoiceMembers.begin(), m_VoiceMembers.end(), m_CurrentUser.username);
                    if (it != m_VoiceMembers.end()) {
                        m_VoiceMembers.erase(it);
                        m_AudioEngine.OnVoiceStateUpdate((int)m_VoiceMembers.size());
                    }
                }

                // STEP 5: Keybind polling
                static bool s_MuteComboWasActive = false;
                static bool s_DeafenComboWasActive = false;
                if (!m_KeyMuteMic.empty()) {
                    bool allDown = true;
                    for (int vk : m_KeyMuteMic) {
                        if (!(GetAsyncKeyState(vk) & 0x8000)) { allDown = false; break; }
                    }
                    if (allDown && !s_MuteComboWasActive) {
                        m_SelfMuted = !m_SelfMuted;
                        if (!m_SelfMuted && m_SelfDeafened) m_SelfDeafened = false;
                    }
                    s_MuteComboWasActive = allDown;
                }
                if (!m_KeyDeafen.empty()) {
                    bool allDown = true;
                    for (int vk : m_KeyDeafen) {
                        if (!(GetAsyncKeyState(vk) & 0x8000)) { allDown = false; break; }
                    }
                    if (allDown && !s_DeafenComboWasActive) {
                        m_SelfDeafened = !m_SelfDeafened;
                        if (m_SelfDeafened) m_SelfMuted = true;
                    }
                    s_DeafenComboWasActive = allDown;
                }
                m_AudioEngine.SetSelfMuted(m_SelfMuted);
                m_AudioEngine.SetSelfDeafened(m_SelfDeafened);

                // STEP 6: Voice channel state
                m_ActiveVoiceChannelIdForVoice.store(m_ActiveVoiceChannelId, std::memory_order_relaxed);
                if (m_ActiveVoiceChannelId != -1) {
                    auto tel = m_AudioEngine.GetTelemetry();
                    m_VoiceRedundancyEnabled = (tel.packetLossPercentage > 40.0f);
                    if (m_PendingVoiceRedundant) {
                        auto now = std::chrono::steady_clock::now();
                        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_LastVoiceRedundantTime).count();
                        if (elapsed >= 5) {
                            if (!m_LastVoiceRedundantPayload.empty() && m_UseUdpVoice) {
                                m_VoiceTransport.SendVoicePacket(m_LastVoiceRedundantPayload);
                            }
                            m_PendingVoiceRedundant = false;
                        }
                    }
                }
                else {
                    m_PendingVoiceRedundant = false;
                }

                if (m_ActiveVoiceChannelId != m_PrevActiveVoiceChannelId) {
                    m_AudioEngine.ClearRemoteTracks();
                    m_JoinSoundPlayedFor.clear();
                    if (m_UseUdpVoice && m_NetClient.IsConnected() && !m_CurrentUser.username.empty()) {
                        m_VoiceTransport.SendHello(m_CurrentUser.username, m_ActiveVoiceChannelId);
                        m_LastUdpHelloTime = std::chrono::steady_clock::now();
                        TalkMe::Logger::Instance().LogVoiceTrace("hello_sent user=" + m_CurrentUser.username + " channel=" + std::to_string(m_ActiveVoiceChannelId));
                    }
                    m_PrevActiveVoiceChannelId = m_ActiveVoiceChannelId;
                }

                m_AudioEngine.SetCaptureEnabled(m_ActiveVoiceChannelId != -1);

                // Voice state and stats
                if (m_ActiveVoiceChannelId != -1 && m_NetClient.IsConnected()) {
                    auto now = std::chrono::steady_clock::now();
                    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - m_LastVoiceStateRequestTime).count();
                    if (elapsed >= m_VoiceConfig.voiceStateRequestIntervalSec) {
                        m_NetClient.Send(PacketType::Join_Voice_Channel, PacketHandler::JoinVoiceChannelPayload(m_ActiveVoiceChannelId));
                        m_LastVoiceStateRequestTime = now;
                    }
                    auto statsElapsed = std::chrono::duration_cast<std::chrono::seconds>(now - m_LastVoiceStatsLogTime).count();
                    if (statsElapsed >= 15) {
                        m_LastVoiceStatsLogTime = now;
                        auto tel = m_AudioEngine.GetTelemetry();
                        Logger::Instance().LogVoiceStats(m_ActiveVoiceChannelId, m_VoiceMembers,
                            tel.totalPacketsReceived, tel.totalPacketsLost, tel.packetLossPercentage,
                            tel.avgJitterMs, tel.currentBufferMs, tel.currentEncoderBitrateKbps);
                        float pingMs = (m_UseUdpVoice && m_VoiceTransport.GetRttMs() > 0.0f)
                            ? m_VoiceTransport.GetRttMs() : tel.currentLatencyMs;
                        m_NetClient.Send(PacketType::Voice_Stats_Report,
                            PacketHandler::CreateVoiceStatsPayload(m_ActiveVoiceChannelId, pingMs,
                                tel.packetLossPercentage, tel.avgJitterMs, tel.currentBufferMs));
                    }
                }

                // UDP voice keepalive
                if (m_UseUdpVoice && m_NetClient.IsConnected() && !m_CurrentUser.username.empty()) {
                    auto now = std::chrono::steady_clock::now();
                    int helloIntervalMs = m_VoiceConfig.keepaliveIntervalMs / 2;
                    if (helloIntervalMs < 1000) helloIntervalMs = 1000;
                    auto helloElapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_LastUdpHelloTime).count();
                    if (helloElapsedMs >= helloIntervalMs) {
                        m_VoiceTransport.SendHello(m_CurrentUser.username, m_ActiveVoiceChannelId);
                        m_LastUdpHelloTime = now;
                    }
                    auto pingElapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_LastPingTime).count();
                    if (pingElapsedMs >= 2000) {
                        m_VoiceTransport.SendPing();
                        m_LastPingTime = now;
                    }
                }

                m_AudioEngine.Update();

                // Broadcast mute/deafen state changes to voice channel
                {
                    static bool s_prevMuted = false;
                    static bool s_prevDeafened = false;
                    if (m_ActiveVoiceChannelId != -1 &&
                        (m_SelfMuted != s_prevMuted || m_SelfDeafened != s_prevDeafened)) {
                        nlohmann::json muteJ;
                        muteJ["muted"] = m_SelfMuted;
                        muteJ["deafened"] = m_SelfDeafened;
                        m_NetClient.Send(PacketType::Voice_Mute_State, muteJ.dump());
                        s_prevMuted = m_SelfMuted;
                        s_prevDeafened = m_SelfDeafened;
                    }
                }

                UpdateOverlay();

                // Update screen share texture from incoming frames
                if (m_ScreenShare.frameUpdated && !m_ScreenShare.lastFrameData.empty()) {
                    auto& tm = TalkMe::TextureManager::Get();
                    if (!tm.GetTexture("screenshare"))
                        tm.SetDevice(m_Graphics.GetDevice());
                    tm.LoadFromBMP("screenshare",
                        m_ScreenShare.lastFrameData.data(),
                        (int)m_ScreenShare.lastFrameData.size());
                    m_ScreenShare.frameUpdated = false;
                }

                // Drain messages again so we never call Present() after user closed the window
                while (::PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
                    ::TranslateMessage(&msg);
                    ::DispatchMessage(&msg);
                    if (msg.message == WM_QUIT) done = true;
                }
                if (done || !m_Window.GetHwnd()) break;

                // STEP 7: Only render when we are the foreground window (or during splash so we can show the window).
                const bool isForeground = (m_Window.GetHwnd() && ::GetForegroundWindow() == m_Window.GetHwnd());
                const bool duringSplash = (m_SplashFrames < 3);

                if (!isForeground && !duringSplash && m_Graphics.IsValid() && FAILED(m_Graphics.GetDeviceRemovedReason())) {
                    LOG_ERROR("Device lost while in background");
                    done = true;
                    break;
                }
                if (isForeground || duringSplash) {
                    if (!m_Graphics.IsValid()) {
                        done = true;
                        break;
                    }
                    if (FAILED(m_Graphics.GetDeviceRemovedReason())) {
                        LOG_ERROR("Device lost during rendering");
                        done = true;
                        break;
                    }

                    RECT cr = {};
                    if (::GetClientRect(m_Window.GetHwnd(), &cr)) {
                        const UINT cw = static_cast<UINT>(cr.right - cr.left);
                        const UINT ch = static_cast<UINT>(cr.bottom - cr.top);
                        if (cw > 0 && ch > 0)
                            m_Graphics.OnResize(cw, ch);
                    }

                    // STEP 8: Render ImGui
                    try {
                        m_Graphics.ImGuiNewFrame();
                        ImGui::NewFrame();
                        RenderUI();
                        ImGui::Render();
                        ImVec4 bg = TalkMe::UI::Styles::BgMain();
                        const float clear_color[4] = { bg.x, bg.y, bg.z, bg.w };
                        if (!m_Graphics.ClearAndPresent(clear_color, ImGui::GetDrawData())) {
                            done = true;
                            break;
                        }
                    }
                    catch (const std::exception& e) {
                        LOG_ERROR(std::string("Rendering exception: ") + e.what());
                        done = true;
                        break;
                    }
                    catch (...) {
                        LOG_ERROR("Rendering unknown exception");
                        done = true;
                        break;
                    }
                }
            }
        }
        catch (const std::exception& e) {
            LOG_ERROR(std::string("Run() exception: ") + e.what());
        }
        catch (...) {
            LOG_ERROR("Run() unknown exception");
        }

        Cleanup();
    }

    void Application::RenderUI() {
        const ImGuiViewport* viewport = ImGui::GetMainViewport(); ImGui::SetNextWindowPos(viewport->WorkPos); ImGui::SetNextWindowSize(viewport->WorkSize);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f); ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f); ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar;
        ImGui::Begin("MainShell", nullptr, flags);
        if (m_SplashFrames < 3) {
            float cx = viewport->WorkPos.x + viewport->WorkSize.x * 0.5f;
            float cy = viewport->WorkPos.y + viewport->WorkSize.y * 0.5f;
            float r = 24.0f;
            float t = (float)ImGui::GetTime();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImU32 col = IM_COL32(255, 255, 255, 200);
            dl->PathClear();
            dl->PathArcTo(ImVec2(cx, cy), r, t, t + 3.14159265f * 1.5f, 24);
            dl->PathStroke(col, 0, 3.0f);
            m_SplashFrames++;
            if (m_SplashFrames == 3)
                m_Window.Show(SW_MAXIMIZE);
            ImGui::End(); ImGui::PopStyleVar(3);
            return;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_F1))
            m_ShowShortcuts = !m_ShowShortcuts;
        if (ImGui::IsKeyPressed(ImGuiKey_F2))
            m_ShowFriendList = !m_ShowFriendList;

        if (m_CurrentState == AppState::Login) RenderLogin();
        else if (m_CurrentState == AppState::Login2FA) RenderLogin2FA();
        else if (m_CurrentState == AppState::Register) RenderRegister();
        else if (m_CurrentState == AppState::MainApp) RenderMainApp();
        ImGui::End(); ImGui::PopStyleVar(3);

        // Friends panel removed — now rendered as a full tab (like Settings)
        if (false) {
            if (ImGui::Begin("_unused_")) {
                ImGui::PushItemWidth(-1);
                ImGui::InputTextWithHint("##friend_search", "Add friend (username#tag)...", m_FriendSearchBuf, sizeof(m_FriendSearchBuf));
                ImGui::PopItemWidth();
                ImGui::SameLine();
                if (ImGui::SmallButton("Add") && strlen(m_FriendSearchBuf) > 0) {
                    nlohmann::json fj; fj["u"] = std::string(m_FriendSearchBuf);
                    m_NetClient.Send(PacketType::Friend_Request, fj.dump());
                    memset(m_FriendSearchBuf, 0, sizeof(m_FriendSearchBuf));
                }
                ImGui::Separator();

                int pendingCount = 0;
                for (const auto& f : m_Friends) {
                    if (f.status == "pending" && f.direction == "received") pendingCount++;
                }
                if (pendingCount > 0) {
                    ImGui::TextColored(ImVec4(1, 0.7f, 0.2f, 1), "Pending Requests (%d)", pendingCount);
                    for (const auto& f : m_Friends) {
                        if (f.status != "pending" || f.direction != "received") continue;
                        std::string disp = f.username;
                        size_t hp = disp.find('#');
                        if (hp != std::string::npos) disp = disp.substr(0, hp);
                        ImGui::Text("%s", disp.c_str());
                        ImGui::SameLine();
                        if (ImGui::SmallButton(("Accept##" + f.username).c_str())) {
                            nlohmann::json aj; aj["u"] = f.username;
                            m_NetClient.Send(PacketType::Friend_Accept, aj.dump());
                        }
                        ImGui::SameLine();
                        if (ImGui::SmallButton(("Reject##" + f.username).c_str())) {
                            nlohmann::json rj; rj["u"] = f.username;
                            m_NetClient.Send(PacketType::Friend_Reject, rj.dump());
                        }
                    }
                    ImGui::Separator();
                }

                if (!m_CurrentCall.state.empty()) {
                    std::string callDisp = m_CurrentCall.otherUser;
                    size_t chp = callDisp.find('#');
                    if (chp != std::string::npos) callDisp = callDisp.substr(0, chp);

                    if (m_CurrentCall.state == "calling") {
                        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.1f, 0.15f, 0.25f, 1.0f));
                        ImGui::BeginChild("CallBanner", ImVec2(0, 40), true);
                        ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1), "Calling %s...", callDisp.c_str());
                        ImGui::SameLine();
                        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.7f, 0.15f, 0.15f, 1.0f));
                        if (ImGui::Button("Cancel", ImVec2(60, 24))) {
                            nlohmann::json rj; rj["to"] = m_CurrentCall.otherUser;
                            m_NetClient.Send(PacketType::Call_Reject, rj.dump());
                            m_CurrentCall = {};
                        }
                        ImGui::PopStyleColor();
                        ImGui::EndChild();
                        ImGui::PopStyleColor();
                    } else if (m_CurrentCall.state == "active") {
                        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.08f, 0.2f, 0.1f, 1.0f));
                        ImGui::BeginChild("CallBanner", ImVec2(0, 40), true);
                        ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.4f, 1), "In call: %s", callDisp.c_str());
                        ImGui::SameLine();
                        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.7f, 0.15f, 0.15f, 1.0f));
                        if (ImGui::Button("End", ImVec2(50, 24))) {
                            nlohmann::json rj; rj["to"] = m_CurrentCall.otherUser;
                            m_NetClient.Send(PacketType::Call_Reject, rj.dump());
                            m_CurrentCall = {};
                        }
                        ImGui::PopStyleColor();
                        ImGui::EndChild();
                        ImGui::PopStyleColor();
                    }
                    ImGui::Separator();
                }

                ImGui::Text("Friends");
                for (const auto& f : m_Friends) {
                    if (f.status != "accepted") continue;
                    std::string disp = f.username;
                    size_t hp = disp.find('#');
                    if (hp != std::string::npos) disp = disp.substr(0, hp);
                    bool online = m_OnlineUsers.count(f.username) > 0;
                    ImU32 dotCol = online ? IM_COL32(80, 220, 100, 255) : IM_COL32(120, 120, 125, 255);
                    ImVec2 pos = ImGui::GetCursorScreenPos();
                    ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(pos.x + 6, pos.y + 8), 4.0f, dotCol);
                    ImGui::Dummy(ImVec2(16, 0)); ImGui::SameLine();
                    ImGui::Text("%s", disp.c_str());
                    ImGui::SameLine();
                    if (online && m_CurrentCall.state.empty() && ImGui::SmallButton(("Call##" + f.username).c_str())) {
                        nlohmann::json cj; cj["to"] = f.username;
                        m_NetClient.Send(PacketType::Call_Request, cj.dump());
                        m_CurrentCall.otherUser = f.username;
                        m_CurrentCall.state = "calling";
                    }
                    ImGui::SameLine();
                    if (ImGui::SmallButton(("Message##" + f.username).c_str())) {
                        m_ActiveDMUser = f.username;
                        m_DirectMessages.clear();
                        nlohmann::json hj; hj["u"] = f.username;
                        m_NetClient.Send(PacketType::DM_History_Request, hj.dump());
                    }
                    ImGui::SameLine();
                    if (ImGui::SmallButton(("Remove##" + f.username).c_str())) {
                        nlohmann::json rj; rj["u"] = f.username;
                        m_NetClient.Send(PacketType::Friend_Reject, rj.dump());
                    }
                }

                if (m_Friends.empty() || std::none_of(m_Friends.begin(), m_Friends.end(), [](const FriendEntry& f) { return f.status == "accepted"; })) {
                    ImGui::TextDisabled("No friends yet. Add someone by their username#tag.");
                }

                if (!m_ActiveDMUser.empty()) {
                    ImGui::Separator();
                    std::string dmDisp = m_ActiveDMUser;
                    size_t dhp = dmDisp.find('#');
                    if (dhp != std::string::npos) dmDisp = dmDisp.substr(0, dhp);
                    ImGui::Text("DM: %s", dmDisp.c_str());
                    ImGui::SameLine();
                    if (ImGui::SmallButton("X##closedm")) m_ActiveDMUser.clear();

                    ImGui::BeginChild("DMMessages", ImVec2(0, -32), true);
                    for (const auto& dm : m_DirectMessages) {
                        bool isMe = (dm.sender == m_CurrentUser.username);
                        std::string senderDisp = dm.sender;
                        size_t shp = senderDisp.find('#');
                        if (shp != std::string::npos) senderDisp = senderDisp.substr(0, shp);
                        ImGui::TextColored(isMe ? ImVec4(0.4f, 0.6f, 1.0f, 1.0f) : ImVec4(1.0f, 0.5f, 0.3f, 1.0f),
                            "%s", senderDisp.c_str());
                        ImGui::SameLine();
                        ImGui::TextDisabled("%s", dm.timestamp.c_str());
                        ImGui::TextWrapped("%s", dm.content.c_str());
                        ImGui::Dummy(ImVec2(0, 4));
                    }
                    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) ImGui::SetScrollHereY(1.0f);
                    ImGui::EndChild();

                    ImGui::PushItemWidth(-60);
                    bool dmSend = ImGui::InputText("##dm_in", m_DMInputBuf, sizeof(m_DMInputBuf), ImGuiInputTextFlags_EnterReturnsTrue);
                    ImGui::PopItemWidth();
                    ImGui::SameLine();
                    if ((ImGui::SmallButton("Send##dm") || dmSend) && strlen(m_DMInputBuf) > 0) {
                        nlohmann::json dj;
                        dj["to"] = m_ActiveDMUser;
                        dj["msg"] = std::string(m_DMInputBuf);
                        m_NetClient.Send(PacketType::DM_Send, dj.dump());
                        memset(m_DMInputBuf, 0, sizeof(m_DMInputBuf));
                    }
                }
            }
            ImGui::End();
        }

        if (m_ChessUI.active) {
            ImGui::SetNextWindowSize(ImVec2(400, 480), ImGuiCond_FirstUseEver);
            bool chessOpen = m_ChessUI.active;
            if (ImGui::Begin("Chess", &chessOpen)) {
                std::string oppDisp = m_ChessUI.opponent;
                size_t hp = oppDisp.find('#');
                if (hp != std::string::npos) oppDisp = oppDisp.substr(0, hp);

                bool inCheck = m_ChessEngine.IsKingInCheck(m_ChessUI.isWhite);
                ImGui::Text("vs %s  |  You: %s  |  %s%s",
                    oppDisp.c_str(),
                    m_ChessUI.isWhite ? "White" : "Black",
                    m_ChessUI.myTurn ? "Your turn" : "Waiting...",
                    (m_ChessUI.myTurn && inCheck) ? "  CHECK!" : "");

                float cellSz = 42.0f;
                ImDrawList* dl = ImGui::GetWindowDrawList();
                ImVec2 origin = ImGui::GetCursorScreenPos();

                // Highlight legal moves
                std::vector<std::pair<int,int>> legalTargets;
                if (m_ChessUI.selectedRow >= 0) {
                    for (int tr = 0; tr < 8; tr++)
                        for (int tc = 0; tc < 8; tc++)
                            if (m_ChessEngine.IsLegalMove(m_ChessUI.selectedRow, m_ChessUI.selectedCol, tr, tc))
                                legalTargets.push_back({tr, tc});
                }

                for (int r = 0; r < 8; r++) {
                    for (int c = 0; c < 8; c++) {
                        ImVec2 p0(origin.x + c * cellSz, origin.y + r * cellSz);
                        ImVec2 p1(p0.x + cellSz, p0.y + cellSz);
                        bool light = ((r + c) % 2 == 0);
                        ImU32 bgCol = light ? IM_COL32(238, 238, 210, 255) : IM_COL32(118, 150, 86, 255);
                        if (m_ChessUI.selectedRow == r && m_ChessUI.selectedCol == c)
                            bgCol = IM_COL32(246, 246, 105, 255);
                        dl->AddRectFilled(p0, p1, bgCol);

                        // Legal move dots
                        for (const auto& [lr, lc] : legalTargets) {
                            if (lr == r && lc == c) {
                                float cx = p0.x + cellSz * 0.5f, cy = p0.y + cellSz * 0.5f;
                                if (m_ChessEngine.board[r][c] != ' ')
                                    dl->AddCircle(ImVec2(cx, cy), cellSz * 0.45f, IM_COL32(0, 0, 0, 80), 0, 3.0f);
                                else
                                    dl->AddCircleFilled(ImVec2(cx, cy), 6.0f, IM_COL32(0, 0, 0, 60));
                            }
                        }

                        char piece = m_ChessEngine.board[r][c];
                        if (piece != ' ') {
                            const char* symbol = "";
                            switch (piece) {
                                case 'K': symbol = "\xe2\x99\x94"; break; case 'Q': symbol = "\xe2\x99\x95"; break;
                                case 'R': symbol = "\xe2\x99\x96"; break; case 'B': symbol = "\xe2\x99\x97"; break;
                                case 'N': symbol = "\xe2\x99\x98"; break; case 'P': symbol = "\xe2\x99\x99"; break;
                                case 'k': symbol = "\xe2\x99\x9a"; break; case 'q': symbol = "\xe2\x99\x9b"; break;
                                case 'r': symbol = "\xe2\x99\x9c"; break; case 'b': symbol = "\xe2\x99\x9d"; break;
                                case 'n': symbol = "\xe2\x99\x9e"; break; case 'p': symbol = "\xe2\x99\x9f"; break;
                            }
                            ImVec2 tsz = ImGui::CalcTextSize(symbol);
                            dl->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 1.6f,
                                ImVec2(p0.x + (cellSz - tsz.x * 1.6f) * 0.5f, p0.y + (cellSz - tsz.y * 1.6f) * 0.3f),
                                IM_COL32(20, 20, 20, 255), symbol);
                        }
                    }
                }

                ImGui::InvisibleButton("##chessboard", ImVec2(8 * cellSz, 8 * cellSz));
                if (m_ChessUI.myTurn && !m_ChessEngine.gameOver && ImGui::IsItemClicked()) {
                    ImVec2 mouse = ImGui::GetMousePos();
                    int clickCol = (int)((mouse.x - origin.x) / cellSz);
                    int clickRow = (int)((mouse.y - origin.y) / cellSz);
                    if (clickRow >= 0 && clickRow < 8 && clickCol >= 0 && clickCol < 8) {
                        if (m_ChessUI.selectedRow == -1) {
                            char piece = m_ChessEngine.board[clickRow][clickCol];
                            bool isMyPiece = m_ChessUI.isWhite ? (piece >= 'A' && piece <= 'Z') : (piece >= 'a' && piece <= 'z');
                            if (isMyPiece) { m_ChessUI.selectedRow = clickRow; m_ChessUI.selectedCol = clickCol; }
                        } else {
                            if (m_ChessEngine.IsLegalMove(m_ChessUI.selectedRow, m_ChessUI.selectedCol, clickRow, clickCol)) {
                                nlohmann::json mj;
                                mj["opponent"] = m_ChessUI.opponent;
                                mj["fr"] = m_ChessUI.selectedRow; mj["fc"] = m_ChessUI.selectedCol;
                                mj["tr"] = clickRow; mj["tc"] = clickCol;
                                m_ChessEngine.MakeMove(m_ChessUI.selectedRow, m_ChessUI.selectedCol, clickRow, clickCol);
                                m_ChessUI.myTurn = false;
                                m_NetClient.Send(PacketType::Game_Move, mj.dump());
                            }
                            m_ChessUI.selectedRow = -1; m_ChessUI.selectedCol = -1;
                        }
                    }
                }

                if (m_ChessEngine.gameOver)
                    ImGui::TextColored(ImVec4(1, 0.8f, 0.2f, 1), "%s", m_ChessEngine.result.c_str());
                if (ImGui::Button("Resign")) { m_ChessUI.active = false; m_ChessUI.opponent.clear(); }
            }
            ImGui::End();
            if (!chessOpen) { m_ChessUI.active = false; m_ChessUI.opponent.clear(); }
        }

        if (!m_ChessUI.active && !m_ChessUI.opponent.empty()) {
            ImGui::SetNextWindowSize(ImVec2(280, 80), ImGuiCond_FirstUseEver);
            if (ImGui::Begin("Game Challenge", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
                std::string challenger = m_ChessUI.opponent;
                size_t hp = challenger.find('#');
                if (hp != std::string::npos) challenger = challenger.substr(0, hp);
                ImGui::Text("%s challenges you to Chess!", challenger.c_str());
                if (ImGui::Button("Accept")) {
                    nlohmann::json aj; aj["to"] = m_ChessUI.opponent; aj["action"] = "accept"; aj["game"] = "chess";
                    m_NetClient.Send(PacketType::Game_Accept, aj.dump());
                    m_ChessEngine.Reset();
                    m_ChessUI.active = true;
                    m_ChessUI.isWhite = false;
                    m_ChessUI.myTurn = false;
                }
                ImGui::SameLine();
                if (ImGui::Button("Decline")) { m_ChessUI.opponent.clear(); }
            }
            ImGui::End();
        }

        // Incoming call popup — centered overlay with green/red buttons
        if (m_CurrentCall.state == "ringing") {
            ImGui::SetNextWindowSize(ImVec2(320, 140), ImGuiCond_Always);
            ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
            ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.12f, 0.12f, 0.16f, 0.95f));
            if (ImGui::Begin("##IncomingCall", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize)) {
                std::string caller = m_CurrentCall.otherUser;
                size_t hp = caller.find('#');
                if (hp != std::string::npos) caller = caller.substr(0, hp);

                ImGui::SetCursorPosX((320 - ImGui::CalcTextSize("Incoming Call").x) * 0.5f);
                ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "Incoming Call");
                ImGui::Dummy(ImVec2(0, 4));
                ImGui::SetCursorPosX((320 - ImGui::CalcTextSize(caller.c_str()).x) * 0.5f);
                ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.0f, 1.0f), "%s", caller.c_str());
                ImGui::Dummy(ImVec2(0, 12));

                float btnW = 90.0f, btnGap = 10.0f;
                float totalW = btnW * 3 + btnGap * 2;
                ImGui::SetCursorPosX((320 - totalW) * 0.5f);

                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.65f, 0.25f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.2f, 0.75f, 0.3f, 1.0f));
                if (ImGui::Button("Accept", ImVec2(btnW, 34))) {
                    nlohmann::json aj; aj["from"] = m_CurrentCall.otherUser;
                    m_NetClient.Send(PacketType::Call_Accept, aj.dump());
                }
                ImGui::PopStyleColor(2);

                ImGui::SameLine(0, btnGap);
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.4f, 0.4f, 0.45f, 1.0f));
                if (ImGui::Button("Snooze 10m", ImVec2(btnW, 34))) {
                    m_CurrentCall = {};
                }
                ImGui::PopStyleColor();

                ImGui::SameLine(0, btnGap);
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.7f, 0.15f, 0.15f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.85f, 0.2f, 0.2f, 1.0f));
                if (ImGui::Button("Decline", ImVec2(btnW, 34))) {
                    nlohmann::json rj; rj["from"] = m_CurrentCall.otherUser;
                    m_NetClient.Send(PacketType::Call_Reject, rj.dump());
                    m_CurrentCall = {};
                }
                ImGui::PopStyleColor(2);

                // Play ring sound
                static float s_lastRing = 0;
                float now = (float)ImGui::GetTime();
                if (now - s_lastRing > 2.0f) {
                    m_Sounds.PlayJoin();
                    s_lastRing = now;
                }
            }
            ImGui::End();
            ImGui::PopStyleColor();
        }

        if (m_Racing.active) {
            ImGui::SetNextWindowSize(ImVec2(620, 560), ImGuiCond_FirstUseEver);
            bool raceOpen = m_Racing.active;
            if (ImGui::Begin("Car Racing", &raceOpen)) {
                std::string oppDisp = m_Racing.opponent.name;
                size_t hp = oppDisp.find('#');
                if (hp != std::string::npos) oppDisp = oppDisp.substr(0, hp);
                ImGui::Text("Lap %d/%d  |  Time: %.1fs  |  Speed: %.0f",
                    m_Racing.player.lap, RacingGame::kTotalLaps, m_Racing.raceTime,
                    std::abs(m_Racing.player.speed) * 50.0f);

                ImDrawList* dl = ImGui::GetWindowDrawList();
                ImVec2 origin = ImGui::GetCursorScreenPos();

                // Draw track (elliptical ring)
                float cx = origin.x + RacingGame::kTrackCX;
                float cy = origin.y + RacingGame::kTrackCY;
                for (int i = 0; i < 64; i++) {
                    float a1 = i * 6.28318f / 64.0f;
                    float a2 = (i + 1) * 6.28318f / 64.0f;
                    for (float rMul : {0.7f, 1.0f, 1.3f}) {
                        ImVec2 p1(cx + std::cos(a1) * RacingGame::kTrackRX * rMul, cy + std::sin(a1) * RacingGame::kTrackRY * rMul);
                        ImVec2 p2(cx + std::cos(a2) * RacingGame::kTrackRX * rMul, cy + std::sin(a2) * RacingGame::kTrackRY * rMul);
                        dl->AddLine(p1, p2, rMul == 1.0f ? IM_COL32(180, 180, 180, 100) : IM_COL32(100, 100, 100, 255), rMul == 1.0f ? 1.0f : 2.0f);
                    }
                }

                // Draw track surface
                dl->AddEllipse(ImVec2(cx, cy), ImVec2(RacingGame::kTrackRX, RacingGame::kTrackRY), IM_COL32(60, 60, 65, 180), 0.0f, 64, 2.0f);

                // Draw cars
                auto drawCar = [&](const RaceCar& car, ImU32 color) {
                    float carX = origin.x + car.x;
                    float carY = origin.y + car.y;
                    float ca = std::cos(car.angle), sa = std::sin(car.angle);
                    float len = 12.0f, wid = 6.0f;
                    ImVec2 pts[4] = {
                        {carX + ca * len - sa * wid, carY + sa * len + ca * wid},
                        {carX + ca * len + sa * wid, carY + sa * len - ca * wid},
                        {carX - ca * len + sa * wid, carY - sa * len - ca * wid},
                        {carX - ca * len - sa * wid, carY - sa * len + ca * wid}
                    };
                    dl->AddConvexPolyFilled(pts, 4, color);
                };

                drawCar(m_Racing.player, IM_COL32(50, 150, 255, 255));
                drawCar(m_Racing.opponent, IM_COL32(255, 80, 80, 255));

                ImGui::InvisibleButton("##racetrack", ImVec2(600, 500));

                // Controls
                bool accel = ImGui::IsKeyDown(ImGuiKey_W) || ImGui::IsKeyDown(ImGuiKey_UpArrow);
                bool brake = ImGui::IsKeyDown(ImGuiKey_S) || ImGui::IsKeyDown(ImGuiKey_DownArrow);
                bool left = ImGui::IsKeyDown(ImGuiKey_A) || ImGui::IsKeyDown(ImGuiKey_LeftArrow);
                bool right = ImGui::IsKeyDown(ImGuiKey_D) || ImGui::IsKeyDown(ImGuiKey_RightArrow);

                float dt = ImGui::GetIO().DeltaTime;
                m_Racing.UpdatePlayer(accel, brake, left, right, dt);

                // Send position to opponent
                static float s_lastSend = 0;
                float now = (float)ImGui::GetTime();
                if (now - s_lastSend > 0.05f) {
                    nlohmann::json rj;
                    rj["opponent"] = m_Racing.opponent.name;
                    rj["x"] = m_Racing.player.x; rj["y"] = m_Racing.player.y;
                    rj["angle"] = m_Racing.player.angle; rj["speed"] = m_Racing.player.speed;
                    rj["lap"] = m_Racing.player.lap; rj["cp"] = m_Racing.player.checkpoint;
                    m_NetClient.Send(PacketType::Game_Move, rj.dump());
                    s_lastSend = now;
                }

                if (m_Racing.finished)
                    ImGui::TextColored(ImVec4(1, 0.8f, 0.2f, 1), "%s wins!", m_Racing.winner.c_str());

                ImGui::Text("WASD / Arrow keys to drive");
                if (ImGui::Button("Quit Race")) { m_Racing.active = false; }
            }
            ImGui::End();
            if (!raceOpen) m_Racing.active = false;
        }

        if (m_ShowShortcuts) {
            ImGui::SetNextWindowSize(ImVec2(300, 250), ImGuiCond_FirstUseEver);
            if (ImGui::Begin("Keyboard Shortcuts", &m_ShowShortcuts)) {
                ImGui::Text("F1  - This help panel");
                ImGui::Text("F2  - Friends / Direct Messages");
                ImGui::Separator();
                ImGui::Text("Ctrl+Shift+M  - Toggle Mute");
                ImGui::Text("Ctrl+Shift+D  - Toggle Deafen");
                ImGui::Separator();
                ImGui::Text("Enter  - Send message");
                ImGui::Text("Right-click message  - Reply / Edit / Delete / Pin / React");
                ImGui::Text("Right-click channel  - Delete channel");
                ImGui::Text("Right-click server   - Copy invite / Leave");
                ImGui::Separator();
                ImGui::TextDisabled("v%s", "1.4.0");
            }
            ImGui::End();
        }
    }

    void Application::RenderLogin() { UI::Views::RenderLogin(m_NetClient, m_CurrentState, m_EmailBuf, m_PasswordBuf, m_StatusMessage, m_ServerIP, m_ServerPort, m_DeviceId, m_ValidatingSession); }

    void Application::RenderLogin2FA() {
        float winW = ImGui::GetWindowWidth();
        float winH = ImGui::GetWindowHeight();
        float cardW = TalkMe::UI::Styles::LoginCardWidth;
        float cardH = 320.0f;
        float cardR = TalkMe::UI::Styles::LoginCardRounding;
        float cx = (winW - cardW) * 0.5f;
        float cy = (winH - cardH) * 0.5f;
        if (cy < 40) cy = 40;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 wp = ImGui::GetCursorScreenPos();
        ImVec2 cardTL(wp.x + cx, wp.y + cy);
        ImVec2 cardBR(cardTL.x + cardW, cardTL.y + cardH);
        dl->AddRectFilled(cardTL, cardBR, TalkMe::UI::Styles::ColBgCard(), cardR);
        float pad = 40.0f;
        float fieldW = cardW - pad * 2;
        ImGui::SetCursorPos(ImVec2(cx + pad, cy + 36));
        ImGui::BeginGroup();
        ImGui::PushStyleColor(ImGuiCol_Text, TalkMe::UI::Styles::Accent());
        ImGui::SetWindowFontScale(1.7f);
        ImVec2 tsz = ImGui::CalcTextSize("TalkMe");
        ImGui::SetCursorPosX(cx + (cardW - tsz.x) * 0.5f);
        ImGui::Text("TalkMe");
        ImGui::SetWindowFontScale(1.0f);
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0, 4));
        ImVec2 subSz = ImGui::CalcTextSize("Two-Factor Authentication Required");
        ImGui::SetCursorPosX(cx + (cardW - subSz.x) * 0.5f);
        ImGui::TextDisabled("Two-Factor Authentication Required");
        ImGui::Dummy(ImVec2(0, 28));
        ImGui::SetCursorPosX(cx + pad);
        ImGui::PushStyleColor(ImGuiCol_Text, TalkMe::UI::Styles::TextSecondary());
        ImGui::Text("6-Digit Code");
        ImGui::PopStyleColor();
        ImGui::SetCursorPosX(cx + pad);
        ImGui::PushItemWidth(fieldW);
        ImGui::InputText("##2facode", m_2FACodeBuf, sizeof(m_2FACodeBuf), ImGuiInputTextFlags_CharsDecimal);
        ImGui::PopItemWidth();
        ImGui::Dummy(ImVec2(0, 24));
        ImGui::SetCursorPosX(cx + pad);
        if (UI::AccentButton("Submit", ImVec2(fieldW, 42))) {
            if (strlen(m_2FACodeBuf) == 6) {
                nlohmann::json j;
                j["email"] = std::string(m_EmailBuf);
                j["code"] = std::string(m_2FACodeBuf);
                // Also send the device token so the server can call TrustDevice.
                // This is the key step: after this the server stores the token and
                // subsequent logins with the same token skip the 2FA prompt entirely.
                if (!m_DeviceId.empty())
                    j["hwid"] = m_DeviceId;
                m_NetClient.Send(PacketType::Submit_2FA_Login_Request, j.dump());
            }
        }
        ImGui::Dummy(ImVec2(0, 12));
        ImGui::SetCursorPosX(cx + pad);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Text, TalkMe::UI::Styles::TextMuted());
        if (ImGui::Button("Cancel", ImVec2(fieldW, 28))) {
            m_CurrentState = AppState::Login;
            m_2FACodeBuf[0] = '\0';
        }
        ImGui::PopStyleColor(4);
        if (m_StatusMessage[0]) {
            ImGui::Dummy(ImVec2(0, 8));
            ImVec2 sts = ImGui::CalcTextSize(m_StatusMessage);
            ImGui::SetCursorPosX(cx + (cardW - sts.x) * 0.5f);
            ImGui::PushStyleColor(ImGuiCol_Text, TalkMe::UI::Styles::Error());
            ImGui::Text("%s", m_StatusMessage);
            ImGui::PopStyleColor();
        }
        ImGui::EndGroup();
    }

    void Application::RenderRegister() { UI::Views::RenderRegister(m_NetClient, m_CurrentState, m_EmailBuf, m_UsernameBuf, m_PasswordBuf, m_PasswordRepeatBuf, m_StatusMessage, m_ServerIP, m_ServerPort); }

    void Application::RenderMainApp() {
        UI::Views::VoiceInfoData vi;
        vi.serverVersion = m_ServerVersion;
        vi.voicePath = m_UseUdpVoice ? "UDP" : "TCP";
        if (m_ActiveVoiceChannelId != -1) {
            auto tel = m_AudioEngine.GetTelemetry();
            float udpRtt = m_VoiceTransport.GetRttMs();
            vi.pingHistory = m_VoiceTransport.GetPingHistory();
            vi.lastPingMs = m_VoiceTransport.GetLastRttMs();
            vi.avgPingMs = (udpRtt > 0.0f) ? udpRtt : tel.currentLatencyMs;
            if (!vi.pingHistory.empty()) {
                float sum = 0.f;
                for (float p : vi.pingHistory) sum += p;
                vi.avgPingMs = sum / static_cast<float>(vi.pingHistory.size());
            }
            vi.packetLossPercent = tel.packetLossPercentage;
            vi.packetsReceived = tel.totalPacketsReceived;
            vi.packetsLost = tel.totalPacketsLost;
            vi.currentBufferMs = tel.currentBufferMs;
            vi.encoderBitrateKbps = tel.currentEncoderBitrateKbps;
        }
        vi.echoLiveEnabled = m_EchoLiveEnabled;
        vi.echoLiveHistory = std::vector<float>(m_EchoLiveHistory.begin(), m_EchoLiveHistory.end());
        vi.currentEchoLossPct = m_EchoLiveHistory.empty() ? 0.f : m_EchoLiveHistory.back();
        UI::Views::RenderSidebar(m_NetClient, m_CurrentUser, m_CurrentState, m_ServerList, m_SelectedServerId, m_SelectedChannelId, m_ActiveVoiceChannelId, m_VoiceMembers, m_NewServerNameBuf, m_NewChannelNameBuf, m_ShowSettings, m_SelfMuted, m_SelfDeafened, vi, nullptr, [this]() { EnsureEchoLiveEnabled(); }, &m_UnreadCounts, &m_ShowFriendList);

        if (m_ShowSettings) {
            static std::vector<TalkMe::AudioDeviceInfo> s_InputDevs;
            static std::vector<TalkMe::AudioDeviceInfo> s_OutputDevs;
            static bool s_DevsLoaded = false;
            if (!s_DevsLoaded) {
                s_InputDevs = m_AudioEngine.GetInputDevices();
                s_OutputDevs = m_AudioEngine.GetOutputDevices();
                s_DevsLoaded = true;
            }
            UI::Views::SettingsContext sctx{
                [this]() {
                    ConfigManager::Get().ClearSession();
                    m_NetClient.Disconnect();
                    m_CurrentState = AppState::Login;
                    m_SelectedServerId = -1;
                    m_SelectedChannelId = -1;
                    m_ActiveVoiceChannelId = -1;
                    m_ShowSettings = false;
                },
                [this](int inIdx, int outIdx) {
                    m_AudioEngine.ReinitDevice(inIdx, outIdx);
                },
                m_SettingsTab,
                m_KeyMuteMic,
                m_KeyDeafen,
                m_SelectedInputDevice,
                m_SelectedOutputDevice,
                s_InputDevs,
                s_OutputDevs,
                m_OverlayEnabled,
                m_OverlayCorner,
                m_OverlayOpacity,
                [this]() {
                    m_Overlay.SetEnabled(m_OverlayEnabled);
                    m_Overlay.SetCorner(m_OverlayCorner);
                    m_Overlay.SetOpacity(m_OverlayOpacity);
                    ConfigManager::Get().SaveOverlay(m_OverlayEnabled, m_OverlayCorner, m_OverlayOpacity);
                    UpdateOverlay();
                },
                [this]() {
                    m_OverlayEnabled = false;
                    m_OverlayCorner = 1;
                    m_OverlayOpacity = 0.85f;
                    m_Overlay.SetEnabled(m_OverlayEnabled);
                    m_Overlay.SetCorner(m_OverlayCorner);
                    m_Overlay.SetOpacity(m_OverlayOpacity);
                    ConfigManager::Get().SaveOverlay(m_OverlayEnabled, m_OverlayCorner, m_OverlayOpacity);
                    UpdateOverlay();
                },
                m_Is2FAEnabled,
                m_IsSettingUp2FA,
                m_2FASecretStr,
                m_2FAUriStr,
                m_2FASetupCodeBuf,
                sizeof(m_2FASetupCodeBuf),
                m_2FASetupStatusMessage,
                [this]() { m_NetClient.Send(PacketType::Generate_2FA_Secret_Request, "{}"); },
                [this]() {
                    nlohmann::json j;
                    j["code"] = std::string(m_2FASetupCodeBuf);
                    m_NetClient.Send(PacketType::Verify_2FA_Setup_Request, j.dump());
                },
                [this]() {
                    m_IsSettingUp2FA = false;
                    m_2FASecretStr.clear();
                    m_2FAUriStr.clear();
                    m_2FASetupCodeBuf[0] = '\0';
                    m_2FASetupStatusMessage[0] = '\0';
                },
                [this]() { m_IsDisabling2FA = true; },
                m_IsDisabling2FA,
                m_Disable2FACodeBuf,
                sizeof(m_Disable2FACodeBuf),
                [this]() {
                    nlohmann::json j; j["code"] = std::string(m_Disable2FACodeBuf);
                    m_NetClient.Send(PacketType::Disable_2FA_Request, j.dump());
                },
                [this]() {
                    m_IsDisabling2FA = false;
                    m_Disable2FACodeBuf[0] = '\0';
                },
                m_NoiseMode,
                m_TestMicEnabled,
                m_TestMicEnabled ? m_AudioEngine.GetMicActivity() : 0.0f,
                [this](int mode) {
                    int m = (std::max)(0, (std::min)(3, mode));
                    m_NoiseMode = m;
                    m_AudioEngine.SetNoiseSuppressionMode(static_cast<TalkMe::NoiseSuppressionMode>(m));
                    ConfigManager::Get().SaveNoiseSuppressionMode(m);
                },
                [this](bool enabled) {
                    m_TestMicEnabled = enabled;
                    m_AudioEngine.SetMicTestEnabled(enabled);
                },
                [this]() {
                    m_SettingsTab = 0;
                    m_KeyMuteMic.clear();
                    m_KeyDeafen.clear();
                    m_SelectedInputDevice = -1;
                    m_SelectedOutputDevice = -1;
                    m_NoiseMode = 1;
                    m_TestMicEnabled = false;
                    m_OverlayEnabled = false;
                    m_OverlayCorner = 1;
                    m_OverlayOpacity = 0.85f;
                    m_AudioEngine.SetNoiseSuppressionMode(TalkMe::NoiseSuppressionMode::RNNoise);
                    m_AudioEngine.SetMicTestEnabled(false);
                    m_AudioEngine.ReinitDevice(-1, -1);
                    TalkMe::UI::Styles::SetTheme(TalkMe::UI::ThemeId::Midnight);
                    ConfigManager::Get().SaveTheme(0);
                    ConfigManager::Get().SaveKeybinds(m_KeyMuteMic, m_KeyDeafen);
                    ConfigManager::Get().SaveOverlay(m_OverlayEnabled, m_OverlayCorner, m_OverlayOpacity);
                    ConfigManager::Get().SaveNoiseSuppressionMode(m_NoiseMode);
                    m_Overlay.SetEnabled(m_OverlayEnabled);
                    m_Overlay.SetCorner(m_OverlayCorner);
                    m_Overlay.SetOpacity(m_OverlayOpacity);
                    UpdateOverlay();
                }
            };
            UI::Views::RenderSettings(sctx);
        }
        else if (m_ShowFriendList) {
            float left = UI::Styles::MainContentLeftOffset;
            float friendW = ImGui::GetWindowWidth() - left - UI::Styles::ServerRailWidth;
            ImGui::SetCursorPos(ImVec2(left, 0));
            ImGui::PushStyleColor(ImGuiCol_ChildBg, UI::Styles::BgChat());
            ImGui::BeginChild("FriendsTab", ImVec2(friendW, ImGui::GetWindowHeight()), false);

            ImGui::Dummy(ImVec2(0, 20));
            ImGui::Indent(36);
            ImGui::PushStyleColor(ImGuiCol_Text, UI::Styles::Accent());
            ImGui::SetWindowFontScale(1.3f);
            ImGui::Text("Friends");
            ImGui::SetWindowFontScale(1.0f);
            ImGui::PopStyleColor();

            ImGui::Dummy(ImVec2(0, 8));
            ImGui::PushItemWidth(friendW - 180);
            ImGui::InputTextWithHint("##friend_add", "Add friend (username#tag)...", m_FriendSearchBuf, sizeof(m_FriendSearchBuf));
            ImGui::PopItemWidth();
            ImGui::SameLine();
            if (UI::AccentButton("Add Friend", ImVec2(100, 28)) && strlen(m_FriendSearchBuf) > 0) {
                nlohmann::json fj; fj["u"] = std::string(m_FriendSearchBuf);
                m_NetClient.Send(PacketType::Friend_Request, fj.dump());
                memset(m_FriendSearchBuf, 0, sizeof(m_FriendSearchBuf));
            }

            ImGui::Dummy(ImVec2(0, 8));
            ImGui::Separator();
            ImGui::Dummy(ImVec2(0, 8));

            // Pending requests
            int pendingCount = 0;
            for (const auto& f : m_Friends)
                if (f.status == "pending" && f.direction == "received") pendingCount++;

            if (pendingCount > 0) {
                ImGui::TextColored(ImVec4(1, 0.7f, 0.2f, 1), "Pending Requests (%d)", pendingCount);
                ImGui::Dummy(ImVec2(0, 4));
                for (const auto& f : m_Friends) {
                    if (f.status != "pending" || f.direction != "received") continue;
                    std::string disp = f.username;
                    size_t hp = disp.find('#');
                    if (hp != std::string::npos) disp = disp.substr(0, hp);
                    ImGui::Text("  %s", disp.c_str());
                    ImGui::SameLine();
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.6f, 0.25f, 1));
                    if (ImGui::SmallButton(("Accept##" + f.username).c_str())) {
                        nlohmann::json aj; aj["u"] = f.username;
                        m_NetClient.Send(PacketType::Friend_Accept, aj.dump());
                    }
                    ImGui::PopStyleColor();
                    ImGui::SameLine();
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.6f, 0.15f, 0.15f, 1));
                    if (ImGui::SmallButton(("Reject##" + f.username).c_str())) {
                        nlohmann::json rj; rj["u"] = f.username;
                        m_NetClient.Send(PacketType::Friend_Reject, rj.dump());
                    }
                    ImGui::PopStyleColor();
                }
                ImGui::Dummy(ImVec2(0, 8));
                ImGui::Separator();
                ImGui::Dummy(ImVec2(0, 8));
            }

            // Online friends
            ImGui::Text("Online");
            ImGui::Dummy(ImVec2(0, 4));
            for (const auto& f : m_Friends) {
                if (f.status != "accepted") continue;
                bool online = m_OnlineUsers.count(f.username) > 0;
                if (!online) continue;
                std::string disp = f.username;
                size_t hp = disp.find('#');
                if (hp != std::string::npos) disp = disp.substr(0, hp);

                ImVec2 pos = ImGui::GetCursorScreenPos();
                ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(pos.x + 42, pos.y + 8), 5.0f, IM_COL32(80, 220, 100, 255));
                ImGui::Text("      %s", disp.c_str());
                ImGui::SameLine();
                if (m_CurrentCall.state.empty() && ImGui::SmallButton(("Call##" + f.username).c_str())) {
                    nlohmann::json cj; cj["to"] = f.username;
                    m_NetClient.Send(PacketType::Call_Request, cj.dump());
                    m_CurrentCall.otherUser = f.username;
                    m_CurrentCall.state = "calling";
                }
                ImGui::SameLine();
                if (ImGui::SmallButton(("Message##" + f.username).c_str())) {
                    m_ActiveDMUser = f.username;
                    m_DirectMessages.clear();
                    nlohmann::json hj; hj["u"] = f.username;
                    m_NetClient.Send(PacketType::DM_History_Request, hj.dump());
                }
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Text, UI::Styles::TextMuted());
                if (ImGui::SmallButton(("Remove##" + f.username).c_str())) {
                    nlohmann::json rj; rj["u"] = f.username;
                    m_NetClient.Send(PacketType::Friend_Reject, rj.dump());
                }
                ImGui::PopStyleColor();
            }

            ImGui::Dummy(ImVec2(0, 12));
            ImGui::Text("Offline");
            ImGui::Dummy(ImVec2(0, 4));
            for (const auto& f : m_Friends) {
                if (f.status != "accepted") continue;
                bool online = m_OnlineUsers.count(f.username) > 0;
                if (online) continue;
                std::string disp = f.username;
                size_t hp = disp.find('#');
                if (hp != std::string::npos) disp = disp.substr(0, hp);

                ImVec2 pos = ImGui::GetCursorScreenPos();
                ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(pos.x + 42, pos.y + 8), 5.0f, IM_COL32(120, 120, 125, 255));
                ImGui::PushStyleColor(ImGuiCol_Text, UI::Styles::TextMuted());
                ImGui::Text("      %s", disp.c_str());
                ImGui::PopStyleColor();
                ImGui::SameLine();
                if (ImGui::SmallButton(("Message##" + f.username).c_str())) {
                    m_ActiveDMUser = f.username;
                    m_DirectMessages.clear();
                    nlohmann::json hj; hj["u"] = f.username;
                    m_NetClient.Send(PacketType::DM_History_Request, hj.dump());
                }
            }

            // DM conversation area
            if (!m_ActiveDMUser.empty()) {
                ImGui::Dummy(ImVec2(0, 12));
                ImGui::Separator();
                ImGui::Dummy(ImVec2(0, 8));
                std::string dmDisp = m_ActiveDMUser;
                size_t dhp = dmDisp.find('#');
                if (dhp != std::string::npos) dmDisp = dmDisp.substr(0, dhp);
                ImGui::Text("Direct Messages: %s", dmDisp.c_str());
                ImGui::SameLine();
                if (ImGui::SmallButton("X##closedm")) m_ActiveDMUser.clear();

                float dmH = ImGui::GetWindowHeight() - ImGui::GetCursorPosY() - 40;
                if (dmH < 100) dmH = 100;
                ImGui::BeginChild("DMMessages", ImVec2(friendW - 80, dmH), true);
                for (const auto& dm : m_DirectMessages) {
                    bool isMe = (dm.sender == m_CurrentUser.username);
                    std::string sDisp = dm.sender;
                    size_t shp = sDisp.find('#');
                    if (shp != std::string::npos) sDisp = sDisp.substr(0, shp);
                    ImGui::TextColored(isMe ? ImVec4(0.4f, 0.6f, 1.0f, 1) : ImVec4(1.0f, 0.5f, 0.3f, 1), "%s", sDisp.c_str());
                    ImGui::SameLine();
                    ImGui::TextDisabled("%s", dm.timestamp.c_str());
                    ImGui::TextWrapped("%s", dm.content.c_str());
                    ImGui::Dummy(ImVec2(0, 4));
                }
                if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) ImGui::SetScrollHereY(1.0f);
                ImGui::EndChild();

                ImGui::PushItemWidth(friendW - 160);
                bool dmSend = ImGui::InputText("##dm_in", m_DMInputBuf, sizeof(m_DMInputBuf), ImGuiInputTextFlags_EnterReturnsTrue);
                ImGui::PopItemWidth();
                ImGui::SameLine();
                if ((ImGui::SmallButton("Send##dm") || dmSend) && strlen(m_DMInputBuf) > 0) {
                    nlohmann::json dj; dj["to"] = m_ActiveDMUser; dj["msg"] = std::string(m_DMInputBuf);
                    m_NetClient.Send(PacketType::DM_Send, dj.dump());
                    memset(m_DMInputBuf, 0, sizeof(m_DMInputBuf));
                }
            }

            if (m_Friends.empty() || std::none_of(m_Friends.begin(), m_Friends.end(), [](const FriendEntry& f) { return f.status == "accepted"; })) {
                ImGui::Dummy(ImVec2(0, 20));
                ImGui::TextDisabled("No friends yet. Add someone by their username#tag above.");
            }

            // Call state banner
            if (!m_CurrentCall.state.empty() && m_CurrentCall.state != "ringing") {
                std::string cd = m_CurrentCall.otherUser;
                size_t chp = cd.find('#');
                if (chp != std::string::npos) cd = cd.substr(0, chp);
                ImGui::Dummy(ImVec2(0, 8));
                ImGui::Separator();
                if (m_CurrentCall.state == "calling")
                    ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1), "Calling %s...", cd.c_str());
                else if (m_CurrentCall.state == "active")
                    ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.4f, 1), "In call: %s", cd.c_str());
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.7f, 0.15f, 0.15f, 1));
                if (ImGui::Button("End Call", ImVec2(80, 26))) {
                    nlohmann::json rj; rj["to"] = m_CurrentCall.otherUser;
                    m_NetClient.Send(PacketType::Call_Reject, rj.dump());
                    m_CurrentCall = {};
                }
                ImGui::PopStyleColor();
            }

            ImGui::Unindent(36);
            ImGui::EndChild();
            ImGui::PopStyleColor();
        }
        else if (m_SelectedServerId != -1) {
            auto it = std::find_if(m_ServerList.begin(), m_ServerList.end(), [this](const Server& s) { return s.id == m_SelectedServerId; });
            if (it != m_ServerList.end()) {
                // Auto-redirect only when selection is invalid (no channel or wrong server); never overwrite valid voice selection
                bool currentValid = false;
                const Channel* firstTextCh = nullptr;
                for (const auto& ch : it->channels) {
                    if (ch.id == m_SelectedChannelId) currentValid = true;
                    if (ch.type == ChannelType::Text && !firstTextCh) firstTextCh = &ch;
                }
                if (!currentValid) {
                    if (firstTextCh) {
                        m_SelectedChannelId = firstTextCh->id;
                        m_NetClient.Send(PacketType::Select_Text_Channel, PacketHandler::SelectTextChannelPayload(firstTextCh->id));
                    }
                    else
                        m_SelectedChannelId = -1;
                }

                UI::Views::RenderChannelView(m_NetClient, m_CurrentUser, *it, m_Messages, m_SelectedChannelId, m_ActiveVoiceChannelId, m_VoiceMembers, m_SpeakingTimers, m_UserVolumes,
                    [this](const std::string& uid, float g) {
                        m_UserVolumes[uid] = g;
                        m_AudioEngine.SetUserGain(uid, g);
                        size_t h = uid.find('#');
                        if (h != std::string::npos)
                            m_AudioEngine.SetUserGain(uid.substr(0, h), g);
                        nlohmann::json j;
                        for (const auto& [k, v] : m_UserVolumes) j[k] = v;
                        std::ofstream of(ConfigManager::GetConfigDirectory() + "\\user_volumes.json");
                        if (of) of << j.dump();
                    }, m_ChatInputBuf, m_SelfMuted, m_SelfDeafened, &m_UserMuteStates,
                    &m_TypingUsers,
                    [this]() {
                        auto now = std::chrono::steady_clock::now();
                        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_LastTypingSentTime);
                        if (elapsed.count() >= 3000 && m_SelectedChannelId != -1) {
                            nlohmann::json tj; tj["cid"] = m_SelectedChannelId;
                            m_NetClient.Send(PacketType::Typing_Indicator, tj.dump());
                            m_LastTypingSentTime = now;
                        }
                    },
                    &m_ReplyingToMessageId,
                    [this]() -> const std::vector<std::pair<std::string, bool>>* {
                        static std::vector<std::pair<std::string, bool>> members;
                        members.clear();
                        for (const auto& sm : m_ServerMembers)
                            members.emplace_back(sm.username, sm.online);
                        return members.empty() ? nullptr : &members;
                    }(),
                    &m_ShowMemberList,
                    m_SearchBuf, &m_ShowSearch,
                    [this](int fps, int quality) {
                        m_ScreenShare.iAmSharing = true;
                        m_ScreenShare.fps = fps;
                        m_ScreenShare.quality = quality;
                        CaptureSettings cs;
                        cs.fps = fps;
                        cs.quality = quality;
                        m_ScreenCapture.Start(cs, [this](const std::vector<uint8_t>& bmpData, int w, int h) {
                            m_NetClient.SendRaw(PacketType::Screen_Share_Frame, bmpData);
                        });
                        nlohmann::json sj;
                        sj["width"] = 1920; sj["height"] = 1080; sj["fps"] = fps;
                        m_NetClient.Send(PacketType::Screen_Share_Start, sj.dump());
                    },
                    [this]() {
                        m_ScreenCapture.Stop();
                        m_ScreenShare.iAmSharing = false;
                        m_NetClient.Send(PacketType::Screen_Share_Stop, "{}");
                    },
                    m_ScreenShare.iAmSharing,
                    m_ScreenShare.someoneSharing,
                    (void*)TalkMe::TextureManager::Get().GetTexture("screenshare"),
                    m_ScreenShare.frameWidth,
                    m_ScreenShare.frameHeight);
            }
        }
    }

    void Application::Cleanup() {
        if (m_CleanedUp) return;
        m_CleanedUp = true;

        LOG_APP("Starting cleanup...");
        m_ShuttingDown.store(true);

        try {
            LOG_APP("Shutting down audio engine...");
            m_AudioEngine.Shutdown();
            LOG_APP("Audio engine shut down");
        }
        catch (const std::exception& e) {
            LOG_ERROR(std::string("AudioEngine::Shutdown() exception: ") + e.what());
        }

        if (m_UseUdpVoice) {
            try {
                LOG_APP("Stopping voice send pacer...");
                m_SendPacer.Stop();
                LOG_APP("Voice send pacer stopped");
            }
            catch (const std::exception& e) {
                LOG_ERROR(std::string("VoiceSendPacer::Stop() exception: ") + e.what());
            }
        }

        if (m_UseUdpVoice) {
            try {
                LOG_APP("Stopping voice transport...");
                m_VoiceTransport.Stop();
                LOG_APP("Voice transport stopped");
            }
            catch (const std::exception& e) {
                LOG_ERROR(std::string("VoiceTransport::Stop() exception: ") + e.what());
            }
        }

        try {
            LOG_APP("Disconnecting network client...");
            m_NetClient.Disconnect();
            LOG_APP("Network client disconnected");
        }
        catch (const std::exception& e) {
            LOG_ERROR(std::string("NetworkClient::Disconnect() exception: ") + e.what());
        }

        try {
            LOG_APP("Destroying overlay...");
            m_Overlay.Destroy();
            LOG_APP("Overlay destroyed");
        }
        catch (const std::exception& e) {
            LOG_ERROR(std::string("GameOverlay::Destroy() exception: ") + e.what());
        }

        // STEP 6: Close wake and quit events AFTER network is disconnected
        if (m_NetworkWakeEvent) {
            try {
                ::CloseHandle(static_cast<HANDLE>(m_NetworkWakeEvent));
                m_NetworkWakeEvent = nullptr;
            }
            catch (const std::exception& e) {
                LOG_ERROR(std::string("CloseHandle(NetworkWakeEvent) exception: ") + e.what());
            }
        }
        if (m_QuitEvent) {
            try {
                ::CloseHandle(static_cast<HANDLE>(m_QuitEvent));
                m_QuitEvent = nullptr;
            }
            catch (const std::exception&) { }
        }

        try {
            LOG_APP("Shutting down graphics (ImGui + D3D)...");
            m_Graphics.Shutdown();
            LOG_APP("Graphics shut down");
        }
        catch (const std::exception& e) {
            LOG_ERROR(std::string("AppGraphics::Shutdown() exception: ") + e.what());
        }

        try {
            LOG_APP("Shutting down logger...");
            Logger::Instance().Shutdown();
        }
        catch (const std::exception&) { }

        try {
            m_Window.Destroy();
        }
        catch (const std::exception&) { }
    }
}