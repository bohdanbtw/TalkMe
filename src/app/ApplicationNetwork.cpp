// Network message dispatch: ProcessNetworkMessages implementation.
// Keeps the packet switch and state updates out of Application.cpp.
#include "Application.h"
#include "../shared/Protocol.h"
#include "../shared/PacketHandler.h"
#include "../core/ConfigManager.h"
#include <nlohmann/json.hpp>
#include <cstring>
#include <algorithm>
#include <chrono>
#include <format>

using json = nlohmann::json;

namespace TalkMe {

namespace {

static std::string GetCurrentTimeStr() {
    // std::format with zoned_time gives the local wall-clock HH:MM with zero allocation overhead.
    return std::format("{:%H:%M}",
        std::chrono::zoned_time{ std::chrono::current_zone(),
                                 std::chrono::system_clock::now() });
}

} // namespace

void Application::ProcessNetworkMessages() {
    auto msgs = m_NetClient.FetchMessages();
    for (const auto& msg : msgs) {
        try {
            // ── Typed packets that do not need JSON parsing ────────────────
            if (msg.type == PacketType::Echo_Response) {
                if (msg.data.size() >= 4) {
                    uint32_t seq = 0;
                    std::memcpy(&seq, msg.data.data(), 4);
                    seq = TalkMe::NetToHost32(seq);
                    OnEchoResponse(seq);
                }
                continue;
            }

            if (msg.type == PacketType::Sender_Report) {
                if (msg.data.size() >= sizeof(TalkMe::SenderReportPayload)) {
                    TalkMe::SenderReportPayload sr;
                    std::memcpy(&sr, msg.data.data(), sizeof(sr));
                    sr.ToHost();
                    m_AudioEngine.ApplyConfig(-1, -1, -1, -1, sr.suggestedBitrateKbps);
                }
                continue;
            }

            // ── All remaining packets require a JSON body ──────────────────
            if (msg.data.empty()) continue;

            // Fast pre-validation: skip the throw path for clearly non-JSON data.
            if (!json::accept(msg.data.begin(), msg.data.end())) continue;

            const json j = json::parse(msg.data);

            if (msg.type == PacketType::Register_Success) {
                const std::string assigned = j.value("u", "");
                if (!assigned.empty()) {
                    m_CurrentUser.username = assigned;
                    m_CurrentUser.email    = m_EmailBuf;
                    ConfigManager::Get().SaveSession(m_EmailBuf, m_PasswordBuf);
                }
                m_CurrentUser.isLoggedIn = true;
                m_CurrentState           = AppState::MainApp;
                continue;
            }

            if (msg.type == PacketType::Register_Failed) {
                strcpy_s(m_StatusMessage, "Registration failed (email already taken).");
                continue;
            }

            if (msg.type == PacketType::Login_Requires_2FA) {
                m_ValidatingSession  = false;
                m_CurrentState       = AppState::Login2FA;
                m_2FACodeBuf[0]      = '\0';
                m_StatusMessage[0]   = '\0';
                continue;
            }

            if (msg.type == PacketType::Login_Success) {
                m_ValidatingSession = false;
                const std::string assigned = j.value("u", "");
                if (!assigned.empty()) {
                    m_CurrentUser.username = assigned;
                    m_CurrentUser.email    = m_EmailBuf;
                    ConfigManager::Get().SaveSession(m_EmailBuf, m_PasswordBuf);
                }
                m_Is2FAEnabled           = j.value("2fa_enabled", false);
                m_CurrentUser.isLoggedIn = true;
                m_CurrentState           = AppState::MainApp;
                m_StatusMessage[0]       = '\0';
                continue;
            }

            if (msg.type == PacketType::Login_Failed) {
                if (m_ValidatingSession) {
                    ConfigManager::Get().ClearSession();
                    strcpy_s(m_StatusMessage, sizeof(m_StatusMessage),
                             "Session expired. Please sign in again.");
                    m_ValidatingSession = false;
                } else {
                    strcpy_s(m_StatusMessage, sizeof(m_StatusMessage),
                             "Invalid credentials.");
                }
                m_CurrentState = AppState::Login;
                continue;
            }

            if (msg.type == PacketType::Generate_2FA_Secret_Response) {
                m_2FASecretStr         = j.value("secret", "");
                m_2FAUriStr            = j.value("uri", "");
                m_IsSettingUp2FA       = true;
                m_2FASetupStatusMessage[0] = '\0';
                continue;
            }

            if (msg.type == PacketType::Verify_2FA_Setup_Request) {
                const bool ok = j.value("ok", j.value("success", false));
                if (ok) {
                    m_Is2FAEnabled    = true;
                    m_IsSettingUp2FA  = false;
                    m_2FASecretStr.clear();
                    m_2FAUriStr.clear();
                    m_2FASetupCodeBuf[0] = '\0';
                    strcpy_s(m_2FASetupStatusMessage, "2FA enabled successfully.");
                } else {
                    strcpy_s(m_2FASetupStatusMessage, "Invalid 2FA code. Try again.");
                }
                continue;
            }

            if (msg.type == PacketType::Disable_2FA_Response) {
                if (j.value("ok", false)) {
                    m_Is2FAEnabled       = false;
                    m_IsDisabling2FA     = false;
                    m_Disable2FACodeBuf[0] = '\0';
                }
                continue;
            }

            // ── Implicit 2FA challenge: {"u":"..."} arriving in Login state ─
            // This path is reached only when the packet type did not match any
            // of the explicit handlers above, so there is no double-parse.
            if (m_CurrentState == AppState::Login
                && j.is_object() && j.size() == 1 && j.contains("u")) {
                m_ValidatingSession = false;
                m_CurrentState      = AppState::Login2FA;
                m_2FACodeBuf[0]     = '\0';
                m_StatusMessage[0]  = '\0';
                continue;
            }

            // ── Packets requiring an active session ────────────────────────
            if (msg.type == PacketType::Voice_Config) {
                m_VoiceConfig.keepaliveIntervalMs          = j.value("keepalive_interval_ms",           m_VoiceConfig.keepaliveIntervalMs);
                m_VoiceConfig.voiceStateRequestIntervalSec = j.value("voice_state_request_interval_sec", m_VoiceConfig.voiceStateRequestIntervalSec);
                m_VoiceConfig.jitterBufferTargetMs         = j.value("jitter_buffer_target_ms",          m_VoiceConfig.jitterBufferTargetMs);
                m_VoiceConfig.jitterBufferMinMs            = j.value("jitter_buffer_min_ms",             m_VoiceConfig.jitterBufferMinMs);
                m_VoiceConfig.jitterBufferMaxMs            = j.value("jitter_buffer_max_ms",             m_VoiceConfig.jitterBufferMaxMs);
                m_VoiceConfig.codecTargetKbps              = j.value("codec_target_kbps",                m_VoiceConfig.codecTargetKbps);
                m_VoiceConfig.preferUdp                    = j.value("prefer_udp",                       m_VoiceConfig.preferUdp);
                m_ServerVersion = j.value("server_version", m_ServerVersion);
                m_UseUdpVoice   = m_VoiceConfig.preferUdp && m_VoiceTransport.IsRunning();
                m_AudioEngine.ApplyConfig(
                    m_VoiceConfig.jitterBufferTargetMs,
                    m_VoiceConfig.jitterBufferMinMs,
                    m_VoiceConfig.jitterBufferMaxMs,
                    m_VoiceConfig.keepaliveIntervalMs,
                    m_VoiceConfig.codecTargetKbps);
                continue;
            }

            if (msg.type == PacketType::Voice_State_Update) {
                const int  updateCid      = j.value("cid", -1);
                const bool forCurrentCh   = (updateCid == m_ActiveVoiceChannelId);
                const bool isSelfLeave    = (j.value("action", "") == "leave"
                                             && j.value("u", "") == m_CurrentUser.username);

                if ((forCurrentCh || isSelfLeave) && j.contains("action")) {
                    const std::string targetUser = j.value("u", "");
                    const std::string action     = j.value("action", "");
                    if (action == "join" && !targetUser.empty() && forCurrentCh) {
                        if (std::find(m_VoiceMembers.begin(), m_VoiceMembers.end(), targetUser)
                            == m_VoiceMembers.end()) {
                            m_VoiceMembers.push_back(targetUser);
                            if (targetUser != m_CurrentUser.username
                                && m_JoinSoundPlayedFor.count(targetUser) == 0) {
                                m_JoinSoundPlayedFor.insert(targetUser);
                                PlayJoinSound();
                            }
                        }
                    } else if (action == "leave" && !targetUser.empty()) {
                        auto it = std::find(m_VoiceMembers.begin(), m_VoiceMembers.end(), targetUser);
                        if (it != m_VoiceMembers.end()) {
                            m_VoiceMembers.erase(it);
                            m_JoinSoundPlayedFor.erase(targetUser);
                            m_AudioEngine.RemoveUserTrack(targetUser);
                            if (targetUser != m_CurrentUser.username) PlayLeaveSound();
                        }
                    }
                    m_AudioEngine.OnVoiceStateUpdate(static_cast<int>(m_VoiceMembers.size()));
                    m_LastVoiceStateRequestTime = std::chrono::steady_clock::now();
                    UpdateOverlay();
                } else if (updateCid == m_ActiveVoiceChannelId && j.contains("members")) {
                    const std::vector<std::string> oldMembers = m_VoiceMembers;
                    m_VoiceMembers.clear();
                    for (const auto& m : j["members"]) m_VoiceMembers.push_back(m);
                    m_AudioEngine.OnVoiceStateUpdate(static_cast<int>(m_VoiceMembers.size()));
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
                        if (std::find(m_VoiceMembers.begin(), m_VoiceMembers.end(), om)
                            == m_VoiceMembers.end()) {
                            m_JoinSoundPlayedFor.erase(om);
                            m_AudioEngine.RemoveUserTrack(om);
                            PlayLeaveSound();
                        }
                    }
                    UpdateOverlay();
                }
                continue;
            }

            if (msg.type == PacketType::Server_List_Response) {
                m_ServerList.clear();
                for (const auto& item : j)
                    m_ServerList.push_back({ item["id"], item["name"], item["code"] });
                if (!m_ServerList.empty() && m_SelectedServerId == -1) {
                    m_SelectedServerId = m_ServerList[0].id;
                    m_NetClient.Send(PacketType::Get_Server_Content_Request,
                                     PacketHandler::GetServerContentPayload(m_SelectedServerId));
                }
                continue;
            }

            if (msg.type == PacketType::Server_Content_Response) {
                if (m_SelectedServerId != -1) {
                    auto it = std::find_if(m_ServerList.begin(), m_ServerList.end(),
                        [this](const Server& s) { return s.id == m_SelectedServerId; });
                    if (it != m_ServerList.end()) {
                        it->channels.clear();
                        for (const auto& item : j) {
                            Channel ch;
                            ch.id   = item["id"];
                            ch.name = item["name"];
                            const std::string typeStr = item.value("type", "text");
                            ch.type = (typeStr == "voice") ? ChannelType::Voice : ChannelType::Text;
                            it->channels.push_back(ch);
                        }
                    }
                }
                continue;
            }

            if (msg.type == PacketType::Message_History_Response) {
                if (!j.empty()) {
                    const int cid = j[0]["cid"];
                    m_Messages.erase(
                        std::remove_if(m_Messages.begin(), m_Messages.end(),
                                       [cid](const ChatMessage& m) { return m.channelId == cid; }),
                        m_Messages.end());
                    for (const auto& item : j)
                        if (item.contains("mid"))
                            m_Messages.push_back({ item.value("mid", 0), item["cid"],
                                                   item.value("u", ""), item.value("msg", ""),
                                                   item.value("time", "Old") });
                }
                continue;
            }

            if (msg.type == PacketType::Message_Edit) {
                const int mid = j.value("mid", 0);
                const std::string newMsg = j.value("msg", "");
                for (auto& m : m_Messages) {
                    if (m.id == mid) { m.content = newMsg; break; }
                }
                continue;
            }

            if (msg.type == PacketType::Message_Delete) {
                const int mid = j.value("mid", 0);
                m_Messages.erase(
                    std::remove_if(m_Messages.begin(), m_Messages.end(),
                                   [mid](const ChatMessage& m) { return m.id == mid; }),
                    m_Messages.end());
                continue;
            }

            if (msg.type == PacketType::Message_Text) {
                m_Messages.push_back({ j.value("mid", 0), j.value("cid", 0),
                                       j.value("u", "??"), j.value("msg", ""),
                                       GetCurrentTimeStr() });
                continue;
            }
        }
        catch (...) {}
    }
}

} // namespace TalkMe