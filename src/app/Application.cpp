#include "Application.h"
#include <tchar.h>
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
#pragma comment(lib, "winmm.lib")

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
#include <nlohmann/json.hpp>

#include "../ui/Theme.h"
#include "../ui/Styles.h"
#include "../ui/Components.h"
#include "../ui/views/LoginView.h"
#include "../ui/views/RegisterView.h"
#include "../ui/views/SidebarView.h"
#include "../ui/views/ChatView.h"
#include "../ui/views/SettingsView.h"
#include "../network/PacketHandler.h"
#include "../core/Logger.h"

using json = nlohmann::json;
static TalkMe::Application* g_AppInstance = nullptr;
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace TalkMe {

    void VoiceSendPacer::Start(SendFn fn) {
        m_SendFn = std::move(fn);
        m_Running = true;
        m_Thread = std::thread([this] {
            ::timeBeginPeriod(1);
            ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
            auto next = std::chrono::steady_clock::now();
            while (m_Running) {
                next += std::chrono::milliseconds(10); // one Opus frame
                std::this_thread::sleep_until(next);
                std::vector<uint8_t> pkt;
                {
                    std::lock_guard<std::mutex> lk(m_Mutex);
                    if (!m_Queue.empty()) {
                        pkt = std::move(m_Queue.front());
                        m_Queue.pop();
                    }
                }
                if (!pkt.empty() && m_SendFn)
                    m_SendFn(pkt);
            }
            ::timeEndPeriod(1);
        });
    }

    void VoiceSendPacer::Stop() {
        m_Running = false;
        if (m_Thread.joinable()) m_Thread.join();
    }

    void VoiceSendPacer::Enqueue(std::vector<uint8_t> payload) {
        static constexpr size_t kMaxQueue = 20;
        std::lock_guard<std::mutex> lk(m_Mutex);
        if (m_Queue.size() >= kMaxQueue)
            m_Queue.pop();
        m_Queue.push(std::move(payload));
    }

    static std::string GetExeDirectory() {
        wchar_t path[MAX_PATH] = {};
        if (::GetModuleFileNameW(nullptr, path, MAX_PATH) == 0) return {};
        std::wstring w(path);
        std::string s(w.begin(), w.end());
        size_t last = s.find_last_of("\\/");
        return (last != std::string::npos) ? s.substr(0, last) : s;
    }

    void HandleResize(HWND hWnd, UINT width, UINT height) {
        if (g_AppInstance) g_AppInstance->OnWindowResize(width, height);
    }

    LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) return true;
        switch (msg) {
        case WM_SIZE: if (wParam != SIZE_MINIMIZED) HandleResize(hWnd, (UINT)LOWORD(lParam), (UINT)HIWORD(lParam)); return 0;
        case WM_SYSCOMMAND: if ((wParam & 0xfff0) == SC_KEYMENU) return 0; break;
        case WM_DESTROY: ::PostQuitMessage(0); return 0;
        }
        return ::DefWindowProc(hWnd, msg, wParam, lParam);
    }

    std::string GetCurrentTimeStr() {
        auto t = std::time(nullptr); struct tm tm; localtime_s(&tm, &t);
        std::ostringstream oss; oss << std::put_time(&tm, "%H:%M"); return oss.str();
    }

    // Discord-like: very soft, low frequency, short — almost unnoticeable for competitive play
    static std::vector<uint8_t> BuildWavSoft(float freq, int durationMs, float volume) {
        const int sampleRate = 44100;
        const int numSamples = sampleRate * durationMs / 1000;
        const int bitsPerSample = 16;
        const int byteRate = sampleRate * bitsPerSample / 8;
        const int dataSize = numSamples * (bitsPerSample / 8);

        std::vector<uint8_t> wav(44 + dataSize);
        auto w16 = [&](size_t p, uint16_t v) { memcpy(&wav[p], &v, 2); };
        auto w32 = [&](size_t p, uint32_t v) { memcpy(&wav[p], &v, 4); };

        memcpy(&wav[0], "RIFF", 4);
        w32(4, 36 + dataSize);
        memcpy(&wav[8], "WAVE", 4);
        memcpy(&wav[12], "fmt ", 4);
        w32(16, 16);
        w16(20, 1);
        w16(22, 1);
        w32(24, sampleRate);
        w32(28, byteRate);
        w16(32, bitsPerSample / 8);
        w16(34, bitsPerSample);
        memcpy(&wav[36], "data", 4);
        w32(40, dataSize);

        int16_t* samples = (int16_t*)&wav[44];
        const float pi2 = 6.28318530f;
        float phase = 0.0f;
        const float fadeMs = 8.0f;
        const float fadeSamples = sampleRate * (fadeMs / 1000.0f);
        for (int i = 0; i < numSamples; i++) {
            float envelope = 1.0f;
            if ((float)i < fadeSamples)
                envelope = (float)i / fadeSamples;
            else if ((float)i > (float)numSamples - fadeSamples)
                envelope = (float)(numSamples - i) / fadeSamples;
            envelope *= envelope;

            phase += pi2 * freq / (float)sampleRate;
            if (phase > pi2) phase -= pi2;
            samples[i] = (int16_t)(std::sin(phase) * volume * envelope * 32767.0f);
        }
        return wav;
    }

    void Application::GenerateSounds() {
        m_JoinSound = BuildWavSoft(260.0f, 45, 0.055f);
        m_LeaveSound = BuildWavSoft(240.0f, 40, 0.05f);
    }

    void Application::PlayJoinSound() {
        if (!m_JoinSound.empty())
            PlaySoundA((LPCSTR)m_JoinSound.data(), NULL, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
    }

    void Application::PlayLeaveSound() {
        if (!m_LeaveSound.empty())
            PlaySoundA((LPCSTR)m_LeaveSound.data(), NULL, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
    }

    void Application::ToggleEchoLive() {
        if (!m_NetClient.IsConnected()) return;
        if (m_EchoLiveEnabled) { m_EchoLiveEnabled = false; return; }
        EnsureEchoLiveEnabled();
    }

    void Application::EnsureEchoLiveEnabled() {
        if (!m_NetClient.IsConnected() || m_EchoLiveEnabled) return;
        m_EchoLiveEnabled = true;
        m_EchoLiveHistory.clear();
        m_EchoLiveBucketStartTime = std::chrono::steady_clock::now();
        m_EchoLiveBucketStartSeq = m_EchoLiveNextSeq;
        m_EchoLiveRecvThisBucket = 0;
        for (int i = 0; i < kEchoLivePacketsPerSecond; i++) {
            std::vector<uint8_t> payload(4);
            uint32_t seq = m_EchoLiveNextSeq++;
            uint32_t netSeq = TalkMe::HostToNet32(seq);
            std::memcpy(payload.data(), &netSeq, 4);
            m_NetClient.SendRaw(PacketType::Echo_Request, payload);
        }
    }

    void Application::OnEchoResponse(uint32_t seq) {
        // Update echo-live bucket for TCP echo responses
        if (m_EchoLiveEnabled && seq >= m_EchoLiveBucketStartSeq && seq < m_EchoLiveBucketStartSeq + kEchoLivePacketsPerSecond)
            m_EchoLiveRecvThisBucket++;
    }

    // ---------------------------------------------------------------------------
    // Link probe: send kProbeCount small UDP packets, measure RTT/loss/jitter.
    // Runs on a background thread so the UI stays responsive.
    // Repeats every kRepeatIntervalSec seconds to adapt to changing conditions.
    // ---------------------------------------------------------------------------
    void Application::StartLinkProbe() {
        if (m_ProbeRunning.exchange(true)) return; // already running

        std::thread([this] {
            static constexpr int   kProbeCount = 120;   // 120 packets → ~1.2s probe
            static constexpr int   kProbeIntervalMs = 10;    // one per 10ms
            static constexpr int   kWaitForEchoMs = 600;   // wait 600ms for last echoes
            static constexpr int   kRepeatIntervalSec = 8;    // re-probe every 8s

            while (true) {
                // Build probe table: seq → send_time_us
                static constexpr uint8_t kProbeTag = 0xEE; // first byte marks this as a probe
                struct ProbeSent { uint32_t seq; int64_t sentUs; };
                std::vector<ProbeSent> sent;
                sent.reserve(kProbeCount);

                // Send phase
                ::timeBeginPeriod(1);
                for (int i = 0; i < kProbeCount; ++i) {
                    auto now_us = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count();
                    uint32_t seq = (uint32_t)i;

                    // Build probe packet: [tag:1][seq:4][timestamp_us:8] = 13 bytes (seq in network order)
                    std::vector<uint8_t> pkt(13);
                    pkt[0] = kProbeTag;
                    uint32_t netSeq = TalkMe::HostToNet32(seq);
                    std::memcpy(&pkt[1], &netSeq, 4);
                    std::memcpy(&pkt[5], &now_us, 8);
                    m_VoiceTransport.SendRaw(pkt); // bypass pacer — probe needs exact timing
                    sent.push_back({ seq, now_us });

                    std::this_thread::sleep_for(std::chrono::milliseconds(kProbeIntervalMs));
                }
                ::timeEndPeriod(1);

                // Wait for echoes
                std::this_thread::sleep_for(std::chrono::milliseconds(kWaitForEchoMs));

                // Collect results
                std::vector<float> rtts;
                {
                    std::lock_guard<std::mutex> lk(m_ProbeEchoMutex);
                    auto now_us = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count();
                    for (auto& [seq, sentUs] : sent) {
                        auto it = m_ProbeEchoTimes.find(seq);
                        if (it != m_ProbeEchoTimes.end()) {
                            float rttMs = (it->second - sentUs) / 1000.f;
                            if (rttMs > 0.f && rttMs < 2000.f)
                                rtts.push_back(rttMs);
                        }
                    }
                    m_ProbeEchoTimes.clear();
                }

                if (!rtts.empty()) {
                    // RTT (median to resist outliers)
                    std::sort(rtts.begin(), rtts.end());
                    float rttMs = rtts[rtts.size() / 2];

                    // Jitter = mean absolute deviation of RTT
                    float meanRtt = 0;
                    for (float r : rtts) meanRtt += r;
                    meanRtt /= rtts.size();
                    float jitterMs = 0;
                    for (float r : rtts) jitterMs += std::abs(r - meanRtt);
                    jitterMs /= rtts.size();

                    // Loss
                    float lossPct = 100.f * (1.f - (float)rtts.size() / kProbeCount);

                    // Log
                    std::ostringstream ss;
                    ss << std::fixed << std::setprecision(1);
                    ss << "step=link_probe samples=" << rtts.size()
                        << "/" << kProbeCount
                        << " rtt_ms=" << rttMs
                        << " jitter_ms=" << jitterMs
                        << " loss_pct=" << lossPct;
                    TalkMe::Logger::Instance().LogVoiceTrace(ss.str());

                    // Apply to AudioEngine — this updates adaptiveBufferLevel BEFORE
                    // the first voice packet arrives.
                    m_AudioEngine.ApplyProbeResults(rttMs, jitterMs, lossPct);
                }

                m_ProbeRunning = false;

                // Wait before re-probing
                std::this_thread::sleep_for(std::chrono::seconds(kRepeatIntervalSec));
                if (!m_UseUdpVoice || !m_NetClient.IsConnected()) break;
                if (m_ProbeRunning.exchange(true)) break; // someone else started a probe
            }
            m_ProbeRunning = false;
            }).detach();
    }

    void Application::HandleProbeEcho(const std::vector<uint8_t>& pkt) {
        // Server echoes probe packets unchanged: [tag:1][seq:4][original_timestamp_us:8]
        if (pkt.size() < 13) return;
        uint32_t seq;
        int64_t sentUs;
        std::memcpy(&seq, &pkt[1], 4);
        seq = TalkMe::NetToHost32(seq);
        std::memcpy(&sentUs, &pkt[5], 8);
        auto now_us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        std::lock_guard<std::mutex> lk(m_ProbeEchoMutex);
        m_ProbeEchoTimes[seq] = now_us; // use arrival time, not echo's internal timestamp
    }

    void Application::UpdateEchoLive() {
    if (!m_EchoLiveEnabled || !m_NetClient.IsConnected()) return;
    auto now = std::chrono::steady_clock::now();
    auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_EchoLiveBucketStartTime).count();
    if (elapsedMs < 1000) return;
    float lossPct = (kEchoLivePacketsPerSecond > 0)
        ? (100.f * (kEchoLivePacketsPerSecond - m_EchoLiveRecvThisBucket) / (float)kEchoLivePacketsPerSecond)
        : 0.f;
    m_EchoLiveHistory.push_back(lossPct);
    while (m_EchoLiveHistory.size() > kEchoLiveHistorySize) m_EchoLiveHistory.pop_front();
    m_EchoLiveBucketStartSeq = m_EchoLiveNextSeq;
    m_EchoLiveRecvThisBucket = 0;
    m_EchoLiveBucketStartTime = now;
    for (int i = 0; i < kEchoLivePacketsPerSecond; i++) {
        std::vector<uint8_t> payload(4);
        uint32_t seq = m_EchoLiveNextSeq++;
        uint32_t netSeq = TalkMe::HostToNet32(seq);
        std::memcpy(payload.data(), &netSeq, 4);
        m_NetClient.SendRaw(PacketType::Echo_Request, payload);
    }
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
        om.isMuted = (name == m_CurrentUser.username) && m_SelfMuted;
        om.isDeafened = (name == m_CurrentUser.username) && m_SelfDeafened;
        auto it = m_SpeakingTimers.find(name);
        om.isSpeaking = (it != m_SpeakingTimers.end() && (now - it->second) < 0.5f);
        members.push_back(om);
    }
    m_Overlay.UpdateMembers(members);
}

Application::Application(const std::string& title, int width, int height) : m_Title(title), m_Width(width), m_Height(height) { g_AppInstance = this; }
Application::~Application() { g_AppInstance = nullptr; Cleanup(); }

bool Application::Initialize() {
    // Add this near the beginning of your app initialization (in main() or early startup)
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
    if (TalkMe::Logger::Instance().InitializeVoiceTrace(tracePath, voiceTraceEnable))
        std::cerr << "Voice trace log: " << tracePath << "\n";
    if (!InitWindow() || !InitDirectX() || !InitImGui()) return false;

    m_AudioEngine.SetReceiverReportCallback([this](const TalkMe::ReceiverReportPayload& report) {
        if (m_CurrentState == AppState::MainApp && m_NetClient.IsConnected() && m_ActiveVoiceChannelId != -1) {
            TalkMe::ReceiverReportPayload out = report;
            out.ToNetwork();
            std::vector<uint8_t> payload(sizeof(TalkMe::ReceiverReportPayload));
            std::memcpy(payload.data(), &out, sizeof(TalkMe::ReceiverReportPayload));

            // Send control telemetry via reliable TCP to guarantee delivery
            m_NetClient.SendRaw(TalkMe::PacketType::Receiver_Report, payload);
        }
    });

    m_AudioEngine.InitializeWithSequence([this](const std::vector<uint8_t>& opusData, uint32_t seqNum) {
        if (m_CurrentState == AppState::MainApp && m_NetClient.IsConnected() && m_ActiveVoiceChannelId != -1) {
            // NOTE: No elapsedMs gate here.  Previously `if (elapsedMs < 18) return`
            // was silently dropping every other 10ms frame (proven by seq gaps of 2 in
            // trace logs).  Rate control is now handled by VoiceSendPacer which drains
            // the queue at exactly 10ms intervals on a TIME_CRITICAL thread.

            auto payload = PacketHandler::CreateVoicePayloadOpus(m_CurrentUser.username, opusData, seqNum);
            const char* pathStr = m_UseUdpVoice ? "udp" : "tcp";

            if (m_UseUdpVoice)
                m_SendPacer.Enqueue(payload);   // paced send — no more bursts
            else
                m_NetClient.SendRaw(PacketType::Voice_Data_Opus, payload);

            if (++m_VoiceTraceSendCount <= 5 || (m_VoiceTraceSendCount % 20) == 0)
                TalkMe::Logger::Instance().LogVoiceTrace(
                    "step=send path=" + std::string(pathStr) +
                    " seq=" + std::to_string(seqNum) +
                    " opus_bytes=" + std::to_string(opusData.size()) +
                    " payload_bytes=" + std::to_string(payload.size()) +
                    " handed_pct=100");
            // Under high loss, send the same frame again after 5ms to improve chance of delivery
            if (m_VoiceRedundancyEnabled) {
                m_PendingVoiceRedundant = true;
                m_LastVoiceRedundantTime = std::chrono::steady_clock::now();
                m_LastVoiceRedundantPayload = payload;
                m_LastVoiceRedundantOpus = opusData;
                m_LastVoiceRedundantTimestamp = seqNum * 960;
            }

            if (m_LocalEcho) {
                try {
                    auto parsed = PacketHandler::ParseVoicePayloadOpus(payload);
                    if (parsed.valid)
                        m_AudioEngine.PushIncomingAudioWithSequence(parsed.sender, parsed.opusData, parsed.sequenceNumber);
                }
                catch (...) {}
            }
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
            uint32_t n = ++m_VoiceTraceRecvCount;
            if (n <= 5 || (n % 20) == 0)
                TalkMe::Logger::Instance().LogVoiceTrace(
                    "step=recv path=tcp sender=" + parsed.sender +
                    " seq=" + std::to_string(parsed.sequenceNumber) +
                    " opus_bytes=" + std::to_string(parsed.opusData.size()) +
                    " recv_ok=1 quality_pct=100");
            pushVoiceIfNew(parsed.sender, parsed.opusData, parsed.sequenceNumber);
        }
        });

    // Start UDP voice transport so voice uses UDP (low latency). If Start fails, we fall back to TCP.
    m_VoiceTransport.SetReceiveCallback([this, pushVoiceIfNew](const std::vector<uint8_t>& pkt) {
        // Probe echo: first byte 0xEE and length 13 → route to HandleProbeEcho
        if (pkt.size() == 13 && pkt[0] == 0xEE) {
            HandleProbeEcho(pkt);
            return;
        }
        auto parsed = PacketHandler::ParseVoicePayloadOpus(pkt);
        if (parsed.valid) {
            uint32_t n = ++m_VoiceTraceRecvCount;
            if (n <= 5 || (n % 20) == 0)
                TalkMe::Logger::Instance().LogVoiceTrace(
                    "step=recv path=udp sender=" + parsed.sender +
                    " seq=" + std::to_string(parsed.sequenceNumber) +
                    " opus_bytes=" + std::to_string(parsed.opusData.size()) +
                    " recv_ok=1 quality_pct=100");
            pushVoiceIfNew(parsed.sender, parsed.opusData, parsed.sequenceNumber);
        }
        });
    if (m_VoiceTransport.Start(m_ServerIP, (uint16_t)VOICE_PORT)) {
        m_UseUdpVoice = true;
        m_LastUdpHelloTime = std::chrono::steady_clock::now();
        TalkMe::Logger::Instance().LogVoiceTrace("udp_start ok server=" + m_ServerIP + " port=" + std::to_string(VOICE_PORT));

        // Start the send pacer — drains queued audio at exactly 10ms on a
        // TIME_CRITICAL thread, replacing the broken elapsedMs<18 gate.
        m_SendPacer.Start([this](const std::vector<uint8_t>& pkt) {
            m_VoiceTransport.SendVoicePacket(pkt);
            });

        // Kick off the UDP link probe so we have real loss/jitter/RTT data
        // before the first voice packet arrives.
        StartLinkProbe();
    }
    else {
        TalkMe::Logger::Instance().LogVoiceTrace("udp_start fail server=" + m_ServerIP + " port=" + std::to_string(VOICE_PORT));
    }

    ConfigManager::Get().LoadKeybinds(m_KeyMuteMic, m_KeyDeafen);
    ConfigManager::Get().LoadOverlay(m_OverlayEnabled, m_OverlayCorner, m_OverlayOpacity);

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
        } catch (...) {}
    }

    GenerateSounds();

    m_Overlay.Create(GetModuleHandle(nullptr));
    m_Overlay.SetCorner(m_OverlayCorner);
    m_Overlay.SetOpacity(m_OverlayOpacity);
    m_Overlay.SetEnabled(m_OverlayEnabled);

    UserSession session = ConfigManager::Get().LoadSession();
    if (session.isLoggedIn) {
        m_CurrentUser = session; m_CurrentState = AppState::MainApp;
        strcpy_s(m_EmailBuf, sizeof(m_EmailBuf), session.email.c_str());
        strcpy_s(m_PasswordBuf, sizeof(m_PasswordBuf), session.password.c_str());
        auto email = session.email;
        auto pass = session.password;
        m_NetClient.ConnectAsync(m_ServerIP, m_ServerPort,
            [this, email, pass](bool success) {
                if (success)
                    m_NetClient.Send(PacketType::Login_Request,
                        PacketHandler::CreateLoginPayload(email, pass));
            });
    }
    return true;
}

void Application::Run() {
    bool done = false;
    while (!done) {
        MSG msg;
        while (::PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            ::TranslateMessage(&msg);
            ::DispatchMessage(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;

        if (m_ActiveVoiceChannelId != -1 && m_NetClient.IsConnected())
            EnsureEchoLiveEnabled();
        else if (m_ActiveVoiceChannelId == -1)
            m_EchoLiveEnabled = false;
        UpdateEchoLive();
        ProcessNetworkMessages();

        {
            std::lock_guard<std::mutex> lock(m_RecentSpeakersMutex);
            float t = (float)ImGui::GetTime();
            for (const auto& uid : m_RecentSpeakers) {
                m_SpeakingTimers[uid] = t;
                // Voice packets may use a short name (e.g. "Guest_0") while voiceMembers
                // uses the full discriminated name ("Guest_0#0019"). Resolve to full name.
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
        // Local speaking detection (suppressed when self-muted)
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
        }

        // Keybind polling (supports key combinations)
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

        m_ActiveVoiceChannelIdForVoice.store(m_ActiveVoiceChannelId, std::memory_order_relaxed);
        if (m_ActiveVoiceChannelId != -1) {
            auto tel = m_AudioEngine.GetTelemetry();
            m_VoiceRedundancyEnabled = (tel.packetLossPercentage > 40.0f);
            if (m_PendingVoiceRedundant) {
                auto now = std::chrono::steady_clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_LastVoiceRedundantTime).count();
                if (elapsed >= 5) {
                    if (!m_LastVoiceRedundantPayload.empty()) {
                        if (m_UseUdpVoice)
                            m_VoiceTransport.SendVoicePacket(m_LastVoiceRedundantPayload);
                        else
                            m_NetClient.SendRaw(PacketType::Voice_Data_Opus, m_LastVoiceRedundantPayload);
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
        // Periodically re-request voice state (interval from server config)
        if (m_ActiveVoiceChannelId != -1 && m_NetClient.IsConnected()) {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - m_LastVoiceStateRequestTime).count();
            if (elapsed >= m_VoiceConfig.voiceStateRequestIntervalSec) {
                m_NetClient.Send(PacketType::Join_Voice_Channel, PacketHandler::JoinVoiceChannelPayload(m_ActiveVoiceChannelId));
                m_LastVoiceStateRequestTime = now;
            }
            // Log voice stats to file every 15 seconds and send to server for control panel graphs
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
            // Detailed voice debug to console every 5 seconds (real data to fix connection/loss/ping)
            auto debugElapsed = std::chrono::duration_cast<std::chrono::seconds>(now - m_LastVoiceDebugLogTime).count();
            if (debugElapsed >= 5) {
                m_LastVoiceDebugLogTime = now;
                auto tel = m_AudioEngine.GetTelemetry();
                float pingMs = (m_UseUdpVoice && m_VoiceTransport.GetRttMs() > 0.0f)
                    ? m_VoiceTransport.GetRttMs() : tel.currentLatencyMs;
                std::string path = m_UseUdpVoice ? "UDP" : "TCP";
                std::ostringstream ss;
                ss << "Voice summary: path=" << path
                    << " channel=" << m_ActiveVoiceChannelId
                    << " members=" << (int)m_VoiceMembers.size()
                    << " ping=" << std::fixed << std::setprecision(1) << pingMs << "ms"
                    << " loss%=" << std::setprecision(1) << tel.packetLossPercentage
                    << " recv=" << tel.totalPacketsReceived << " lost=" << tel.totalPacketsLost
                    << " buffer_ms=" << tel.currentBufferMs
                    << " bitrate_kbps=" << tel.currentEncoderBitrateKbps;
                Logger::Instance().LogVoiceDebug(ss.str());
            }
        }
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
        UpdateOverlay();

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        RenderUI();
        ImGui::Render();

        ImVec4 bg = TalkMe::UI::Styles::BgMain();
        const float clear_color[4] = { bg.x, bg.y, bg.z, bg.w };
        m_pd3dDeviceContext->OMSetRenderTargets(1, &m_mainRenderTargetView, nullptr);
        m_pd3dDeviceContext->ClearRenderTargetView(m_mainRenderTargetView, clear_color);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        m_pSwapChain->Present(1, 0);
    }
}

void Application::ProcessNetworkMessages() {
    auto msgs = m_NetClient.FetchMessages();
    for (const auto& msg : msgs) {
        try {
            if (msg.type == PacketType::Echo_Response) {
                if (msg.data.size() >= 4) {
                    uint32_t seq = 0;
                    std::memcpy(&seq, msg.data.data(), 4);
                    seq = TalkMe::NetToHost32(seq);
                    OnEchoResponse(seq);
                }
                continue;
            }
            if (msg.type == PacketType::Register_Success) {
                auto j_res = nlohmann::json::parse(msg.data);
                std::string assigned = j_res.value("u", "");
                if (!assigned.empty()) {
                    m_CurrentUser.username = assigned;
                    m_CurrentUser.email = m_EmailBuf;
                    ConfigManager::Get().SaveSession(m_EmailBuf, m_PasswordBuf);
                }
                m_CurrentUser.isLoggedIn = true; m_CurrentState = AppState::MainApp; continue;
            }
            if (msg.type == PacketType::Register_Failed) {
                strcpy_s(m_StatusMessage, "Registration failed (Email already taken)."); continue;
            }
            if (msg.type == PacketType::Login_Success) {
                auto j_res = nlohmann::json::parse(msg.data);
                std::string assigned = j_res.value("u", "");
                if (!assigned.empty()) {
                    m_CurrentUser.username = assigned;
                    m_CurrentUser.email = m_EmailBuf;
                    ConfigManager::Get().SaveSession(m_EmailBuf, m_PasswordBuf);
                }
                m_CurrentUser.isLoggedIn = true; m_CurrentState = AppState::MainApp; continue;
            }
            if (msg.type == PacketType::Login_Failed) {
                strcpy_s(m_StatusMessage, sizeof(m_StatusMessage), "Invalid credentials."); continue;
            }
            if (msg.type == PacketType::Sender_Report) {
                if (msg.data.size() >= sizeof(TalkMe::SenderReportPayload)) {
                    TalkMe::SenderReportPayload sr;
                    std::memcpy(&sr, msg.data.data(), sizeof(sr));
                    sr.ToHost();

                    // Apply the dynamically calculated bitrate from the server
                    // -1 means "do not change this specific parameter"
                    m_AudioEngine.ApplyConfig(-1, -1, -1, -1, sr.suggestedBitrateKbps);
                }
                continue;
            }

            auto j = nlohmann::json::parse(msg.data);
            if (msg.type == PacketType::Voice_State_Update) {
                if (j["cid"] == m_ActiveVoiceChannelId && j.contains("action")) {
                    std::string targetUser = j.value("u", "");
                    std::string action = j.value("action", "");
                    if (action == "join" && !targetUser.empty()) {
                        if (std::find(m_VoiceMembers.begin(), m_VoiceMembers.end(), targetUser) == m_VoiceMembers.end()) {
                            m_VoiceMembers.push_back(targetUser);
                            if (targetUser != m_CurrentUser.username && m_JoinSoundPlayedFor.count(targetUser) == 0) {
                                m_JoinSoundPlayedFor.insert(targetUser);
                                PlayJoinSound();
                            }
                        }
                    }
                    else if (action == "leave" && !targetUser.empty()) {
                        auto it = std::find(m_VoiceMembers.begin(), m_VoiceMembers.end(), targetUser);
                        if (it != m_VoiceMembers.end()) {
                            m_VoiceMembers.erase(it);
                            m_JoinSoundPlayedFor.erase(targetUser);
                            m_AudioEngine.RemoveUserTrack(targetUser);
                            if (targetUser != m_CurrentUser.username) PlayLeaveSound();
                        }
                    }
                    m_AudioEngine.OnVoiceStateUpdate((int)m_VoiceMembers.size());
                    m_LastVoiceStateRequestTime = std::chrono::steady_clock::now();
                    UpdateOverlay();
                }
                else if (j["cid"] == m_ActiveVoiceChannelId && j.contains("members")) {
                    std::vector<std::string> oldMembers = m_VoiceMembers;
                    m_VoiceMembers.clear();
                    for (const auto& m : j["members"]) m_VoiceMembers.push_back(m);
                    m_AudioEngine.OnVoiceStateUpdate((int)m_VoiceMembers.size());
                    m_LastVoiceStateRequestTime = std::chrono::steady_clock::now();

                    for (const auto& nm : m_VoiceMembers) {
                        if (nm == m_CurrentUser.username) continue;
                        if (std::find(oldMembers.begin(), oldMembers.end(), nm) == oldMembers.end()) {
                            if (m_JoinSoundPlayedFor.count(nm) == 0) {
                                m_JoinSoundPlayedFor.insert(nm);
                                PlayJoinSound();
                            }
                            break;
                        }
                    }
                    for (const auto& om : oldMembers) {
                        if (om == m_CurrentUser.username) continue;
                        if (std::find(m_VoiceMembers.begin(), m_VoiceMembers.end(), om) == m_VoiceMembers.end()) {
                            m_JoinSoundPlayedFor.erase(om);
                            m_AudioEngine.RemoveUserTrack(om);
                            PlayLeaveSound();
                        }
                    }
                    UpdateOverlay();
                }
            }
            else if (msg.type == PacketType::Voice_Config) {
                m_VoiceConfig.keepaliveIntervalMs = j.value("keepalive_interval_ms", m_VoiceConfig.keepaliveIntervalMs);
                m_VoiceConfig.voiceStateRequestIntervalSec = j.value("voice_state_request_interval_sec", m_VoiceConfig.voiceStateRequestIntervalSec);
                m_VoiceConfig.jitterBufferTargetMs = j.value("jitter_buffer_target_ms", m_VoiceConfig.jitterBufferTargetMs);
                m_VoiceConfig.jitterBufferMinMs = j.value("jitter_buffer_min_ms", m_VoiceConfig.jitterBufferMinMs);
                m_VoiceConfig.jitterBufferMaxMs = j.value("jitter_buffer_max_ms", m_VoiceConfig.jitterBufferMaxMs);
                m_VoiceConfig.codecTargetKbps = j.value("codec_target_kbps", m_VoiceConfig.codecTargetKbps);
                m_VoiceConfig.preferUdp = j.value("prefer_udp", m_VoiceConfig.preferUdp);
                m_ServerVersion = j.value("server_version", m_ServerVersion);
                m_UseUdpVoice = m_VoiceConfig.preferUdp && m_VoiceTransport.IsRunning();
                m_AudioEngine.ApplyConfig(
                    m_VoiceConfig.jitterBufferTargetMs,
                    m_VoiceConfig.jitterBufferMinMs,
                    m_VoiceConfig.jitterBufferMaxMs,
                    m_VoiceConfig.keepaliveIntervalMs,
                    m_VoiceConfig.codecTargetKbps);
            }
            else if (msg.type == PacketType::Server_List_Response) {
                m_ServerList.clear();
                for (const auto& item : j) m_ServerList.push_back({ item["id"], item["name"], item["code"] });
                if (!m_ServerList.empty() && m_SelectedServerId == -1) {
                    m_SelectedServerId = m_ServerList[0].id;
                    m_NetClient.Send(PacketType::Get_Server_Content_Request, PacketHandler::GetServerContentPayload(m_SelectedServerId));
                }
            }
            else if (msg.type == PacketType::Server_Content_Response) {
                if (m_SelectedServerId != -1) {
                    auto it = std::find_if(m_ServerList.begin(), m_ServerList.end(), [this](const Server& s) { return s.id == m_SelectedServerId; });
                    if (it != m_ServerList.end()) {
                        it->channels.clear();
                        for (const auto& item : j) {
                            Channel ch; ch.id = item["id"]; ch.name = item["name"];
                            std::string typeStr = item.value("type", "text");
                            ch.type = (typeStr == "voice") ? ChannelType::Voice : ChannelType::Text;
                            it->channels.push_back(ch);
                        }
                    }
                }
            }
            else if (msg.type == PacketType::Message_History_Response) {
                if (!j.empty()) {
                    int cid = j[0]["cid"];
                    m_Messages.erase(std::remove_if(m_Messages.begin(), m_Messages.end(), [cid](const ChatMessage& m) { return m.channelId == cid; }), m_Messages.end());
                }
                for (const auto& item : j) m_Messages.push_back({ item.value("mid", 0), item["cid"], item["u"], item["msg"], item.value("time", "Old") });
            }
            else if (msg.type == PacketType::Message_Text) {
                m_Messages.push_back({ j.value("mid", 0), j.value("cid", 0), j.value("u", "??"), j.value("msg", ""), GetCurrentTimeStr() });
            }
        }
        catch (...) {}
    }
}

void Application::RenderUI() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport(); ImGui::SetNextWindowPos(viewport->WorkPos); ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f); ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f); ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar;
    ImGui::Begin("MainShell", nullptr, flags);
    if (m_CurrentState == AppState::Login) RenderLogin(); else if (m_CurrentState == AppState::Register) RenderRegister(); else if (m_CurrentState == AppState::MainApp) RenderMainApp();
    ImGui::End(); ImGui::PopStyleVar(3);
}

void Application::RenderLogin() { UI::Views::RenderLogin(m_NetClient, m_CurrentState, m_EmailBuf, m_PasswordBuf, m_StatusMessage, m_ServerIP, m_ServerPort); }
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
    UI::Views::RenderSidebar(m_NetClient, m_CurrentUser, m_CurrentState, m_ServerList, m_SelectedServerId, m_SelectedChannelId, m_ActiveVoiceChannelId, m_VoiceMembers, m_NewServerNameBuf, m_NewChannelNameBuf, m_ShowSettings, m_SelfMuted, m_SelfDeafened, vi, nullptr, [this]() { EnsureEchoLiveEnabled(); });

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
            }
        };
        UI::Views::RenderSettings(sctx);
    }
    else if (m_SelectedServerId != -1) {
        auto it = std::find_if(m_ServerList.begin(), m_ServerList.end(), [this](const Server& s) { return s.id == m_SelectedServerId; });
        if (it != m_ServerList.end()) {
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
                }, m_ChatInputBuf, m_SelfMuted, m_SelfDeafened);
        }
    }
}

bool Application::InitWindow() { WNDCLASSEX wc = { sizeof(WNDCLASSEX), CS_CLASSDC, WndProc, 0L, 0L, GetModuleHandle(nullptr), nullptr, nullptr, nullptr, nullptr, _T("TalkMeClass"), nullptr }; ::RegisterClassEx(&wc); m_Hwnd = ::CreateWindow(wc.lpszClassName, _T("TalkMe"), WS_OVERLAPPEDWINDOW, 0, 0, m_Width, m_Height, nullptr, nullptr, wc.hInstance, nullptr); if (!m_Hwnd) return false; ::ShowWindow(m_Hwnd, SW_MAXIMIZE); ::UpdateWindow(m_Hwnd); return true; }
bool Application::InitDirectX() {
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2; sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.OutputWindow = m_Hwnd; sd.SampleDesc.Count = 1; sd.Windowed = TRUE; sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    D3D_FEATURE_LEVEL fl;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd, &m_pSwapChain, &m_pd3dDevice, &fl, &m_pd3dDeviceContext);
    if (FAILED(hr)) return false;
    CreateRenderTarget();
    RECT r = {};
    if (::GetClientRect(m_Hwnd, &r)) { m_LastResizeWidth = (UINT)(r.right - r.left); m_LastResizeHeight = (UINT)(r.bottom - r.top); }
    return true;
}

void Application::CreateRenderTarget() {
    if (!m_pSwapChain || !m_pd3dDevice) return;
    ID3D11Texture2D* pBackBuffer = nullptr;
    HRESULT hr = m_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    if (FAILED(hr) || !pBackBuffer) return;
    hr = m_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &m_mainRenderTargetView);
    pBackBuffer->Release();
    if (FAILED(hr)) {
        if (m_mainRenderTargetView) { m_mainRenderTargetView->Release(); m_mainRenderTargetView = nullptr; }
    }
}
void Application::CleanupRenderTarget() { if (m_mainRenderTargetView) { m_mainRenderTargetView->Release(); m_mainRenderTargetView = nullptr; } }

void Application::OnWindowResize(UINT width, UINT height) {
    if (width == 0 || height == 0) return;
    if (width == m_LastResizeWidth && height == m_LastResizeHeight) return;
    m_LastResizeWidth = width;
    m_LastResizeHeight = height;
    if (!m_pSwapChain || !m_pd3dDevice) return;
    CleanupRenderTarget();
    m_pSwapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
    CreateRenderTarget();
}
bool Application::InitImGui() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    std::string configDir = ConfigManager::GetConfigDirectory();
    CreateDirectoryA(configDir.c_str(), NULL);
    static std::string s_imguiIniPath;
    s_imguiIniPath = configDir + "\\imgui.ini";
    io.IniFilename = s_imguiIniPath.c_str();
    ImFont* font = io.Fonts->AddFontFromFileTTF("c:\\Windows\\Fonts\\segoeui.ttf", 20.0f);
    if (!font) io.Fonts->AddFontDefault();
    UI::ApplyAppStyle();
    ImGui_ImplWin32_Init(m_Hwnd);
    ImGui_ImplDX11_Init(m_pd3dDevice, m_pd3dDeviceContext);
    return true;
}
void Application::Cleanup() {
    m_Overlay.Destroy();
    if (m_UseUdpVoice) {
        m_SendPacer.Stop();
        m_VoiceTransport.Stop();
    }
    m_AudioEngine.Shutdown();
    ImGui_ImplDX11_Shutdown(); ImGui_ImplWin32_Shutdown(); ImGui::DestroyContext(); CleanupRenderTarget();
    if (m_pSwapChain) m_pSwapChain->Release(); if (m_pd3dDeviceContext) m_pd3dDeviceContext->Release(); if (m_pd3dDevice) m_pd3dDevice->Release(); ::DestroyWindow(m_Hwnd); ::UnregisterClass(_T("TalkMeClass"), GetModuleHandle(nullptr));
}
}