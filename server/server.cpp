#include <iostream>
#include <memory>
#include <unordered_set>
#include <unordered_map>
#include <vector>
#include <deque>
#include <mutex>
#include <algorithm>
#include <cstring>
#include <random>
#include <thread>
#include <chrono>
#include <asio.hpp>
#include <nlohmann/json.hpp> 
#include <sqlite3.h>
#include "Protocol.h"

using json = nlohmann::json;
using asio::ip::tcp;

class Database {
public:
    static Database& Get() { static Database db; return db; }

    Database() {
        if (sqlite3_open("talkme.db", &m_Db) != SQLITE_OK) std::cerr << "Can't open DB\n";
        
        sqlite3_exec(m_Db, "PRAGMA journal_mode=WAL;", 0, 0, 0);
        sqlite3_exec(m_Db, "PRAGMA synchronous=NORMAL;", 0, 0, 0);

        sqlite3_stmt* checkStmt;
        if (sqlite3_prepare_v2(m_Db, "SELECT email FROM users LIMIT 1;", -1, &checkStmt, 0) != SQLITE_OK) {
            sqlite3_exec(m_Db, "DROP TABLE IF EXISTS users;", 0, 0, 0); 
        }
        sqlite3_finalize(checkStmt);

        const char* sql = 
            "CREATE TABLE IF NOT EXISTS users (email TEXT PRIMARY KEY, username TEXT, password TEXT);"
            "CREATE TABLE IF NOT EXISTS servers (id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT, invite_code TEXT UNIQUE, owner TEXT);"
            "CREATE TABLE IF NOT EXISTS channels (id INTEGER PRIMARY KEY AUTOINCREMENT, server_id INTEGER, name TEXT, type TEXT);"
            "CREATE TABLE IF NOT EXISTS server_members (username TEXT, server_id INTEGER, PRIMARY KEY(username, server_id));"
            "CREATE TABLE IF NOT EXISTS messages (id INTEGER PRIMARY KEY AUTOINCREMENT, channel_id INTEGER, sender TEXT, content TEXT, time DATETIME DEFAULT CURRENT_TIMESTAMP);";
        sqlite3_exec(m_Db, sql, 0, 0, 0);
    }
    ~Database() { sqlite3_close(m_Db); }

    std::string GenerateInviteCode() {
        const char charset[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
        std::default_random_engine rng(std::random_device{}());
        std::uniform_int_distribution<> dist(0, sizeof(charset) - 2);
        std::string code = ""; for (int i = 0; i < 6; ++i) code += charset[dist(rng)];
        return code;
    }

    std::string RegisterUser(const std::string& email, std::string u, const std::string& p) {
        std::lock_guard<std::mutex> lock(m_Mutex);
        if (email.empty() || u.empty()) return "";
        u.erase(std::remove(u.begin(), u.end(), '#'), u.end());
        
        sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(m_Db, "SELECT email FROM users WHERE email = ?;", -1, &stmt, 0) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, email.c_str(), -1, SQLITE_STATIC);
            if (sqlite3_step(stmt) == SQLITE_ROW) { sqlite3_finalize(stmt); return ""; }
            sqlite3_finalize(stmt);
        }

        int next_tag = 1;
        if (sqlite3_prepare_v2(m_Db, "SELECT username FROM users WHERE username LIKE ? ORDER BY username DESC LIMIT 1;", -1, &stmt, 0) == SQLITE_OK) {
            std::string like_pattern = u + "#%";
            sqlite3_bind_text(stmt, 1, like_pattern.c_str(), -1, SQLITE_STATIC);
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                std::string last_user = (const char*)sqlite3_column_text(stmt, 0);
                size_t hash_pos = last_user.find_last_of('#');
                if (hash_pos != std::string::npos) {
                    try { next_tag = std::stoi(last_user.substr(hash_pos + 1)) + 1; } catch (...) {}
                }
            }
            sqlite3_finalize(stmt);
        }
        
        char buf[128]; snprintf(buf, sizeof(buf), "%s#%04d", u.c_str(), next_tag);
        std::string final_username = buf;

        bool success = false;
        if (sqlite3_prepare_v2(m_Db, "INSERT INTO users (email, username, password) VALUES (?, ?, ?);", -1, &stmt, 0) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, email.c_str(), -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 2, final_username.c_str(), -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 3, p.c_str(), -1, SQLITE_STATIC);
            success = (sqlite3_step(stmt) == SQLITE_DONE);
            sqlite3_finalize(stmt);
        }
        return success ? final_username : "";
    }

    std::string LoginUser(const std::string& email, const std::string& p) {
        std::lock_guard<std::mutex> lock(m_Mutex);
        sqlite3_stmt* stmt;
        std::string final_username = "";
        if (sqlite3_prepare_v2(m_Db, "SELECT username, password FROM users WHERE email = ?;", -1, &stmt, 0) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, email.c_str(), -1, SQLITE_STATIC);
            if (sqlite3_step(stmt) == SQLITE_ROW) { 
                std::string db_user = (const char*)sqlite3_column_text(stmt, 0);
                std::string db_pass = (const char*)sqlite3_column_text(stmt, 1);
                if (db_pass == p) final_username = db_user; 
            }
            sqlite3_finalize(stmt); 
        }
        return final_username;
    }

    void CreateServer(const std::string& name, const std::string& owner) {
        std::lock_guard<std::mutex> lock(m_Mutex);
        std::string code = GenerateInviteCode(); sqlite3_stmt* stmt;
        if(sqlite3_prepare_v2(m_Db, "INSERT INTO servers (name, invite_code, owner) VALUES (?, ?, ?);", -1, &stmt, 0) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, name.c_str(), -1, SQLITE_STATIC); sqlite3_bind_text(stmt, 2, code.c_str(), -1, SQLITE_STATIC); sqlite3_bind_text(stmt, 3, owner.c_str(), -1, SQLITE_STATIC);
            sqlite3_step(stmt); int serverId = (int)sqlite3_last_insert_rowid(m_Db); sqlite3_finalize(stmt);

            sqlite3_prepare_v2(m_Db, "INSERT INTO server_members (username, server_id) VALUES (?, ?);", -1, &stmt, 0);
            sqlite3_bind_text(stmt, 1, owner.c_str(), -1, SQLITE_STATIC); sqlite3_bind_int(stmt, 2, serverId);
            sqlite3_step(stmt); sqlite3_finalize(stmt);

            const char* sql = "INSERT INTO channels (server_id, name, type) VALUES (?, ?, ?);";
            sqlite3_prepare_v2(m_Db, sql, -1, &stmt, 0); sqlite3_bind_int(stmt, 1, serverId); sqlite3_bind_text(stmt, 2, "general", -1, SQLITE_STATIC); sqlite3_bind_text(stmt, 3, "text", -1, SQLITE_STATIC); sqlite3_step(stmt); sqlite3_finalize(stmt);
            sqlite3_prepare_v2(m_Db, sql, -1, &stmt, 0); sqlite3_bind_int(stmt, 1, serverId); sqlite3_bind_text(stmt, 2, "General", -1, SQLITE_STATIC); sqlite3_bind_text(stmt, 3, "voice", -1, SQLITE_STATIC); sqlite3_step(stmt); sqlite3_finalize(stmt);
        }
    }

    void CreateChannel(int serverId, const std::string& name, const std::string& type) {
        std::lock_guard<std::mutex> lock(m_Mutex); sqlite3_stmt* stmt;
        if(sqlite3_prepare_v2(m_Db, "INSERT INTO channels (server_id, name, type) VALUES (?, ?, ?);", -1, &stmt, 0) == SQLITE_OK) {
            sqlite3_bind_int(stmt, 1, serverId); sqlite3_bind_text(stmt, 2, name.c_str(), -1, SQLITE_STATIC); sqlite3_bind_text(stmt, 3, type.c_str(), -1, SQLITE_STATIC);
            sqlite3_step(stmt); sqlite3_finalize(stmt);
        }
    }

    int JoinServer(const std::string& username, const std::string& code) {
        std::lock_guard<std::mutex> lock(m_Mutex); sqlite3_stmt* stmt;
        int sid = -1;
        if(sqlite3_prepare_v2(m_Db, "SELECT id FROM servers WHERE invite_code = ?;", -1, &stmt, 0) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, code.c_str(), -1, SQLITE_STATIC);
            if (sqlite3_step(stmt) == SQLITE_ROW) sid = sqlite3_column_int(stmt, 0); sqlite3_finalize(stmt);
        }
        if (sid != -1) {
            sqlite3_prepare_v2(m_Db, "INSERT OR IGNORE INTO server_members (username, server_id) VALUES (?, ?);", -1, &stmt, 0);
            sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_STATIC); sqlite3_bind_int(stmt, 2, sid); sqlite3_step(stmt); sqlite3_finalize(stmt);
        }
        return sid;
    }

    std::string GetUserServersJSON(const std::string& username) {
        std::lock_guard<std::mutex> lock(m_Mutex); json j = json::array(); sqlite3_stmt* stmt;
        if(sqlite3_prepare_v2(m_Db, "SELECT s.id, s.name, s.invite_code FROM servers s JOIN server_members m ON s.id = m.server_id WHERE m.username = ?;", -1, &stmt, 0) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_STATIC);
            while (sqlite3_step(stmt) == SQLITE_ROW) j.push_back({ {"id", sqlite3_column_int(stmt, 0)}, {"name", (const char*)sqlite3_column_text(stmt, 1)}, {"code", (const char*)sqlite3_column_text(stmt, 2)} });
            sqlite3_finalize(stmt); 
        }
        return j.dump();
    }

    std::string GetServerContentJSON(int serverId) {
        std::lock_guard<std::mutex> lock(m_Mutex); json j = json::array(); sqlite3_stmt* stmt;
        if(sqlite3_prepare_v2(m_Db, "SELECT id, name, type FROM channels WHERE server_id = ?;", -1, &stmt, 0) == SQLITE_OK) {
            sqlite3_bind_int(stmt, 1, serverId);
            while (sqlite3_step(stmt) == SQLITE_ROW) j.push_back({ {"id", sqlite3_column_int(stmt, 0)}, {"name", (const char*)sqlite3_column_text(stmt, 1)}, {"type", (const char*)sqlite3_column_text(stmt, 2)} });
            sqlite3_finalize(stmt); 
        }
        return j.dump();
    }

    std::string GetMessageHistoryJSON(int channelId) {
        std::lock_guard<std::mutex> lock(m_Mutex); json j = json::array(); sqlite3_stmt* stmt;
        if(sqlite3_prepare_v2(m_Db, "SELECT id, channel_id, sender, content, time FROM messages WHERE channel_id = ? ORDER BY time ASC LIMIT 50;", -1, &stmt, 0) == SQLITE_OK) {
            sqlite3_bind_int(stmt, 1, channelId);
            while (sqlite3_step(stmt) == SQLITE_ROW) j.push_back({ {"mid", sqlite3_column_int(stmt, 0)}, {"cid", sqlite3_column_int(stmt, 1)}, {"u", (const char*)sqlite3_column_text(stmt, 2)}, {"msg", (const char*)sqlite3_column_text(stmt, 3)}, {"time", (const char*)sqlite3_column_text(stmt, 4)} });
            sqlite3_finalize(stmt); 
        }
        return j.dump();
    }

    void SaveMessage(int cid, const std::string& sender, const std::string& msg) {
        std::lock_guard<std::mutex> lock(m_Mutex); sqlite3_stmt* stmt;
        if(sqlite3_prepare_v2(m_Db, "INSERT INTO messages (channel_id, sender, content) VALUES (?, ?, ?);", -1, &stmt, 0) == SQLITE_OK) {
            sqlite3_bind_int(stmt, 1, cid); sqlite3_bind_text(stmt, 2, sender.c_str(), -1, SQLITE_STATIC); sqlite3_bind_text(stmt, 3, msg.c_str(), -1, SQLITE_STATIC);
            sqlite3_step(stmt); sqlite3_finalize(stmt);
        }
    }
    
    bool DeleteMessage(int msgId, const std::string& username) {
        std::lock_guard<std::mutex> lock(m_Mutex); sqlite3_stmt* stmt;
        bool isSender = false;
        if(sqlite3_prepare_v2(m_Db, "SELECT sender FROM messages WHERE id = ?;", -1, &stmt, 0) == SQLITE_OK) {
            sqlite3_bind_int(stmt, 1, msgId);
            if (sqlite3_step(stmt) == SQLITE_ROW) { if ((const char*)sqlite3_column_text(stmt, 0) == username) isSender = true; }
            sqlite3_finalize(stmt);
        }
        if (isSender) {
            if(sqlite3_prepare_v2(m_Db, "DELETE FROM messages WHERE id = ?;", -1, &stmt, 0) == SQLITE_OK) {
                sqlite3_bind_int(stmt, 1, msgId); sqlite3_step(stmt); sqlite3_finalize(stmt); return true;
            }
        }
        return false;
    }

private:
    sqlite3* m_Db;
    std::mutex m_Mutex;
};

class TalkMeServer;

class ChatSession : public std::enable_shared_from_this<ChatSession> {
public:
    ChatSession(tcp::socket socket, TalkMeServer& server) 
        : m_Socket(std::move(socket)), m_Server(server), m_Strand(asio::make_strand(m_Socket.get_executor())) {}

    void Start();
    int GetVoiceChannelId() const { return m_CurrentVoiceCid; }
    const std::string& GetUsername() const { return m_Username; }
    bool IsHealthy() const { return m_IsHealthy; }
    std::chrono::steady_clock::time_point GetLastVoicePacketTime() const { return m_LastVoicePacket; }
    std::chrono::steady_clock::time_point GetLastActivityTime() const { return m_LastActivityTime; }

    void UpdateActivity() {
        m_LastActivityTime = std::chrono::steady_clock::now();
    }

    void TouchVoiceActivity() {
        m_LastVoicePacket = std::chrono::steady_clock::now();
        m_VoicePacketCount = 0;
    }

    void SendShared(std::shared_ptr<std::vector<uint8_t>> buffer, bool isVoiceData) {
        asio::post(m_Strand, [this, self = shared_from_this(), buffer, isVoiceData]() {
            // Proactive congestion control for voice
            if (isVoiceData && m_WriteQueue.size() > 10) {
                while (m_WriteQueue.size() > 5) {
                    m_WriteQueue.pop_front();
                }
            }
            else if (!isVoiceData && m_WriteQueue.size() > 100) {
                return;
            }

            bool writeInProgress = !m_WriteQueue.empty();
            m_WriteQueue.push_back(buffer);
            if (!writeInProgress) DoWrite();
        });
    }

private:
    void ReadHeader();
    void ReadBody();
    void ProcessPacket();
    void DoWrite() {
        if (m_WriteQueue.empty()) return;

        asio::async_write(m_Socket, asio::buffer(*m_WriteQueue.front()),
            asio::bind_executor(m_Strand, [this, self = shared_from_this()](std::error_code ec, std::size_t) {
                if (!ec) { 
                    m_WriteQueue.pop_front(); 
                    if (!m_WriteQueue.empty()) DoWrite(); 
                } else {
                    m_IsHealthy = false;
                    Disconnect(); 
                }
            }));
    }
    void Disconnect();

    tcp::socket m_Socket;
    TalkMeServer& m_Server;
    asio::strand<asio::any_io_executor> m_Strand;
    TalkMe::PacketHeader m_Header;
    std::vector<uint8_t> m_Body;
    std::deque<std::shared_ptr<std::vector<uint8_t>>> m_WriteQueue;
    int m_CurrentVoiceCid = -1;
    std::string m_Username;

    std::chrono::steady_clock::time_point m_LastVoicePacket;
    std::chrono::steady_clock::time_point m_LastActivityTime;
    int m_VoicePacketCount = 0;
    bool m_IsHealthy = true;
};

class TalkMeServer {
public:
    TalkMeServer(asio::io_context& io_context, short port) 
        : m_Acceptor(io_context, tcp::endpoint(tcp::v4(), port)), m_IoContext(io_context) { 
        DoAccept(); 
        StartVoiceOptimizationTimer();
        StartConnectionHealthCheck();
    }

    void JoinClient(std::shared_ptr<ChatSession> session) {
        std::lock_guard<std::mutex> lock(m_RoomMutex);
        m_AllSessions.insert(session);
    }

    void LeaveClient(std::shared_ptr<ChatSession> session) {
        std::lock_guard<std::mutex> lock(m_RoomMutex);
        m_AllSessions.erase(session);
        if (session->GetVoiceChannelId() != -1) {
            m_VoiceChannels[session->GetVoiceChannelId()].erase(session);
        }
    }

    void SetVoiceChannel(std::shared_ptr<ChatSession> session, int newCid, int oldCid) {
        std::lock_guard<std::mutex> lock(m_RoomMutex);
        if (oldCid != -1) {
            m_VoiceChannels[oldCid].erase(session);
            BroadcastVoiceStateLockFree(oldCid);
        }

        if (newCid != -1) {
            auto& channelSessions = m_VoiceChannels[newCid];

            // Duplicate Protection (Ghost Busting)
            for (auto it = channelSessions.begin(); it != channelSessions.end(); ) {
                if ((*it)->GetUsername() == session->GetUsername() && *it != session) {
                    it = channelSessions.erase(it);
                } else {
                    ++it;
                }
            }

            channelSessions.insert(session);
            BroadcastVoiceStateLockFree(newCid);
        }
    }

    void BroadcastToAll(TalkMe::PacketType type, const std::string& data) {
        auto buffer = CreateBuffer(type, data);
        std::lock_guard<std::mutex> lock(m_RoomMutex);
        for (auto& s : m_AllSessions) s->SendShared(buffer, false);
    }

    void BroadcastVoice(int cid, std::shared_ptr<ChatSession> sender, TalkMe::PacketHeader h, const std::vector<uint8_t>& body) {
        auto buffer = CreateBufferRaw(h, body);

        std::lock_guard<std::mutex> lock(m_RoomMutex);
        auto& channelMembers = m_VoiceChannels[cid];

        for (auto& s : channelMembers) {
            if (s != sender && s->GetVoiceChannelId() == cid)
                s->SendShared(buffer, true);
        }
    }

    void SendVoiceConfig(std::shared_ptr<ChatSession> session, int cid) {
        size_t memberCount = 0;
        { std::lock_guard<std::mutex> lock(m_RoomMutex); memberCount = m_VoiceChannels[cid].size(); }
        int targetMs = m_VoiceConfig.jitterBufferTargetMs;
        int minMs = m_VoiceConfig.jitterBufferMinMs;
        int maxMs = m_VoiceConfig.jitterBufferMaxMs;
        if (memberCount <= 2) { targetMs = 80; minMs = 50; maxMs = 150; }
        else if (memberCount <= 4) { targetMs = 120; minMs = 60; maxMs = 200; }
        else { targetMs = 180; minMs = 80; maxMs = 300; }
        json cfg;
        cfg["keepalive_interval_ms"] = m_VoiceConfig.keepaliveIntervalMs;
        cfg["voice_state_request_interval_sec"] = m_VoiceConfig.voiceStateRequestIntervalSec;
        cfg["jitter_buffer_target_ms"] = targetMs;
        cfg["jitter_buffer_min_ms"] = minMs;
        cfg["jitter_buffer_max_ms"] = maxMs;
        auto buffer = CreateBuffer(TalkMe::PacketType::Voice_Config, cfg.dump());
        session->SendShared(buffer, false);
    }

private:
    void BroadcastVoiceStateLockFree(int cid) {
        if (cid == -1) return;
        json members = json::array();
        for (auto& s : m_VoiceChannels[cid]) members.push_back(s->GetUsername());

        json payload; payload["cid"] = cid; payload["members"] = members;
        auto buffer = CreateBuffer(TalkMe::PacketType::Voice_State_Update, payload.dump());

        for (auto& s : m_VoiceChannels[cid]) s->SendShared(buffer, false);
    }

    void StartConnectionHealthCheck() {
        auto timer = std::make_shared<asio::steady_timer>(m_IoContext, std::chrono::seconds(5));
        timer->async_wait([this, timer](const std::error_code& ec) {
            if (!ec) {
                auto now = std::chrono::steady_clock::now();
                std::lock_guard<std::mutex> lock(m_RoomMutex);

                std::vector<std::shared_ptr<ChatSession>> sessionsToRemove;

                for (auto& session : m_AllSessions) {
                    // Check for unhealthy sessions
                    if (!session->IsHealthy()) {
                        sessionsToRemove.push_back(session);
                        continue;
                    }

                    // Check for inactive voice users (no activity for 15 seconds - any packet counts)
                    if (session->GetVoiceChannelId() != -1) {
                        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - session->GetLastActivityTime());
                        if (elapsed.count() > 15) {
                            // Force disconnect from voice channel - user is inactive/ghost
                            int cid = session->GetVoiceChannelId();
                            m_VoiceChannels[cid].erase(session);
                            BroadcastVoiceStateLockFree(cid);
                            // Don't remove from AllSessions - just from voice channel
                            // The client should handle reconnecting
                        }
                    }
                }

                // Remove unhealthy sessions
                for (auto& session : sessionsToRemove) {
                    m_AllSessions.erase(session);
                    int cid = session->GetVoiceChannelId();
                    if (cid != -1) {
                        m_VoiceChannels[cid].erase(session);
                    }
                }

                StartConnectionHealthCheck();
            }
        });
    }

    void StartVoiceOptimizationTimer() {
        auto timer = std::make_shared<asio::steady_timer>(m_IoContext, std::chrono::seconds(30));
        timer->async_wait([this, timer](const std::error_code& ec) {
            if (!ec) {
                std::lock_guard<std::mutex> lock(m_RoomMutex);
                for (auto it = m_VoiceChannels.begin(); it != m_VoiceChannels.end();) {
                    if (it->second.empty()) {
                        it = m_VoiceChannels.erase(it);
                    } else {
                        ++it;
                    }
                }
                StartVoiceOptimizationTimer();
            }
        });
    }

    struct VoiceConfig {
        int keepaliveIntervalMs = 8000;
        int voiceStateRequestIntervalSec = 25;
        int jitterBufferTargetMs = 150;
        int jitterBufferMinMs = 80;
        int jitterBufferMaxMs = 300;
    } m_VoiceConfig;

    std::shared_ptr<std::vector<uint8_t>> CreateBuffer(TalkMe::PacketType type, const std::string& data) {
        uint32_t size = (uint32_t)data.size(); TalkMe::PacketHeader header = { type, size };
        auto buffer = std::make_shared<std::vector<uint8_t>>(sizeof(header) + size);
        std::memcpy(buffer->data(), &header, sizeof(header));
        if (size > 0) std::memcpy(buffer->data() + sizeof(header), data.data(), size);
        return buffer;
    }

    std::shared_ptr<std::vector<uint8_t>> CreateBufferRaw(TalkMe::PacketHeader h, const std::vector<uint8_t>& body) {
        auto buffer = std::make_shared<std::vector<uint8_t>>(sizeof(h) + body.size());
        std::memcpy(buffer->data(), &h, sizeof(h));
        if (!body.empty()) std::memcpy(buffer->data() + sizeof(h), body.data(), body.size());
        return buffer;
    }

    void DoAccept() {
        m_Acceptor.async_accept([this](std::error_code ec, tcp::socket socket) { 
            if (!ec) {
                socket.set_option(tcp::no_delay(true));
                std::make_shared<ChatSession>(std::move(socket), *this)->Start();
            }
            DoAccept(); 
        });
    }

    tcp::acceptor m_Acceptor;
    asio::io_context& m_IoContext;
    std::mutex m_RoomMutex;
    std::unordered_set<std::shared_ptr<ChatSession>> m_AllSessions;
    std::unordered_map<int, std::unordered_set<std::shared_ptr<ChatSession>>> m_VoiceChannels;
};

void ChatSession::Start() { 
    m_LastVoicePacket = std::chrono::steady_clock::now();
    m_Server.JoinClient(shared_from_this()); 
    ReadHeader(); 
}

void ChatSession::Disconnect() { m_Server.LeaveClient(shared_from_this()); if (m_Socket.is_open()) m_Socket.close(); }

void ChatSession::ReadHeader() {
    asio::async_read(m_Socket, asio::buffer(&m_Header, sizeof(TalkMe::PacketHeader)),
        asio::bind_executor(m_Strand, [this, self = shared_from_this()](std::error_code ec, std::size_t) {
            if (!ec) { if (m_Header.size > 10 * 1024 * 1024) { Disconnect(); return; } m_Body.resize(m_Header.size); ReadBody(); } else Disconnect();
        }));
}

void ChatSession::ReadBody() {
    asio::async_read(m_Socket, asio::buffer(m_Body),
        asio::bind_executor(m_Strand, [this, self = shared_from_this()](std::error_code ec, std::size_t) { 
            if (!ec) { ProcessPacket(); ReadHeader(); } else Disconnect(); 
        }));
}

void ChatSession::ProcessPacket() {
    using namespace TalkMe;

    UpdateActivity();

    if (m_Header.type == PacketType::Voice_Data_Opus || m_Header.type == PacketType::Voice_Data) {
        if (m_CurrentVoiceCid != -1) {
            TouchVoiceActivity();

            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_LastVoicePacket);

            if (elapsed.count() < 1000) {
                m_VoicePacketCount++;
                if (m_VoicePacketCount > 100) return;
            } else {
                m_VoicePacketCount = 0;
            }

            m_Server.BroadcastVoice(m_CurrentVoiceCid, shared_from_this(), m_Header, m_Body);
        }
        return;
    }

    auto SendLocal = [this](PacketType type, const std::string& data) {
        uint32_t size = (uint32_t)data.size(); PacketHeader header = { type, size };
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
            if (!new_user.empty()) { m_Username = new_user; json res; res["u"] = new_user; SendLocal(PacketType::Register_Success, res.dump()); }
            else SendLocal(PacketType::Register_Failed, "");
        }
        else if (m_Header.type == PacketType::Login_Request) {
            std::string username = Database::Get().LoginUser(j.value("e", ""), j["p"]);
            if (!username.empty()) { m_Username = username; json res; res["u"] = username; SendLocal(PacketType::Login_Success, res.dump()); SendLocal(PacketType::Server_List_Response, Database::Get().GetUserServersJSON(username)); }
            else SendLocal(PacketType::Login_Failed, "");
        }
        else if (m_Header.type == PacketType::Create_Server_Request) {
            Database::Get().CreateServer(j["name"], j["u"]); SendLocal(PacketType::Server_List_Response, Database::Get().GetUserServersJSON(j["u"]));
        }
        else if (m_Header.type == PacketType::Join_Server_Request) {
            Database::Get().JoinServer(j["u"], j["code"]); SendLocal(PacketType::Server_List_Response, Database::Get().GetUserServersJSON(j["u"]));
        }
        else if (m_Header.type == PacketType::Get_Server_Content_Request) {
            SendLocal(PacketType::Server_Content_Response, Database::Get().GetServerContentJSON(j["sid"]));
        }
        else if (m_Header.type == PacketType::Create_Channel_Request) {
            Database::Get().CreateChannel(j["sid"], j["name"], j["type"]); SendLocal(PacketType::Server_Content_Response, Database::Get().GetServerContentJSON(j["sid"]));
        }
        else if (m_Header.type == PacketType::Select_Text_Channel) {
            SendLocal(PacketType::Message_History_Response, Database::Get().GetMessageHistoryJSON(j["cid"]));
        }
        else if (m_Header.type == PacketType::Join_Voice_Channel) {
            int oldCid = m_CurrentVoiceCid;
            m_CurrentVoiceCid = j["cid"];
            TouchVoiceActivity();
            m_Server.SetVoiceChannel(shared_from_this(), m_CurrentVoiceCid, oldCid);
            if (m_CurrentVoiceCid != -1) m_Server.SendVoiceConfig(shared_from_this(), m_CurrentVoiceCid);
        }
        else if (m_Header.type == PacketType::Delete_Message_Request) {
            if (Database::Get().DeleteMessage(j["mid"], j["u"])) {
                int cid = j["cid"]; std::string history = Database::Get().GetMessageHistoryJSON(cid);
                m_Server.BroadcastToAll(PacketType::Message_History_Response, history);
            }
        }
        else if (m_Header.type == PacketType::Message_Text) {
            int cid = j["cid"]; Database::Get().SaveMessage(cid, j["u"], j["msg"]);
            m_Server.BroadcastToAll(m_Header.type, payload);
        }
    } catch (...) {}
}

int main(int argc, char* argv[]) {
    try {
        asio::io_context io_context; 
        TalkMeServer server(io_context, 5555);
        
        unsigned int thread_count = std::thread::hardware_concurrency();
        if (thread_count == 0) thread_count = 4;
        
        std::cout << "TalkMe Server running on 5555 with " << thread_count << " threads...\n";
        
        std::vector<std::thread> threads;
        for (unsigned int i = 0; i < thread_count; ++i) {
            threads.emplace_back([&io_context]() { io_context.run(); });
        }
        for (auto& t : threads) { t.join(); }
        
    } catch (std::exception& e) { std::cerr << e.what() << "\n"; }
    return 0;
}