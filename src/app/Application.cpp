#include "Application.h"
#include <tchar.h>
#include <cstring>
#include <ctime>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <algorithm>
#include <windows.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")

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

using json = nlohmann::json;
static TalkMe::Application* g_AppInstance = nullptr;
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace TalkMe {

    void HandleResize(HWND hWnd, UINT width, UINT height) {
        if (g_AppInstance && g_AppInstance->GetDevice() && g_AppInstance->GetSwapChain()) {
            g_AppInstance->CleanupRenderTarget();
            g_AppInstance->GetSwapChain()->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
            g_AppInstance->CreateRenderTarget();
        }
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

    static std::vector<uint8_t> BuildWav(float freqStart, float freqEnd, int durationMs, float volume = 0.25f) {
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
        for (int i = 0; i < numSamples; i++) {
            float t = (float)i / (float)numSamples;
            float freq = freqStart + (freqEnd - freqStart) * t;
            float envelope = 1.0f;
            float fadeSamples = sampleRate * 0.01f;
            if ((float)i < fadeSamples) envelope = (float)i / fadeSamples;
            if ((float)i > (float)numSamples - fadeSamples) envelope = (float)(numSamples - i) / fadeSamples;

            phase += pi2 * freq / (float)sampleRate;
            if (phase > pi2) phase -= pi2;
            samples[i] = (int16_t)(std::sin(phase) * volume * envelope * 32767.0f);
        }
        return wav;
    }

    void Application::GenerateSounds() {
        m_JoinSound = BuildWav(600.0f, 900.0f, 120, 0.2f);
        m_LeaveSound = BuildWav(700.0f, 400.0f, 150, 0.2f);
    }

    void Application::PlayJoinSound() {
        if (!m_JoinSound.empty())
            PlaySoundA((LPCSTR)m_JoinSound.data(), NULL, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
    }

    void Application::PlayLeaveSound() {
        if (!m_LeaveSound.empty())
            PlaySoundA((LPCSTR)m_LeaveSound.data(), NULL, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
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
        if (!InitWindow() || !InitDirectX() || !InitImGui()) return false;

        m_AudioEngine.InitializeWithSequence([this](const std::vector<uint8_t>& opusData, uint32_t seqNum) {
            if (m_CurrentState == AppState::MainApp && m_NetClient.IsConnected() && m_ActiveVoiceChannelId != -1) {
                auto payload = PacketHandler::CreateVoicePayloadOpus(m_CurrentUser.username, opusData, seqNum);
                if (m_UseUdpVoice) {
                    m_VoiceTransport.SendVoicePacket(payload);
                } else {
                    m_NetClient.SendRaw(PacketType::Voice_Data_Opus, payload);
                }

                if (m_LocalEcho) {
                    try {
                        auto parsed = PacketHandler::ParseVoicePayloadOpus(payload);
                        if (parsed.valid)
                            m_AudioEngine.PushIncomingAudioWithSequence(parsed.sender, parsed.opusData, parsed.sequenceNumber);
                    } catch (...) {}
                }
            }
        });

        m_NetClient.SetVoiceCallback([this](const std::vector<uint8_t>& packetData) {
            if (m_ActiveVoiceChannelIdForVoice.load(std::memory_order_relaxed) == -1) return;
            auto parsed = PacketHandler::ParseVoicePayloadOpus(packetData);
            if (parsed.valid) {
                m_AudioEngine.PushIncomingAudioWithSequence(parsed.sender, parsed.opusData, parsed.sequenceNumber);
                std::lock_guard<std::mutex> lock(m_RecentSpeakersMutex);
                m_RecentSpeakers.push_back(parsed.sender);
            }
        });

        // Start UDP voice transport (best-effort)
        if (m_UseUdpVoice) {
            m_VoiceTransport.SetReceiveCallback([this](const std::vector<uint8_t>& pkt) {
                if (m_ActiveVoiceChannelIdForVoice.load(std::memory_order_relaxed) == -1) return;
                auto parsed = PacketHandler::ParseVoicePayloadOpus(pkt);
                if (parsed.valid) {
                    m_AudioEngine.PushIncomingAudioWithSequence(parsed.sender, parsed.opusData, parsed.sequenceNumber);
                    std::lock_guard<std::mutex> lock(m_RecentSpeakersMutex);
                    m_RecentSpeakers.push_back(parsed.sender);
                }
            });
            m_VoiceTransport.Start(m_ServerIP, (uint16_t)VOICE_PORT);
        }

        ConfigManager::Get().LoadKeybinds(m_KeyMuteMic, m_KeyDeafen);
        ConfigManager::Get().LoadOverlay(m_OverlayEnabled, m_OverlayCorner, m_OverlayOpacity);
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
            if (m_ActiveVoiceChannelId != m_PrevActiveVoiceChannelId) {
                m_AudioEngine.ClearRemoteTracks();
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
                // Log voice stats to file every 15 seconds
                auto statsElapsed = std::chrono::duration_cast<std::chrono::seconds>(now - m_LastVoiceStatsLogTime).count();
                if (statsElapsed >= 15) {
                    m_LastVoiceStatsLogTime = now;
                    auto tel = m_AudioEngine.GetTelemetry();
                    Logger::Instance().LogVoiceStats(m_ActiveVoiceChannelId, m_VoiceMembers,
                        tel.totalPacketsReceived, tel.totalPacketsLost, tel.packetLossPercentage,
                        tel.avgJitterMs, tel.currentBufferMs, tel.currentEncoderBitrateKbps);
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

                auto j = nlohmann::json::parse(msg.data);
                if (msg.type == PacketType::Voice_State_Update) {
                    if (j["cid"] == m_ActiveVoiceChannelId) {
                        std::vector<std::string> oldMembers = m_VoiceMembers;
                        m_VoiceMembers.clear();
                        for (const auto& m : j["members"]) m_VoiceMembers.push_back(m);
                        m_AudioEngine.OnVoiceStateUpdate((int)m_VoiceMembers.size());
                        m_LastVoiceStateRequestTime = std::chrono::steady_clock::now();

                        for (const auto& nm : m_VoiceMembers) {
                            if (nm == m_CurrentUser.username) continue;
                            if (std::find(oldMembers.begin(), oldMembers.end(), nm) == oldMembers.end()) {
                                PlayJoinSound();
                                break;
                            }
                        }
                        for (const auto& om : oldMembers) {
                            if (om == m_CurrentUser.username) continue;
                            if (std::find(m_VoiceMembers.begin(), m_VoiceMembers.end(), om) == m_VoiceMembers.end()) {
                                PlayLeaveSound();
                                break;
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
                    m_AudioEngine.ApplyConfig(m_VoiceConfig.jitterBufferTargetMs, m_VoiceConfig.jitterBufferMinMs, m_VoiceConfig.jitterBufferMaxMs, m_VoiceConfig.keepaliveIntervalMs);
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
        if (m_ActiveVoiceChannelId != -1) {
            auto tel = m_AudioEngine.GetTelemetry();
            vi.avgPingMs = tel.currentLatencyMs;
            vi.packetLossPercent = tel.packetLossPercentage;
            vi.packetsReceived = tel.totalPacketsReceived;
            vi.packetsLost = tel.totalPacketsLost;
            vi.currentBufferMs = tel.currentBufferMs;
            vi.encoderBitrateKbps = tel.currentEncoderBitrateKbps;
        }
        UI::Views::RenderSidebar(m_NetClient, m_CurrentUser, m_CurrentState, m_ServerList, m_SelectedServerId, m_SelectedChannelId, m_ActiveVoiceChannelId, m_VoiceMembers, m_NewServerNameBuf, m_NewChannelNameBuf, m_ShowSettings, m_SelfMuted, m_SelfDeafened, vi);

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
        } else if (m_SelectedServerId != -1) {
            auto it = std::find_if(m_ServerList.begin(), m_ServerList.end(), [this](const Server& s) { return s.id == m_SelectedServerId; });
            if (it != m_ServerList.end()) {
                UI::Views::RenderChannelView(m_NetClient, m_CurrentUser, *it, m_Messages, m_SelectedChannelId, m_ActiveVoiceChannelId, m_VoiceMembers, m_SpeakingTimers, m_UserVolumes,
                    [this](const std::string& uid, float g) {
                        m_UserVolumes[uid] = g;
                        m_AudioEngine.SetUserGain(uid, g);
                        size_t h = uid.find('#');
                        if (h != std::string::npos)
                            m_AudioEngine.SetUserGain(uid.substr(0, h), g);
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
        if (m_UseUdpVoice) m_VoiceTransport.Stop();
        m_AudioEngine.Shutdown();
        ImGui_ImplDX11_Shutdown(); ImGui_ImplWin32_Shutdown(); ImGui::DestroyContext(); CleanupRenderTarget();
        if (m_pSwapChain) m_pSwapChain->Release(); if (m_pd3dDeviceContext) m_pd3dDeviceContext->Release(); if (m_pd3dDevice) m_pd3dDevice->Release(); ::DestroyWindow(m_Hwnd); ::UnregisterClass(_T("TalkMeClass"), GetModuleHandle(nullptr));
    }
}