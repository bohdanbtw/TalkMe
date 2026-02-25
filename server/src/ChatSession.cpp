#include "ChatSession.h"
#include "TalkMeServer.h"
#include "Database.h"
#include "Crypto.h"
#include "Logger.h"
#include "Protocol.h"
#include <nlohmann/json.hpp>
#include <cstring>
#include <filesystem>
#include <algorithm>
#include <ctime>
#include <random>
#include <cstdio>
#include <sstream>
#include <iomanip>

using json = nlohmann::json;

namespace {
    std::string UrlEncode(const std::string& value) {
        std::ostringstream out;
        for (unsigned char c : value) {
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~')
                out << c;
            else
                out << '%' << std::hex << std::uppercase << std::setw(2) << std::setfill('0') << (int)c;
        }
        return out.str();
    }
}
using asio::ip::tcp;

namespace TalkMe {

    ChatSession::ChatSession(tcp::socket socket, TalkMeServer& server)
        : m_Socket(std::move(socket)), m_Server(server), m_Strand(asio::make_strand(m_Socket.get_executor())) {
    }

    void ChatSession::Start() {
        m_LastVoicePacket = std::chrono::steady_clock::now();
        m_Server.JoinClient(shared_from_this());
        ReadHeader();
    }

    void ChatSession::Disconnect() {
        if (m_UploadFile.is_open()) {
            m_UploadFile.close();
            // Prevent storage exhaustion: delete partial files if the transfer didn't finish
            if (m_UploadBytesReceived < m_UploadTargetSize && !m_UploadId.empty()) {
                std::error_code ec;
                std::filesystem::remove("attachments/" + m_UploadId, ec);
            }
        }
        m_Server.LeaveClient(shared_from_this());
        if (m_Socket.is_open()) m_Socket.close();
    }

    void ChatSession::ReadHeader() {
        asio::async_read(m_Socket, asio::buffer(&m_Header, sizeof(TalkMe::PacketHeader)),
            asio::bind_executor(m_Strand, [this, self = shared_from_this()](std::error_code ec, std::size_t) {
                if (!ec) {
                    m_Header.ToHost();
                    if (m_Header.size > 10 * 1024 * 1024) { Disconnect(); return; }
                    m_Body.resize(m_Header.size);
                    ReadBody();
                }
                else {
                    std::fprintf(stderr, "[TalkMe Server] ReadHeader error: %s (%d), disconnecting\n", ec.message().c_str(), ec.value());
                    Disconnect();
                }
                }));
    }

    void ChatSession::ReadBody() {
        asio::async_read(m_Socket, asio::buffer(m_Body),
            asio::bind_executor(m_Strand, [this, self = shared_from_this()](std::error_code ec, std::size_t) {
                if (!ec) { ProcessPacket(); ReadHeader(); }
                else {
                    std::fprintf(stderr, "[TalkMe Server] ReadBody error: %s (%d), disconnecting\n", ec.message().c_str(), ec.value());
                    Disconnect();
                }
                }));
    }

    void ChatSession::DoWrite() {
        if (m_WriteQueue.empty()) return;

        asio::async_write(m_Socket, asio::buffer(*m_WriteQueue.front()),
            asio::bind_executor(m_Strand, [this, self = shared_from_this()](std::error_code ec, std::size_t) {
                if (!ec) {
                    m_WriteQueue.pop_front();
                    if (!m_WriteQueue.empty()) DoWrite();
                }
                else {
                    m_IsHealthy.store(false, std::memory_order_relaxed);
                    Disconnect();
                }
                }));
    }

    void ChatSession::SendShared(std::shared_ptr<std::vector<uint8_t>> buffer, bool isVoiceData) {
        asio::post(m_Strand, [this, self = shared_from_this(), buffer, isVoiceData]() {
            bool writeInProgress = !m_WriteQueue.empty();
            // OPTIMIZATION & BUG FIX: Never pop_front() if writeInProgress is true.
            // ASIO is actively reading from m_WriteQueue.front() in the background.
            // Freeing it causes severe Use-After-Free memory corruption and crashes.

            if (isVoiceData) {
                // Just drop the incoming packet if the queue is backed up
                size_t voiceDropAt = 100;
                if (m_CurrentVoiceLoad > 80) voiceDropAt = 12;
                else if (m_CurrentVoiceLoad > 30) voiceDropAt = 24;
                else if (m_CurrentVoiceLoad > 8) voiceDropAt = 32;
                else if (m_CurrentVoiceLoad > 4) voiceDropAt = 48;
                if (m_WriteQueue.size() >= voiceDropAt) {
                    return; // Discard late voice frame gracefully
                }
            }
            else {
                // Control/Text packets should be queued with higher tolerance
                if (m_WriteQueue.size() > 200) {
                    return; // Extreme congestion limit
                }
            }
            m_WriteQueue.push_back(buffer);
            if (!writeInProgress) DoWrite();
        });
    }

    void ChatSession::UpdateActivity() {
        auto now = std::chrono::steady_clock::now();
        int64_t ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
        m_LastActivityTimeMs.store(ms, std::memory_order_relaxed);
    }

    void ChatSession::TouchVoiceActivity() {
        m_LastVoicePacket = std::chrono::steady_clock::now();
        m_VoicePacketCount = 0;
    }

    void ChatSession::ProcessPacket() {
        using namespace TalkMe;

        UpdateActivity();

        if (m_Header.type == PacketType::Voice_Data_Opus || m_Header.type == PacketType::Voice_Data) {
            if (m_CurrentVoiceCid != -1) {
                // Bug fix: elapsed MUST be computed before updating m_LastVoicePacket.
                // The old code called TouchVoiceActivity() first, which stamped
                // m_LastVoicePacket = now and reset m_VoicePacketCount = 0.
                // The subsequent elapsed calculation was therefore always ~0 ms,
                // meaning the "< 1000" branch was always taken, and the counter
                // was reset before it could ever accumulate. The rate limiter
                // never fired — unlimited TCP voice spam passed straight through.
                //
                // Correct logic: m_LastVoicePacket is the start of the current
                // 1-second window. Advance it and reset the counter only when the
                // window expires. UpdateActivity() (called unconditionally above)
                // handles the general idle-eviction timestamp independently.
                auto now = std::chrono::steady_clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - m_LastVoicePacket);

                if (elapsed.count() >= 1000) {
                    m_VoicePacketCount = 0;
                    m_LastVoicePacket = now;
                }

                if (++m_VoicePacketCount > 100) return;

                m_Server.BroadcastVoice(m_CurrentVoiceCid, shared_from_this(), m_Header, m_Body);
            }
            return;
        }

        if (m_Header.type == PacketType::Echo_Request) {
            PacketHeader h{ PacketType::Echo_Response, static_cast<uint32_t>(m_Body.size()) };
            h.ToNetwork();
            auto buffer = std::make_shared<std::vector<uint8_t>>(sizeof(h) + m_Body.size());
            std::memcpy(buffer->data(), &h, sizeof(h));
            if (!m_Body.empty()) std::memcpy(buffer->data() + sizeof(h), m_Body.data(), m_Body.size());
            SendShared(buffer, false);
            return;
        }

        if (m_Header.type == PacketType::Receiver_Report) {
            if (m_Body.size() >= sizeof(ReceiverReportPayload)) {
                ReceiverReportPayload rr;
                std::memcpy(&rr, m_Body.data(), sizeof(rr));
                rr.ToHost();

                SenderReportPayload sr{};
                double jitterGradient = static_cast<double>(rr.jitterMs) - m_LastJitterMs;
                m_LastJitterMs = rr.jitterMs;

                if (rr.fractionLost > 10 || jitterGradient > 30.0) {
                    m_CurrentAssignedBitrateKbps = std::max(16u, m_CurrentAssignedBitrateKbps / 2);
                    m_ConsecutiveStableReports = 0;
                    sr.networkState = 2;
                }
                else if (rr.fractionLost == 0 && jitterGradient < 10.0 && rr.jitterMs < 60) {
                    m_ConsecutiveStableReports++;
                    if (m_ConsecutiveStableReports >= 3) {
                        m_CurrentAssignedBitrateKbps = std::min(64u, m_CurrentAssignedBitrateKbps + 4);
                        m_ConsecutiveStableReports = 0;
                    }
                    sr.networkState = 0;
                }
                else {
                    m_ConsecutiveStableReports = 0;
                    sr.networkState = 1;
                }

                uint32_t channelLimit = m_Server.GetChannelBitrateLimit(m_CurrentVoiceCid);
                sr.suggestedBitrateKbps = (std::min)(m_CurrentAssignedBitrateKbps, channelLimit);

                PacketHeader h{ PacketType::Sender_Report, sizeof(sr) };
                sr.ToNetwork();
                h.ToNetwork();
                auto buffer = std::make_shared<std::vector<uint8_t>>(sizeof(h) + sizeof(sr));
                std::memcpy(buffer->data(), &h, sizeof(h));
                std::memcpy(buffer->data() + sizeof(h), &sr, sizeof(sr));
                SendShared(buffer, false);
            }
            return;
        }

        if (m_Header.type == PacketType::File_Transfer_Chunk) {
            if (m_UploadFile.is_open()) {
                // Prevent infinite chunking DoS attacks
                if (m_UploadBytesReceived + m_Body.size() > m_UploadTargetSize) {
                    m_UploadFile.close();
                    Disconnect();
                    return;
                }
                m_UploadFile.write(reinterpret_cast<const char*>(m_Body.data()), static_cast<std::streamsize>(m_Body.size()));
                m_UploadBytesReceived += m_Body.size();
            }
            return;
        }

        auto SendLocal = [this](PacketType type, const std::string& data) {
            uint32_t size = static_cast<uint32_t>(data.size());
            PacketHeader header = { type, size };
            header.ToNetwork();
            auto buffer = std::make_shared<std::vector<uint8_t>>(sizeof(header) + size);
            std::memcpy(buffer->data(), &header, sizeof(header));
            if (size > 0) std::memcpy(buffer->data() + sizeof(header), data.data(), size);
            SendShared(buffer, false);
            };

        std::string payload(m_Body.begin(), m_Body.end());
        try {
            auto j = json::parse(payload);

            if (m_Header.type == PacketType::Register_Request) {
                std::string new_user = Database::Get().RegisterUser(j.value("e", ""), j["u"], j["p"]);
                if (!new_user.empty()) {
                    m_Username = new_user;
                    Database::Get().AddUserToDefaultServer(new_user);
                    json res; res["u"] = new_user;
                    SendLocal(PacketType::Register_Success, res.dump());
                    SendLocal(PacketType::Server_List_Response, Database::Get().GetUserServersJSON(new_user));
                }
                else {
                    SendLocal(PacketType::Register_Failed, "");
                }
                return;
            }

            if (m_Header.type == PacketType::Login_Request) {
                std::fprintf(stderr, "[TalkMe Server] Login_Request received, calling LoginUserAsync...\n");
                std::fflush(stderr);
                std::string email = j.value("e", "");
                std::string pass;
                if (j.contains("p") && j["p"].is_string())
                    pass = j["p"].get<std::string>();
                std::string hwid = j.value("hwid", "");
                auto self = shared_from_this();
                Database::Get().LoginUserAsync(email, pass, hwid,
                    [this, self, hwid](int loginResult, std::string username, std::string serversJson, bool has2fa) {
                        asio::post(m_Strand, [this, self, loginResult, username = std::move(username),
                            serversJson = std::move(serversJson), has2fa, hwid]() {
                                auto sendResp = [this](PacketType type, const std::string& data) {
                                    uint32_t size = static_cast<uint32_t>(data.size());
                                    PacketHeader header = { type, size };
                                    header.ToNetwork();
                                    auto buffer = std::make_shared<std::vector<uint8_t>>(sizeof(header) + size);
                                    std::memcpy(buffer->data(), &header, sizeof(header));
                                    if (size > 0) std::memcpy(buffer->data() + sizeof(header), data.data(), size);
                                    SendShared(buffer, false);
                                    };
                                std::fprintf(stderr, "[TalkMe Server] LoginUserAsync returned %d\n", loginResult);
                                std::fflush(stderr);
                                if (loginResult == 1) {
                                    m_Username = username;
                                    json res; res["u"] = username; res["2fa_enabled"] = has2fa;
                                    sendResp(PacketType::Login_Success, res.dump());
                                    if (!serversJson.empty())
                                        sendResp(PacketType::Server_List_Response, serversJson);
                                }
                                else if (loginResult == 2) {
                                    m_PendingHWID = hwid;
                                    json res; res["u"] = username;
                                    sendResp(PacketType::Login_Requires_2FA, res.dump());
                                }
                                else {
                                    sendResp(PacketType::Login_Failed, "");
                                }
                            });
                    });
                return;
            }

            if (m_Header.type == PacketType::Validate_Session_Request) {
                std::string u = Database::Get().ValidateSession(j.value("e", ""), j.value("ph", ""));
                if (!u.empty()) {
                    m_Username = u;
                    json res; res["valid"] = true; res["u"] = u;
                    SendLocal(PacketType::Validate_Session_Response, res.dump());
                    // Bug fix: the old code stopped here. A reconnecting client
                    // (network switch, brief drop) needs the server/channel list
                    // to restore its full UI state. Without it every reconnect
                    // appeared as a blank slate. Send the list immediately after
                    // confirming the session, mirroring the Login_Success flow.
                    SendLocal(PacketType::Server_List_Response,
                        Database::Get().GetUserServersJSON(u));
                }
                else {
                    json res; res["valid"] = false;
                    SendLocal(PacketType::Validate_Session_Response, res.dump());
                }
                return;
            }

            if (m_Header.type == PacketType::Submit_2FA_Login_Request) {
                std::string email = j.value("email", "");
                std::string code = j.value("code", "");
                // Accept hwid from the submit packet itself as well - this is the
                // reliable path because m_PendingHWID can be lost if the TCP session
                // is recreated between Login_Request and Submit_2FA_Login_Request.
                std::string submitHwid = j.value("hwid", "");
                if (!submitHwid.empty()) m_PendingHWID = submitHwid;
                std::string username;
                std::string secret = Database::Get().GetUserTOTPSecret(email, &username);
                if (secret.empty() || !VerifyTOTP(secret, code)) {
                    SendLocal(PacketType::Login_Failed, "");
                }
                else {
                    m_Username = username;
                    if (m_PendingHWID.empty())
                        std::fprintf(stderr, "[TalkMe] 2FA verified but no HWID present — device will not be trusted; user will be prompted for 2FA on next login.\n");
                    Database::Get().TrustDevice(username, m_PendingHWID);
                    json res; res["u"] = m_Username; res["2fa_enabled"] = true;
                    SendLocal(PacketType::Login_Success, res.dump());
                    SendLocal(PacketType::Server_List_Response, Database::Get().GetUserServersJSON(m_Username));
                }
                return;
            }

            if (m_Username.empty()) return;

            if (m_Header.type == PacketType::Disable_2FA_Request) {
                std::string code = j.value("code", "");
                std::string secret = Database::Get().GetUserTOTPSecret(m_Username, nullptr);
                bool ok = (!secret.empty() && VerifyTOTP(secret, code)) && Database::Get().DisableUser2FA(m_Username);
                json res; res["ok"] = ok;
                SendLocal(PacketType::Disable_2FA_Response, res.dump());
                return;
            }

            if (m_Header.type == PacketType::Generate_2FA_Secret_Request) {
                m_Pending2FASecret = GenerateBase32Secret(16);
                size_t hashPos = m_Username.find('#');
                std::string displayName = (hashPos != std::string::npos) ? m_Username.substr(0, hashPos) : m_Username;
                std::string label = "TalkMe:" + UrlEncode(displayName);
                std::string uri = "otpauth://totp/" + label + "?secret=" + m_Pending2FASecret + "&issuer=TalkMe";
                json res; res["secret"] = m_Pending2FASecret; res["uri"] = uri;
                SendLocal(PacketType::Generate_2FA_Secret_Response, res.dump());
                return;
            }

            if (m_Header.type == PacketType::Verify_2FA_Setup_Request) {
                std::string code = j.value("code", "");
                if (m_Pending2FASecret.empty()) {
                    json res; res["ok"] = false; SendLocal(PacketType::Verify_2FA_Setup_Request, res.dump());
                }
                else if (VerifyTOTP(m_Pending2FASecret, code)) {
                    if (Database::Get().EnableUser2FA(m_Username, m_Pending2FASecret)) {
                        m_Pending2FASecret.clear();
                        json res; res["ok"] = true; SendLocal(PacketType::Verify_2FA_Setup_Request, res.dump());
                    }
                    else {
                        json res; res["ok"] = false; SendLocal(PacketType::Verify_2FA_Setup_Request, res.dump());
                    }
                }
                else {
                    json res; res["ok"] = false; SendLocal(PacketType::Verify_2FA_Setup_Request, res.dump());
                }
                return;
            }

            if (m_Header.type == PacketType::Create_Server_Request) {
                Database::Get().CreateServer(j["name"], m_Username);
                SendLocal(PacketType::Server_List_Response, Database::Get().GetUserServersJSON(m_Username));
                return;
            }

            if (m_Header.type == PacketType::Join_Server_Request) {
                Database::Get().JoinServer(m_Username, j["code"]);
                SendLocal(PacketType::Server_List_Response, Database::Get().GetUserServersJSON(m_Username));
                return;
            }
            if (m_Header.type == PacketType::Get_Server_Content_Request) {
                SendLocal(PacketType::Server_Content_Response, Database::Get().GetServerContentJSON(j["sid"]));
                return;
            }

            if (m_Header.type == PacketType::Create_Channel_Request) {
                Database::Get().CreateChannel(j["sid"], j["name"], j["type"]);
                SendLocal(PacketType::Server_Content_Response, Database::Get().GetServerContentJSON(j["sid"]));
                return;
            }

            if (m_Header.type == PacketType::Select_Text_Channel) {
                SendLocal(PacketType::Message_History_Response, Database::Get().GetMessageHistoryJSON(j["cid"]));
                return;
            }

            if (m_Header.type == PacketType::Join_Voice_Channel) {
                int oldCid = m_CurrentVoiceCid;
                m_CurrentVoiceCid.store(j["cid"], std::memory_order_relaxed);
                TouchVoiceActivity();
                m_Server.SetVoiceChannel(shared_from_this(), m_CurrentVoiceCid, oldCid);
                return;
            }

            if (m_Header.type == PacketType::Delete_Message_Request) {
                if (Database::Get().DeleteMessage(j["mid"], j["cid"], m_Username)) {
                    int cid = j["cid"];
                    json res;
                    res["mid"] = j["mid"];
                    res["cid"] = cid;
                    m_Server.BroadcastToChannelMembers(cid, PacketType::Message_Delete, res.dump());
                }
                return;
            }

            if (m_Header.type == PacketType::Edit_Message_Request) {
                if (Database::Get().EditMessage(j["mid"], m_Username, j["msg"])) {
                    int cid = j["cid"];
                    m_Server.BroadcastToAll(PacketType::Message_History_Response, Database::Get().GetMessageHistoryJSON(cid));
                }
                return;
            }

            if (m_Header.type == PacketType::Pin_Message_Request) {
                if (Database::Get().PinMessage(j["mid"], j["cid"], m_Username, j["pin"])) {
                    int cid = j["cid"];
                    m_Server.BroadcastToAll(PacketType::Message_History_Response, Database::Get().GetMessageHistoryJSON(cid));
                }
                return;
            }

            if (m_Header.type == PacketType::File_Transfer_Request) {
                if (m_UploadFile.is_open()) m_UploadFile.close();
                size_t size = j.value("size", 0);
                if (size > 10 * 1024 * 1024) return;
                // Enforce maximum size during chunking
                m_UploadTargetSize = size;
                std::string filename = j.value("filename", "");
                size_t pos = filename.find_last_of("/\\");
                std::string base = (pos != std::string::npos) ? filename.substr(pos + 1) : filename;
                std::mt19937 rng(std::random_device{}());
                std::uniform_int_distribution<uint32_t> dist;
                char randHex[9];
                std::snprintf(randHex, sizeof(randHex), "%08x", dist(rng));
                m_UploadId = std::to_string(std::time(nullptr)) + "_" + randHex + "_" + base;
                m_UploadBytesReceived = 0;
                // Ensure the directory actually exists before opening the file
                std::error_code ec;
                std::filesystem::create_directories("attachments", ec);
                m_UploadFile.open("attachments/" + m_UploadId, std::ios::binary);
                json res;
                res["action"] = "upload_approved";
                res["id"] = m_UploadId;
                SendLocal(PacketType::File_Transfer_Complete, res.dump());
                return;
            }

            if (m_Header.type == PacketType::File_Transfer_Complete) {
                m_UploadFile.close();
                json res;
                res["action"] = "upload_finished";
                res["id"] = m_UploadId;
                SendLocal(PacketType::File_Transfer_Complete, res.dump());
                return;
            }

            if (m_Header.type == PacketType::Message_Text) {
                int cid = j["cid"];
                int mid = Database::Get().SaveMessageReturnId(cid, m_Username, j["msg"], j.value("attachment_id", ""));
                json out;
                out["mid"] = mid;
                out["cid"] = cid;
                out["u"] = m_Username;
                out["msg"] = j["msg"];
                out["attachment_id"] = j.value("attachment_id", "");
                m_Server.BroadcastToChannelMembers(cid, PacketType::Message_Text, out.dump());
                return;
            }

            if (m_Header.type == PacketType::Voice_Stats_Report) {
                m_Server.RecordVoiceStats(m_Username, j.value("cid", -1),
                    j.value("ping_ms", 0.0), j.value("loss_pct", 0.0),
                    j.value("jitter_ms", 0.0), j.value("buffer_ms", 0));
                return;
            }
        }
        catch (const json::exception& e) {
            VoiceTrace::log(std::string("step=json_error msg=") + e.what());
            std::fprintf(stderr, "[TalkMe Server] ProcessPacket json_error: %s\n", e.what());
            if (m_Header.type == PacketType::Login_Request) {
                auto SendFail = [this](PacketType type, const std::string& data) {
                    uint32_t size = static_cast<uint32_t>(data.size());
                    PacketHeader header = { type, size };
                    header.ToNetwork();
                    auto buffer = std::make_shared<std::vector<uint8_t>>(sizeof(header) + size);
                    std::memcpy(buffer->data(), &header, sizeof(header));
                    if (size > 0) std::memcpy(buffer->data() + sizeof(header), data.data(), size);
                    SendShared(buffer, false);
                    };
                SendFail(PacketType::Login_Failed, "");
            }
        }
        catch (const std::exception& e) {
            VoiceTrace::log(std::string("step=std_error msg=") + e.what());
            std::fprintf(stderr, "[TalkMe Server] ProcessPacket std_error: %s\n", e.what());
            if (m_Header.type == PacketType::Login_Request) {
                auto SendFail = [this](PacketType type, const std::string& data) {
                    uint32_t size = static_cast<uint32_t>(data.size());
                    PacketHeader header = { type, size };
                    header.ToNetwork();
                    auto buffer = std::make_shared<std::vector<uint8_t>>(sizeof(header) + size);
                    std::memcpy(buffer->data(), &header, sizeof(header));
                    if (size > 0) std::memcpy(buffer->data() + sizeof(header), data.data(), size);
                    SendShared(buffer, false);
                    };
                SendFail(PacketType::Login_Failed, "");
            }
        }
        catch (...) {
            VoiceTrace::log("step=unknown_error msg=unknown_exception_caught");
            std::fprintf(stderr, "[TalkMe Server] ProcessPacket unknown exception\n");
            if (m_Header.type == PacketType::Login_Request) {
                auto SendFail = [this](PacketType type, const std::string& data) {
                    uint32_t size = static_cast<uint32_t>(data.size());
                    PacketHeader header = { type, size };
                    header.ToNetwork();
                    auto buffer = std::make_shared<std::vector<uint8_t>>(sizeof(header) + size);
                    std::memcpy(buffer->data(), &header, sizeof(header));
                    if (size > 0) std::memcpy(buffer->data() + sizeof(header), data.data(), size);
                    SendShared(buffer, false);
                    };
                SendFail(PacketType::Login_Failed, "");
            }
        }
    }

} // namespace TalkMe