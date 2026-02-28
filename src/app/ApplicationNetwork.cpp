// Network message dispatch: ProcessNetworkMessages implementation.
// Keeps the packet switch and state updates out of Application.cpp.
#include "Application.h"
#include "../shared/Protocol.h"
#include "../shared/PacketHandler.h"
#include "../core/ConfigManager.h"
#include <imgui.h>
#include "../ui/TextureManager.h"
#include <stb_image.h>
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

            // ── Binary packets (not JSON) — handle before JSON validation ──
            if (msg.type == PacketType::Screen_Share_Frame) {
                if (!msg.data.empty()) {
                    // Frames come from the currently broadcasting user — find which stream
                    // For now frames don't contain sender info, so update the viewingStream
                    std::string streamUser = m_ScreenShare.viewingStream;
                    if (streamUser.empty() && !m_ScreenShare.activeStreams.empty())
                        streamUser = m_ScreenShare.activeStreams.begin()->first;
                    if (!streamUser.empty()) {
                        auto& si = m_ScreenShare.activeStreams[streamUser];
                        si.lastFrameData = std::vector<uint8_t>(msg.data.begin(), msg.data.end());
                        si.frameUpdated = true;
                    }
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
                m_MediaBaseUrl          = "http://" + m_ServerIP + ":5557";
                {
                    const std::string dbPath = GetMessageCacheDbPath();
                    if (!dbPath.empty()) m_MessageCacheDb.Open(dbPath);
                }
                LoadStateCache();
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
                std::string game = j.value("game", "chess");
                std::string from = j.value("from", "");
                if (game == "flappy") {
                    m_FlappyBird.Reset(m_CurrentUser.username);
                } else if (game == "tictactoe") {
                    m_TicTacToe.Reset(from, false);
                } else if (game == "racing") {
                    m_Racing.Reset(m_CurrentUser.username, from);
                } else {
                    m_ChessUI.opponent = from;
                    m_ChessUI.active = false;
                }
                continue;
            }

            if (msg.type == PacketType::Game_State) {
                if (j.contains("action") && j["action"] == "accept") {
                    std::string game = j.value("game", "chess");
                    if (game == "racing") {
                        m_Racing.Reset(m_CurrentUser.username, j.value("from", ""));
                    } else if (game == "tictactoe") {
                        m_TicTacToe.Reset(j.value("from", ""), true);
                    } else {
                        m_ChessEngine.Reset();
                        m_ChessUI.active = true;
                        m_ChessUI.isWhite = (j.value("from", "") != m_CurrentUser.username);
                        m_ChessUI.myTurn = m_ChessUI.isWhite;
                        m_ChessUI.selectedRow = -1;
                        m_ChessUI.selectedCol = -1;
                    }
                }
                continue;
            }

            if (msg.type == PacketType::Game_Move) {
                // Chess moves
                if (m_ChessUI.active && j.contains("fr") && j.contains("fc") && j.contains("tr") && j.contains("tc")) {
                    int fr = j["fr"], fc = j["fc"], tr = j["tr"], tc = j["tc"];
                    m_ChessEngine.MakeMove(fr, fc, tr, tc);
                    m_ChessUI.myTurn = true;
                }
                // Tic-tac-toe moves
                if (m_TicTacToe.active && j.contains("cell")) {
                    m_TicTacToe.MakeMove(j["cell"], m_TicTacToe.TheirPiece());
                    m_TicTacToe.myTurn = true;
                }
                // Racing position updates
                if (m_Racing.active && j.contains("x") && j.contains("y")) {
                    m_Racing.UpdateOpponent(
                        j.value("x", 0.0f), j.value("y", 0.0f),
                        j.value("angle", 0.0f), j.value("speed", 0.0f),
                        j.value("lap", 0), j.value("cp", 0));
                }
                continue;
            }

            if (msg.type == PacketType::Cinema_State) {
                m_Cinema.active = true;
                m_Cinema.currentUrl = j.value("url", m_Cinema.currentUrl);
                m_Cinema.currentTitle = j.value("title", m_Cinema.currentTitle);
                m_Cinema.playing = j.value("playing", m_Cinema.playing);
                m_Cinema.currentTime = j.value("time", m_Cinema.currentTime);
                m_Cinema.duration = j.value("duration", m_Cinema.duration);
                m_Cinema.host = j.value("u", m_Cinema.host);
                if (j.contains("action")) {
                    std::string action = j["action"];
                    if (action == "pause") m_Cinema.playing = false;
                    else if (action == "play") m_Cinema.playing = true;
                    else if (action == "seek") m_Cinema.currentTime = j.value("time", 0.0f);
                    else if (action == "next" && !m_Cinema.queue.empty()) m_Cinema.queue.erase(m_Cinema.queue.begin());
                }
                if (j.contains("queue") && j["queue"].is_array()) {
                    m_Cinema.queue.clear();
                    for (const auto& item : j["queue"]) {
                        CinemaQueueItem qi;
                        qi.url = item.value("url", "");
                        qi.title = item.value("title", "");
                        qi.addedBy = item.value("by", "");
                        if (!qi.url.empty()) m_Cinema.queue.push_back(qi);
                    }
                }
                continue;
            }

            if (msg.type == PacketType::Cinema_Stop) {
                m_Cinema = {};
                continue;
            }

            if (msg.type == PacketType::Screen_Share_State) {
                std::string action = j.value("action", "");
                std::string user = j.value("u", "");
                if (action == "start" && !user.empty()) {
                    StreamInfo si;
                    si.username = user;
                    m_ScreenShare.activeStreams[user] = si;
                    if (m_ScreenShare.viewingStream.empty() && user != m_CurrentUser.username)
                        m_ScreenShare.viewingStream = user;
                } else if (action == "stop" && !user.empty()) {
                    m_ScreenShare.activeStreams.erase(user);
                    if (m_ScreenShare.viewingStream == user) {
                        m_ScreenShare.viewingStream.clear();
                        if (!m_ScreenShare.activeStreams.empty())
                            m_ScreenShare.viewingStream = m_ScreenShare.activeStreams.begin()->first;
                    }
                }
                continue;
            }

            // Screen_Share_Frame handled above (before JSON validation)

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

            if (msg.type == PacketType::Avatar_Response) {
                std::string user = j.value("u", "");
                std::string data = j.value("data", "");
                if (!user.empty() && !data.empty()) {
                    m_AvatarCache[user] = data;
                    // Decode base64 and create texture
                    // Base64 decode inline
                    static const std::string b64chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
                    std::vector<uint8_t> decoded;
                    decoded.reserve(data.size() * 3 / 4);
                    int val = 0, bits = -8;
                    for (char c : data) {
                        size_t pos = b64chars.find(c);
                        if (pos == std::string::npos) continue;
                        val = (val << 6) + (int)pos;
                        bits += 6;
                        if (bits >= 0) { decoded.push_back((uint8_t)((val >> bits) & 0xFF)); bits -= 8; }
                    }
                    if (!decoded.empty()) {
                        auto& tm = TalkMe::TextureManager::Get();
                        tm.SetDevice(m_Graphics.GetDevice());
                        tm.LoadFromMemory("avatar_" + user, decoded.data(), (int)decoded.size(), nullptr, nullptr);
                    }
                }
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
                std::vector<Server> oldList = std::move(m_ServerList);
                m_ServerList.clear();
                for (const auto& item : j) {
                    Server s;
                    s.id = item["id"];
                    s.name = item["name"];
                    s.inviteCode = item["code"];
                    auto it = std::find_if(oldList.begin(), oldList.end(), [&s](const Server& o) { return o.id == s.id; });
                    if (it != oldList.end())
                        s.channels = it->channels;
                    m_ServerList.push_back(std::move(s));
                }
                if (!m_ServerList.empty() && m_SelectedServerId == -1) {
                    m_SelectedServerId = m_ServerList[0].id;
                    m_NetClient.Send(PacketType::Get_Server_Content_Request,
                                     PacketHandler::GetServerContentPayload(m_SelectedServerId));
                    { nlohmann::json mj; mj["sid"] = m_SelectedServerId;
                      m_NetClient.Send(PacketType::Member_List_Request, mj.dump()); }
                }
                SaveStateCache();
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
                            if (typeStr == "voice") ch.type = ChannelType::Voice;
                            else if (typeStr == "cinema") ch.type = ChannelType::Cinema;
                            else if (typeStr == "announcement") ch.type = ChannelType::Announcement;
                            else ch.type = ChannelType::Text;
                            ch.description = item.value("desc", "");
                            ch.userLimit = item.value("limit", 0);
                            it->channels.push_back(ch);
                        }
                    }
                }
                SaveStateCache();
                continue;
            }

            if (msg.type == PacketType::File_Transfer_Complete) {
                std::string action = j.value("action", "");
                if (action == "upload_approved" && j.contains("id") && !m_PendingUploadData.empty()) {
                    std::string id = j["id"].get<std::string>();
                    const size_t chunkSize = 32 * 1024;
                    for (size_t offset = 0; offset < m_PendingUploadData.size(); offset += chunkSize) {
                        size_t len = (std::min)(chunkSize, m_PendingUploadData.size() - offset);
                        std::vector<uint8_t> chunk(m_PendingUploadData.data() + offset, m_PendingUploadData.data() + offset + len);
                        m_NetClient.SendRaw(PacketType::File_Transfer_Chunk, chunk);
                    }
                    m_NetClient.Send(PacketType::File_Transfer_Complete, "{}");
                    json msgJ;
                    msgJ["cid"] = m_PendingUploadChannelId;
                    msgJ["u"] = m_CurrentUser.username;
                    msgJ["msg"] = "";
                    msgJ["attachment_id"] = id;
                    m_NetClient.Send(PacketType::Message_Text, msgJ.dump());
                    m_PendingUploadData.clear();
                    m_PendingUploadFilename.clear();
                    m_PendingUploadChannelId = -1;
                }
                continue;
            }

            if (msg.type == PacketType::Media_Response) {
                if (!j.contains("id") || !j.contains("data") || !j["data"].is_string()) continue;
                std::string id = j["id"].get<std::string>();
                std::string b64 = j["data"].get<std::string>();
                m_AttachmentRequested.erase(id);
                static const std::string b64chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
                std::string raw;
                raw.reserve((b64.size() / 4) * 3);
                int val = 0, bits = -8;
                for (char c : b64) {
                    size_t p = b64chars.find(c);
                    if (p == std::string::npos) { if (c == '=') break; continue; }
                    val = (val << 6) + (int)p;
                    bits += 6;
                    if (bits >= 0) { raw += (char)((val >> bits) & 0xFF); bits -= 8; }
                }
                m_AttachmentFileData[id] = std::vector<uint8_t>(raw.begin(), raw.end());
                int w = 0, h = 0, ch = 0;
                unsigned char* pixels = stbi_load_from_memory(
                    reinterpret_cast<const unsigned char*>(raw.data()), (int)raw.size(), &w, &h, &ch, 4);
                if (pixels && w > 0 && h > 0) {
                    auto& tm = TextureManager::Get();
                    std::string texId = "att_" + id;
                    tm.LoadFromRGBA(texId, pixels, w, h);
                    stbi_image_free(pixels);
                    AttachmentDisplay disp;
                    disp.ready = true;
                    disp.failed = false;
                    disp.textureId = texId;
                    disp.width = w;
                    disp.height = h;
                    m_AttachmentCache[id] = std::move(disp);
                } else {
                    if (pixels) stbi_image_free(pixels);
                    AttachmentDisplay disp;
                    disp.ready = false;
                    disp.failed = true;
                    m_AttachmentCache[id] = std::move(disp);
                }
                continue;
            }

            if (msg.type == PacketType::Message_History_Response) {
                nlohmann::json arr;
                int cid = 0;
                bool hasMeta = false;
                int oldestMid = 0;
                int newestMid = 0;
                bool hasMoreOlder = false;
                bool hasMoreNewer = false;

                if (j.is_object() && j.contains("messages") && j["messages"].is_array()) {
                    cid = j.value("cid", 0);
                    arr = j["messages"];
                    hasMeta = true;
                    oldestMid = j.value("oldest_mid", 0);
                    newestMid = j.value("newest_mid", 0);
                    hasMoreOlder = j.value("has_more_older", false);
                    hasMoreNewer = j.value("has_more_newer", false);
                } else if (j.is_array()) {
                    arr = j;
                    if (!arr.empty() && arr[0].is_object()) cid = arr[0].value("cid", 0);
                }

                if (cid > 0 && arr.is_array()) {
                    auto& state = m_ChannelStates[cid];
                    const bool wasLoadingOlder = state.loadingOlder;

                    std::vector<ChatMessage> newMsgs;
                    for (const auto& item : arr) {
                        if (!item.is_object() || !item.contains("mid")) continue;
                        ChatMessage cm{ item.value("mid", 0), item.value("cid", cid),
                                        item.value("u", ""), item.value("msg", ""),
                                        item.value("time", "Old"),
                                        item.value("reply_to", 0),
                                        item.value("pin", false) };
                        cm.attachmentId = item.value("attachment_id", item.value("attachment", ""));
                        if (item.contains("reactions") && item["reactions"].is_object()) {
                            for (auto& [emoji, users] : item["reactions"].items()) {
                                std::vector<std::string> userList;
                                for (const auto& u : users) if (u.is_string()) userList.push_back(u.get<std::string>());
                                cm.reactions[emoji] = std::move(userList);
                            }
                        }
                        newMsgs.push_back(std::move(cm));
                    }

                    if (!newMsgs.empty()) {
                        m_MessageCacheDb.UpsertMessages(newMsgs);
                        m_MessageCacheDb.PruneKeepLast(cid, 2000);
                    }

                    if (wasLoadingOlder && !state.messages.empty() && !newMsgs.empty()) {
                        int oldestCurrent = state.messages.front().id;
                        if (newMsgs.back().id < oldestCurrent)
                            state.messages.insert(state.messages.begin(), newMsgs.begin(), newMsgs.end());
                    } else if (!newMsgs.empty()) {
                        state.messages = std::move(newMsgs);
                    }

                    state.loadingOlder = false;
                    state.loadingLatest = false;

                    if (hasMeta) {
                        state.oldestLoadedMid = oldestMid;
                        state.newestLoadedMid = newestMid;
                        state.hasMoreOlder = hasMoreOlder;
                        state.hasMoreNewer = hasMoreNewer;
                    } else if (!state.messages.empty()) {
                        state.oldestLoadedMid = state.messages.front().id;
                        state.newestLoadedMid = state.messages.back().id;
                    }

                    SaveStateCache();
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
                    for (auto& [cid, state] : m_ChannelStates) {
                        for (auto& m : state.messages) {
                            if (m.id == mid) {
                                m.reactions.clear();
                                for (auto& [emoji, users] : j["reactions"].items()) {
                                    std::vector<std::string> userList;
                                    for (const auto& u : users) userList.push_back(u.get<std::string>());
                                    m.reactions[emoji] = userList;
                                }
                                SaveStateCache();
                                goto reaction_done;
                            }
                        }
                    }
                    reaction_done:;
                }
                continue;
            }

            if (msg.type == PacketType::Message_Edit) {
                const int mid = j.value("mid", 0);
                const std::string newMsg = j.value("msg", "");
                for (auto& [cid, state] : m_ChannelStates) {
                    for (auto& m : state.messages) {
                        if (m.id == mid) {
                            m.content = newMsg;
                            m_MessageCacheDb.UpsertMessages(std::vector<ChatMessage>{ m });
                            SaveStateCache();
                            goto edit_done;
                        }
                    }
                }
                edit_done:;
                continue;
            }

            if (msg.type == PacketType::Message_Pin_Update) {
                const int mid = j.value("mid", 0);
                const bool pin = j.value("pin", false);
                const int cid = j.value("cid", 0);
                if (mid > 0) {
                    if (cid > 0) {
                        auto& state = m_ChannelStates[cid];
                        for (auto& m : state.messages) {
                            if (m.id == mid) { m.pinned = pin; m_MessageCacheDb.UpsertMessages(std::vector<ChatMessage>{ m }); SaveStateCache(); break; }
                        }
                    } else {
                        for (auto& [c, state] : m_ChannelStates) {
                            for (auto& m : state.messages) {
                                if (m.id == mid) { m.pinned = pin; m_MessageCacheDb.UpsertMessages(std::vector<ChatMessage>{ m }); SaveStateCache(); goto pin_done; }
                            }
                        }
                        pin_done:;
                    }
                }
                continue;
            }

            if (msg.type == PacketType::Message_Delete) {
                const int mid = j.value("mid", 0);
                for (auto& [cid, state] : m_ChannelStates) {
                    auto it = std::remove_if(state.messages.begin(), state.messages.end(),
                        [mid](const ChatMessage& m) { return m.id == mid; });
                    if (it != state.messages.end()) {
                        state.messages.erase(it, state.messages.end());
                        SaveStateCache();
                        break;
                    }
                }
                continue;
            }

            if (msg.type == PacketType::Message_Text) {
                int incomingCid = j.value("cid", 0);
                std::string msgContent = j.value("msg", "");
                int newMid = j.value("mid", 0);
                ChatMessage cm{ newMid, incomingCid, j.value("u", "??"), msgContent,
                                GetCurrentTimeStr(), j.value("reply_to", 0), false };
                cm.attachmentId = j.value("attachment_id", "");
                auto& state = m_ChannelStates[incomingCid];
                state.messages.push_back(std::move(cm));
                if (!state.messages.empty())
                    m_MessageCacheDb.UpsertMessages(std::vector<ChatMessage>{ state.messages.back() });

                // Maintain bounded in-memory window for active paging.
                constexpr size_t kMaxWindow = 200;
                if (state.messages.size() > kMaxWindow) {
                    state.messages.erase(state.messages.begin(), state.messages.begin() + (state.messages.size() - kMaxWindow));
                }

                SaveStateCache();
                if (j.value("u", "") != m_CurrentUser.username) {
                    if (incomingCid != m_SelectedChannelId)
                        m_UnreadCounts[incomingCid]++;

                    // Check for mentions (@username#tag or @all)
                    bool isMentioned = false;
                    if (msgContent.find("@all") != std::string::npos)
                        isMentioned = true;
                    if (!isMentioned && msgContent.find("@" + m_CurrentUser.username) != std::string::npos)
                        isMentioned = true;
                    if (!isMentioned) {
                        // Also check without #tag
                        size_t hashPos = m_CurrentUser.username.find('#');
                        if (hashPos != std::string::npos) {
                            std::string shortName = m_CurrentUser.username.substr(0, hashPos);
                            if (msgContent.find("@" + shortName) != std::string::npos)
                                isMentioned = true;
                        }
                    }

                    if (isMentioned && !m_NotifSettings.muteMentions) {
                        m_Sounds.PlayMention();
                    } else if (!m_NotifSettings.muteMessages && GetForegroundWindow() != m_Window.GetHwnd()) {
                        m_Sounds.PlayMessage();
                    }
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