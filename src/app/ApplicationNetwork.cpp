// Network message dispatch: ProcessNetworkMessages implementation.
// Keeps the packet switch and state updates out of Application.cpp.
#include "Application.h"
#include "../shared/Protocol.h"
#include "../shared/PacketHandler.h"
#include "../core/ConfigManager.h"
#include "../../vendor/imgui.h"
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

            // ── Presence packets ─────────────────────────────────────────
            if (msg.type == PacketType::Voice_Mute_State) {
                const std::string user = j.value("u", "");
                if (!user.empty()) {
                    m_UserMuteStates[user] = { j.value("muted", false), j.value("deafened", false) };
                    UpdateOverlay();
                }
                continue;
            }

            if (msg.type == PacketType::Typing_Indicator) {
                const std::string user = j.value("u", "");
                if (!user.empty() && user != m_CurrentUser.username)
                    m_TypingUsers[user] = (float)ImGui::GetTime();
                continue;
            }

            if (msg.type == PacketType::Member_List_Response) {
                m_ServerMembers.clear();
                for (const auto& item : j) {
                    ServerMember sm;
                    sm.username = item.value("u", "");
                    sm.online = item.value("online", false);
                    if (!sm.username.empty()) m_ServerMembers.push_back(sm);
                }
                std::sort(m_ServerMembers.begin(), m_ServerMembers.end(),
                    [](const ServerMember& a, const ServerMember& b) {
                        if (a.online != b.online) return a.online > b.online;
                        return a.username < b.username;
                    });
                continue;
            }

            if (msg.type == PacketType::Game_Challenge) {
                m_ChessUI.opponent = j.value("from", "");
                m_ChessUI.active = false;
                continue;
            }

            if (msg.type == PacketType::Game_State) {
                if (j.contains("action") && j["action"] == "accept") {
                    m_ChessEngine.Reset();
                    m_ChessUI.active = true;
                    m_ChessUI.isWhite = (j.value("from", "") != m_CurrentUser.username);
                    m_ChessUI.myTurn = m_ChessUI.isWhite;
                    m_ChessUI.selectedRow = -1;
                    m_ChessUI.selectedCol = -1;
                }
                continue;
            }

            if (msg.type == PacketType::Game_Move) {
                if (m_ChessUI.active && j.contains("fr") && j.contains("fc") && j.contains("tr") && j.contains("tc")) {
                    int fr = j["fr"], fc = j["fc"], tr = j["tr"], tc = j["tc"];
                    m_ChessEngine.MakeMove(fr, fc, tr, tc);
                    m_ChessUI.myTurn = true;
                }
                continue;
            }

            if (msg.type == PacketType::Cinema_State) {
                m_Cinema.active = true;
                m_Cinema.url = j.value("url", m_Cinema.url);
                m_Cinema.title = j.value("title", m_Cinema.title);
                m_Cinema.playing = j.value("playing", m_Cinema.playing);
                m_Cinema.currentTime = j.value("time", m_Cinema.currentTime);
                m_Cinema.host = j.value("u", m_Cinema.host);
                if (j.contains("action") && j["action"] == "pause") m_Cinema.playing = false;
                if (j.contains("action") && j["action"] == "play") m_Cinema.playing = true;
                if (j.contains("action") && j["action"] == "seek") m_Cinema.currentTime = j.value("time", 0.0f);
                continue;
            }

            if (msg.type == PacketType::Cinema_Stop) {
                m_Cinema = {};
                continue;
            }

            if (msg.type == PacketType::Screen_Share_State) {
                std::string action = j.value("action", "");
                std::string user = j.value("u", "");
                if (action == "start" && user != m_CurrentUser.username) {
                    m_ScreenShare.someoneSharing = true;
                    m_ScreenShare.sharingUser = user;
                } else if (action == "stop") {
                    m_ScreenShare.someoneSharing = false;
                    m_ScreenShare.sharingUser.clear();
                }
                continue;
            }

            if (msg.type == PacketType::Screen_Share_Frame) {
                if (m_ScreenShare.someoneSharing && !msg.data.empty()) {
                    m_ScreenShare.lastFrameData = msg.data;
                    m_ScreenShare.frameUpdated = true;
                }
                continue;
            }

            if (msg.type == PacketType::Admin_Action_Result) {
                std::string action = j.value("action", "");
                if (action == "force_mute") m_SelfMuted = j.value("state", true);
                else if (action == "force_deafen") { m_SelfDeafened = j.value("state", true); if (m_SelfDeafened) m_SelfMuted = true; }
                else if (action == "disconnect" && m_ActiveVoiceChannelId != -1) {
                    m_ActiveVoiceChannelId = -1;
                    m_ActiveVoiceChannelIdForVoice.store(-1);
                    m_VoiceMembers.clear();
                }
                else if (action == "move" && j.contains("cid")) {
                    int newCid = j["cid"];
                    m_ActiveVoiceChannelId = newCid;
                    m_ActiveVoiceChannelIdForVoice.store(newCid);
                    m_NetClient.Send(PacketType::Join_Voice_Channel, PacketHandler::JoinVoiceChannelPayload(newCid));
                }
                continue;
            }

            if (msg.type == PacketType::Status_Update) {
                const std::string user = j.value("u", "");
                const std::string status = j.value("status", "");
                if (!user.empty()) m_UserStatuses[user] = status;
                continue;
            }

            if (msg.type == PacketType::Presence_Update) {
                const std::string user = j.value("u", "");
                if (!user.empty()) {
                    if (j.value("online", false))
                        m_OnlineUsers.insert(user);
                    else
                        m_OnlineUsers.erase(user);
                }
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
                    { nlohmann::json mj; mj["sid"] = m_SelectedServerId;
                      m_NetClient.Send(PacketType::Member_List_Request, mj.dump()); }
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
                            ch.description = item.value("desc", "");
                            ch.userLimit = item.value("limit", 0);
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
                    for (const auto& item : j) {
                        if (!item.contains("mid")) continue;
                        ChatMessage cm{ item.value("mid", 0), item["cid"],
                                        item.value("u", ""), item.value("msg", ""),
                                        item.value("time", "Old"),
                                        item.value("reply_to", 0),
                                        item.value("pin", false) };
                        if (item.contains("reactions") && item["reactions"].is_object()) {
                            for (auto& [emoji, users] : item["reactions"].items()) {
                                std::vector<std::string> userList;
                                for (const auto& u : users) userList.push_back(u.get<std::string>());
                                cm.reactions[emoji] = userList;
                            }
                        }
                        m_Messages.push_back(std::move(cm));
                    }
                }
                continue;
            }

            if (msg.type == PacketType::Call_State) {
                std::string state = j.value("state", "");
                std::string from = j.value("from", "");
                std::string to = j.value("to", "");
                if (state == "ringing") {
                    m_CurrentCall.otherUser = from;
                    m_CurrentCall.state = "ringing";
                } else if (state == "active") {
                    m_CurrentCall.otherUser = (from == m_CurrentUser.username) ? to : from;
                    m_CurrentCall.state = "active";
                } else if (state == "ended") {
                    m_CurrentCall.otherUser.clear();
                    m_CurrentCall.state.clear();
                }
                continue;
            }

            if (msg.type == PacketType::DM_Receive) {
                DirectMessage dm;
                dm.id = j.value("mid", 0);
                dm.sender = j.value("u", "");
                dm.content = j.value("msg", "");
                dm.timestamp = GetCurrentTimeStr();
                m_DirectMessages.push_back(dm);
                if (dm.sender != m_CurrentUser.username && GetForegroundWindow() != m_Window.GetHwnd())
                    m_Sounds.PlayMessage();
                continue;
            }

            if (msg.type == PacketType::DM_History_Response) {
                m_DirectMessages.clear();
                for (const auto& item : j) {
                    DirectMessage dm;
                    dm.id = item.value("mid", 0);
                    dm.sender = item.value("u", "");
                    dm.content = item.value("msg", "");
                    dm.timestamp = item.value("time", "");
                    m_DirectMessages.push_back(dm);
                }
                continue;
            }

            if (msg.type == PacketType::Friend_List_Response) {
                m_Friends.clear();
                for (const auto& item : j)
                    m_Friends.push_back({ item.value("u", ""), item.value("status", ""), item.value("direction", "") });
                continue;
            }

            if (msg.type == PacketType::Friend_Update) {
                const std::string user = j.value("u", "");
                const std::string status = j.value("status", "");
                const std::string direction = j.value("direction", "");
                bool found = false;
                for (auto& f : m_Friends) {
                    if (f.username == user) { f.status = status; f.direction = direction; found = true; break; }
                }
                if (!found) m_Friends.push_back({ user, status, direction });
                continue;
            }

            if (msg.type == PacketType::Reaction_Update) {
                const int mid = j.value("mid", 0);
                if (mid > 0 && j.contains("reactions")) {
                    for (auto& m : m_Messages) {
                        if (m.id == mid) {
                            m.reactions.clear();
                            for (auto& [emoji, users] : j["reactions"].items()) {
                                std::vector<std::string> userList;
                                for (const auto& u : users) userList.push_back(u.get<std::string>());
                                m.reactions[emoji] = userList;
                            }
                            break;
                        }
                    }
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
                int incomingCid = j.value("cid", 0);
                m_Messages.push_back({ j.value("mid", 0), incomingCid,
                                       j.value("u", "??"), j.value("msg", ""),
                                       GetCurrentTimeStr(),
                                       j.value("reply_to", 0) });
                if (j.value("u", "") != m_CurrentUser.username) {
                    if (incomingCid != m_SelectedChannelId)
                        m_UnreadCounts[incomingCid]++;
                    if (GetForegroundWindow() != m_Window.GetHwnd())
                        m_Sounds.PlayMessage();
                }
                continue;
            }
        }
        catch (const std::exception& e) {
            LOG_ERROR(std::string("ProcessNetworkMessages: ") + e.what());
        }
        catch (...) {
            LOG_ERROR("ProcessNetworkMessages: unknown exception");
        }
    }
}

} // namespace TalkMe