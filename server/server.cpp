#include <iostream>
#include <memory>
#include <unordered_set>
#include <unordered_map>
#include <vector>
#include <deque>
#include <array>
#include <mutex>
#include <shared_mutex>
#include <algorithm>
#include <cstring>
#include <random>
#include <thread>
#include <chrono>
#include <cstdlib>
#include <atomic>
#include <condition_variable>
#include <fstream>
#include <functional>
#include <iomanip>
#include <queue>
#include <sstream>
#include <csignal>
#include <asio.hpp>
#include <nlohmann/json.hpp> 
#include <sqlite3.h>
#include "Protocol.h"

using json = nlohmann::json;
using asio::ip::tcp;
using asio::ip::udp;

namespace {
    struct VoiceTrace {
        static std::ofstream s_file;
        static std::mutex s_mutex;
        static bool s_enabled;
        static void init() {
            if (const char* e = std::getenv("VOICE_TRACE")) { s_enabled = (e[0] == '1' || e[0] == 'y' || e[0] == 'Y'); }
            if (s_enabled) { s_file.open("voice_trace.log", std::ios::out | std::ios::trunc); }
        }
        static void log(const std::string& msg) {
            if (!s_enabled || !s_file.is_open()) return;
            std::lock_guard<std::mutex> lock(s_mutex);
            auto now = std::chrono::system_clock::now();
            auto t = std::chrono::system_clock::to_time_t(now);
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
            struct tm tm;
#ifdef _WIN32
            localtime_s(&tm, &t);
#else
            localtime_r(&t, &tm);
#endif
            char buf[32];
            std::strftime(buf, sizeof(buf), "%H:%M:%S", &tm);
            s_file << buf << "." << std::setfill('0') << std::setw(3) << ms.count() << " [TRACE] " << msg << "\n";
            s_file.flush();
        }
    };
    std::ofstream VoiceTrace::s_file;
    std::mutex VoiceTrace::s_mutex;
    bool VoiceTrace::s_enabled = false;
}

namespace {
    constexpr uint8_t kUdpVoicePacket = 0;
    constexpr uint8_t kUdpHelloPacket = 1;
    constexpr uint8_t kUdpPingPacket = 2;
    constexpr uint8_t kUdpPongPacket = 3;
    constexpr size_t kPingPayloadSize = 8;

    struct ParsedVoiceOpus {
        std::string sender;
        std::vector<uint8_t> opus;
        bool valid = false;
    };

    struct AdaptiveVoiceProfile {
        int keepaliveIntervalMs = 4000;
        int voiceStateRequestIntervalSec = 4;
        int jitterTargetMs = 90;
        int jitterMinMs = 50;
        int jitterMaxMs = 160;
        int codecTargetKbps = 48;
        bool preferUdp = true;
    };

    int32_t ReadI32BE(const uint8_t* p) {
        return (static_cast<int32_t>(p[0]) << 24) |
            (static_cast<int32_t>(p[1]) << 16) |
            (static_cast<int32_t>(p[2]) << 8) |
            static_cast<int32_t>(p[3]);
    }

    ParsedVoiceOpus ParseVoicePayloadOpus(const std::vector<uint8_t>& payload) {
        ParsedVoiceOpus out;
        if (payload.size() < 5) return out;
        size_t offset = 4; // sequence
        uint8_t ulen = payload[offset++];
        if (payload.size() < offset + ulen || ulen == 0) return out;
        out.sender.assign(reinterpret_cast<const char*>(payload.data() + offset), ulen);
        offset += ulen;
        if (offset >= payload.size()) return out;
        out.opus.assign(payload.begin() + offset, payload.end());
        out.valid = true;
        return out;
    }

    AdaptiveVoiceProfile BuildVoiceProfile(size_t memberCount) {
        AdaptiveVoiceProfile p;
        // Buffer targets are intentionally generous: the client adaptive formula will reduce
        // the buffer dynamically when jitter is low, but these values serve as safe initial
        // settings that prevent the underrun→rebuffer doom loop on typical LAN/internet links.
        if (memberCount <= 2) {
            p.keepaliveIntervalMs = 2500;
            p.voiceStateRequestIntervalSec = 3;
            p.jitterTargetMs = 150;  // was 70  — must exceed jitter(~40ms)*3 + loss margin
            p.jitterMinMs = 100;     // was 35  — absolute floor; below this underruns are certain
            p.jitterMaxMs = 300;     // was 130
            p.codecTargetKbps = 56;
        }
        else if (memberCount <= 6) {
            p.keepaliveIntervalMs = 3000;
            p.voiceStateRequestIntervalSec = 3;
            p.jitterTargetMs = 180;  // was 90
            p.jitterMinMs = 120;     // was 45
            p.jitterMaxMs = 350;     // was 170
            p.codecTargetKbps = 48;
        }
        else if (memberCount <= 20) {
            p.keepaliveIntervalMs = 3500;
            p.voiceStateRequestIntervalSec = 4;
            p.jitterTargetMs = 220;  // was 120
            p.jitterMinMs = 150;     // was 60
            p.jitterMaxMs = 400;     // was 220
            p.codecTargetKbps = 36;
        }
        else if (memberCount <= 80) {
            p.keepaliveIntervalMs = 4500;
            p.voiceStateRequestIntervalSec = 5;
            p.jitterTargetMs = 260;  // was 160
            p.jitterMinMs = 180;     // was 80
            p.jitterMaxMs = 450;     // was 280
            p.codecTargetKbps = 28;
        }
        else {
            p.keepaliveIntervalMs = 6000;
            p.voiceStateRequestIntervalSec = 6;
            p.jitterTargetMs = 300;  // was 210
            p.jitterMinMs = 200;     // was 110
            p.jitterMaxMs = 500;     // was 380
            p.codecTargetKbps = 20;
        }
        return p;
    }

    std::string EndpointKey(const udp::endpoint& ep) {
        return ep.address().to_string() + ":" + std::to_string(ep.port());
    }
} // namespace

class Database {
public:
    static Database& Get() { static Database db; return db; }

    Database() {
        if (sqlite3_open_v2("talkme.db", &m_Db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK)
            std::cerr << "Can't open DB\n";
        sqlite3_exec(m_Db, "PRAGMA journal_mode=WAL;", 0, 0, 0);
        sqlite3_exec(m_Db, "PRAGMA synchronous=NORMAL;", 0, 0, 0);
        sqlite3_stmt* checkStmt;
        if (sqlite3_prepare_v2(m_Db, "SELECT email FROM users LIMIT 1;", -1, &checkStmt, 0) != SQLITE_OK)
            sqlite3_exec(m_Db, "DROP TABLE IF EXISTS users;", 0, 0, 0);
        sqlite3_finalize(checkStmt);
        const char* sql =
            "CREATE TABLE IF NOT EXISTS users (email TEXT PRIMARY KEY, username TEXT, password TEXT);"
            "CREATE TABLE IF NOT EXISTS servers (id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT, invite_code TEXT UNIQUE, owner TEXT);"
            "CREATE TABLE IF NOT EXISTS channels (id INTEGER PRIMARY KEY AUTOINCREMENT, server_id INTEGER, name TEXT, type TEXT);"
            "CREATE TABLE IF NOT EXISTS server_members (username TEXT, server_id INTEGER, PRIMARY KEY(username, server_id));"
            "CREATE TABLE IF NOT EXISTS messages (id INTEGER PRIMARY KEY AUTOINCREMENT, channel_id INTEGER, sender TEXT, content TEXT, time DATETIME DEFAULT CURRENT_TIMESTAMP);";
        sqlite3_exec(m_Db, sql, 0, 0, 0);
        m_Worker = std::thread(&Database::WorkerLoop, this);
    }

    ~Database() {
        {
            std::lock_guard<std::mutex> lock(m_QueueMutex);
            m_Shutdown = true;
        }
        m_QueueCv.notify_all();
        if (m_Worker.joinable()) m_Worker.join();
        sqlite3_close(m_Db);
    }

    std::string GenerateInviteCode() {
        const char charset[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
        std::default_random_engine rng(std::random_device{}());
        std::uniform_int_distribution<> dist(0, static_cast<int>(sizeof(charset) - 2));
        std::string code;
        for (int i = 0; i < 6; ++i) code += charset[dist(rng)];
        return code;
    }

    std::string RegisterUser(const std::string& email, std::string u, const std::string& p) {
        std::unique_lock<std::shared_mutex> lock(m_RwMutex);
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
                    try { next_tag = std::stoi(last_user.substr(hash_pos + 1)) + 1; }
                    catch (...) {}
                }
            }
            sqlite3_finalize(stmt);
        }
        char buf[128];
        snprintf(buf, sizeof(buf), "%s#%04d", u.c_str(), next_tag);
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
        std::shared_lock<std::shared_mutex> lock(m_RwMutex);
        sqlite3_stmt* stmt;
        std::string final_username;
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
        std::unique_lock<std::shared_mutex> lock(m_RwMutex);
        std::string code = GenerateInviteCode();
        sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(m_Db, "INSERT INTO servers (name, invite_code, owner) VALUES (?, ?, ?);", -1, &stmt, 0) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, name.c_str(), -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 2, code.c_str(), -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 3, owner.c_str(), -1, SQLITE_STATIC);
            sqlite3_step(stmt);
            int serverId = static_cast<int>(sqlite3_last_insert_rowid(m_Db));
            sqlite3_finalize(stmt);
            sqlite3_prepare_v2(m_Db, "INSERT INTO server_members (username, server_id) VALUES (?, ?);", -1, &stmt, 0);
            sqlite3_bind_text(stmt, 1, owner.c_str(), -1, SQLITE_STATIC);
            sqlite3_bind_int(stmt, 2, serverId);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
            const char* sql = "INSERT INTO channels (server_id, name, type) VALUES (?, ?, ?);";
            sqlite3_prepare_v2(m_Db, sql, -1, &stmt, 0);
            sqlite3_bind_int(stmt, 1, serverId);
            sqlite3_bind_text(stmt, 2, "general", -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 3, "text", -1, SQLITE_STATIC);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
            sqlite3_prepare_v2(m_Db, sql, -1, &stmt, 0);
            sqlite3_bind_int(stmt, 1, serverId);
            sqlite3_bind_text(stmt, 2, "General", -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 3, "voice", -1, SQLITE_STATIC);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
    }

    void CreateChannel(int serverId, const std::string& name, const std::string& type) {
        std::unique_lock<std::shared_mutex> lock(m_RwMutex);
        sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(m_Db, "INSERT INTO channels (server_id, name, type) VALUES (?, ?, ?);", -1, &stmt, 0) == SQLITE_OK) {
            sqlite3_bind_int(stmt, 1, serverId);
            sqlite3_bind_text(stmt, 2, name.c_str(), -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 3, type.c_str(), -1, SQLITE_STATIC);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
    }

    int JoinServer(const std::string& username, const std::string& code) {
        std::unique_lock<std::shared_mutex> lock(m_RwMutex);
        sqlite3_stmt* stmt;
        int sid = -1;
        if (sqlite3_prepare_v2(m_Db, "SELECT id FROM servers WHERE invite_code = ?;", -1, &stmt, 0) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, code.c_str(), -1, SQLITE_STATIC);
            if (sqlite3_step(stmt) == SQLITE_ROW) sid = sqlite3_column_int(stmt, 0);
            sqlite3_finalize(stmt);
        }
        if (sid != -1) {
            sqlite3_prepare_v2(m_Db, "INSERT OR IGNORE INTO server_members (username, server_id) VALUES (?, ?);", -1, &stmt, 0);
            sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_STATIC);
            sqlite3_bind_int(stmt, 2, sid);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
        return sid;
    }

    std::string GetUserServersJSON(const std::string& username) {
        std::shared_lock<std::shared_mutex> lock(m_RwMutex);
        json j = json::array();
        sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(m_Db, "SELECT s.id, s.name, s.invite_code FROM servers s JOIN server_members m ON s.id = m.server_id WHERE m.username = ?;", -1, &stmt, 0) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_STATIC);
            while (sqlite3_step(stmt) == SQLITE_ROW)
                j.push_back({{"id", sqlite3_column_int(stmt, 0)}, {"name", (const char*)sqlite3_column_text(stmt, 1)}, {"code", (const char*)sqlite3_column_text(stmt, 2)}});
            sqlite3_finalize(stmt);
        }
        return j.dump();
    }

    std::string GetServerContentJSON(int serverId) {
        std::shared_lock<std::shared_mutex> lock(m_RwMutex);
        json j = json::array();
        sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(m_Db, "SELECT id, name, type FROM channels WHERE server_id = ?;", -1, &stmt, 0) == SQLITE_OK) {
            sqlite3_bind_int(stmt, 1, serverId);
            while (sqlite3_step(stmt) == SQLITE_ROW)
                j.push_back({{"id", sqlite3_column_int(stmt, 0)}, {"name", (const char*)sqlite3_column_text(stmt, 1)}, {"type", (const char*)sqlite3_column_text(stmt, 2)}});
            sqlite3_finalize(stmt);
        }
        return j.dump();
    }

    std::string GetMessageHistoryJSON(int channelId) {
        std::shared_lock<std::shared_mutex> lock(m_RwMutex);
        json j = json::array();
        sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(m_Db, "SELECT id, channel_id, sender, content, time FROM messages WHERE channel_id = ? ORDER BY time ASC LIMIT 50;", -1, &stmt, 0) == SQLITE_OK) {
            sqlite3_bind_int(stmt, 1, channelId);
            while (sqlite3_step(stmt) == SQLITE_ROW)
                j.push_back({{"mid", sqlite3_column_int(stmt, 0)}, {"cid", sqlite3_column_int(stmt, 1)}, {"u", (const char*)sqlite3_column_text(stmt, 2)}, {"msg", (const char*)sqlite3_column_text(stmt, 3)}, {"time", (const char*)sqlite3_column_text(stmt, 4)}});
            sqlite3_finalize(stmt);
        }
        return j.dump();
    }

    void SaveMessage(int cid, const std::string& sender, const std::string& msg) {
        int ch = cid;
        std::string s = sender;
        std::string m = msg;
        Enqueue([this, ch, s, m]() {
            std::unique_lock<std::shared_mutex> lock(m_RwMutex);
            sqlite3_stmt* stmt;
            if (sqlite3_prepare_v2(m_Db, "INSERT INTO messages (channel_id, sender, content) VALUES (?, ?, ?);", -1, &stmt, 0) == SQLITE_OK) {
                sqlite3_bind_int(stmt, 1, ch);
                sqlite3_bind_text(stmt, 2, s.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 3, m.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_step(stmt);
                sqlite3_finalize(stmt);
            }
        });
    }

    bool DeleteMessage(int msgId, const std::string& username) {
        std::unique_lock<std::shared_mutex> lock(m_RwMutex);
        sqlite3_stmt* stmt;
        bool isSender = false;
        if (sqlite3_prepare_v2(m_Db, "SELECT sender FROM messages WHERE id = ?;", -1, &stmt, 0) == SQLITE_OK) {
            sqlite3_bind_int(stmt, 1, msgId);
            if (sqlite3_step(stmt) == SQLITE_ROW)
                isSender = ((std::string((const char*)sqlite3_column_text(stmt, 0)) == username));
            sqlite3_finalize(stmt);
        }
        if (isSender && sqlite3_prepare_v2(m_Db, "DELETE FROM messages WHERE id = ?;", -1, &stmt, 0) == SQLITE_OK) {
            sqlite3_bind_int(stmt, 1, msgId);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
            return true;
        }
        return false;
    }

private:
    void Enqueue(std::function<void()> task) {
        if (!task) return;
        {
            std::lock_guard<std::mutex> lock(m_QueueMutex);
            if (m_Shutdown) return;
            m_TaskQueue.push(std::move(task));
        }
        m_QueueCv.notify_one();
    }

    void WorkerLoop() {
        while (true) {
            std::unique_lock<std::mutex> lock(m_QueueMutex);
            m_QueueCv.wait(lock, [this] { return m_Shutdown || !m_TaskQueue.empty(); });
            if (m_Shutdown && m_TaskQueue.empty()) break;
            if (m_TaskQueue.empty()) continue;
            auto task = std::move(m_TaskQueue.front());
            m_TaskQueue.pop();
            lock.unlock();
            task();
        }
    }

    sqlite3* m_Db;
    std::shared_mutex m_RwMutex;
    std::thread m_Worker;
    std::mutex m_QueueMutex;
    std::condition_variable m_QueueCv;
    std::queue<std::function<void()>> m_TaskQueue;
    bool m_Shutdown = false;
};

class TalkMeServer;

class ChatSession : public std::enable_shared_from_this<ChatSession> {
public:
    ChatSession(tcp::socket socket, TalkMeServer& server)
        : m_Socket(std::move(socket)), m_Server(server), m_Strand(asio::make_strand(m_Socket.get_executor())) {
    }

    void Start();
    int GetVoiceChannelId() const { return m_CurrentVoiceCid; }
    const std::string& GetUsername() const { return m_Username; }
    bool IsHealthy() const { return m_IsHealthy; }
    std::chrono::steady_clock::time_point GetLastVoicePacketTime() const { return m_LastVoicePacket; }
    std::chrono::steady_clock::time_point GetLastActivityTime() const { return m_LastActivityTime; }
    void SetVoiceLoad(size_t load) { m_CurrentVoiceLoad = std::max<size_t>(1, load); }

    void UpdateActivity() {
        m_LastActivityTime = std::chrono::steady_clock::now();
    }

    void TouchVoiceActivity() {
        m_LastVoicePacket = std::chrono::steady_clock::now();
        m_VoicePacketCount = 0;
    }

    void SendShared(std::shared_ptr<std::vector<uint8_t>> buffer, bool isVoiceData) {
        asio::post(m_Strand, [this, self = shared_from_this(), buffer, isVoiceData]() {
            // Voice queue: only drop under real congestion. For small calls (<=4) never drop.
            size_t voiceDropAt = 0;   // 0 = disabled
            size_t voiceKeepAt = 0;
            if (m_CurrentVoiceLoad > 80) { voiceDropAt = 24; voiceKeepAt = 12; }
            else if (m_CurrentVoiceLoad > 30) { voiceDropAt = 48; voiceKeepAt = 24; }
            else if (m_CurrentVoiceLoad > 8) { voiceDropAt = 64; voiceKeepAt = 32; }
            else if (m_CurrentVoiceLoad > 4) { voiceDropAt = 96; voiceKeepAt = 48; }
            if (isVoiceData && voiceDropAt > 0 && m_WriteQueue.size() > voiceDropAt) {
                while (m_WriteQueue.size() > voiceKeepAt) {
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
                }
                else {
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
    size_t m_CurrentVoiceLoad = 1;

    double m_LastJitterMs = 0.0;
    int m_ConsecutiveStableReports = 0;
    uint32_t m_CurrentAssignedBitrateKbps = 48;
};

class TalkMeServer {
public:
    TalkMeServer(asio::io_context& io_context, short port)
        : m_Acceptor(io_context, tcp::endpoint(tcp::v4(), port)),
        m_VoiceUdpSocket(io_context, udp::endpoint(udp::v4(), TalkMe::VOICE_PORT)),
        m_IoContext(io_context) {
        DoAccept();
        StartVoiceUdpReceive();
        StartVoiceOptimizationTimer();
        StartConnectionHealthCheck();
        StartVoiceStatsWriteTimer();
    }

    void RecordVoiceStats(const std::string& username, int cid, float ping_ms, float loss_pct, float jitter_ms, int buffer_ms) {
        if (username.empty() || cid < 0) return;
        std::lock_guard<std::mutex> lock(m_StatsMutex);
        m_LastVoiceStats[username] = { ping_ms, loss_pct, jitter_ms, buffer_ms, cid };
    }

    uint32_t GetChannelBitrateLimit(int cid) {
        std::lock_guard<std::mutex> lock(m_SpeakerMutex);
        auto it = m_RecentSpeakersByChannel.find(cid);
        size_t activeCount = (it != m_RecentSpeakersByChannel.end() && !it->second.empty()) ? it->second.size() : 1;
        uint32_t globalBudgetKbps = 128;
        uint32_t perSpeakerLimit = globalBudgetKbps / static_cast<uint32_t>(activeCount);
        return (std::max)(12u, (std::min)(64u, perSpeakerLimit));
    }

    void JoinClient(std::shared_ptr<ChatSession> session) {
        std::unique_lock<std::shared_mutex> lock(m_RoomMutex);
        m_AllSessions.insert(session);
    }

    void LeaveClient(std::shared_ptr<ChatSession> session) {
        std::unique_lock<std::shared_mutex> lock(m_RoomMutex);
        m_AllSessions.erase(session);
        std::string username = session->GetUsername();
        if (!username.empty()) m_UdpBindings.erase(username);
        if (session->GetVoiceChannelId() != -1) {
            int cid = session->GetVoiceChannelId();
            m_VoiceChannels[cid].erase(session);
            RefreshChannelControlLockFree(cid, username, false);
        }
    }

    void SetVoiceChannel(std::shared_ptr<ChatSession> session, int newCid, int oldCid) {
        std::unique_lock<std::shared_mutex> lock(m_RoomMutex);
        // Only leave old channel (and clear UDP binding) when actually changing or leaving, not on periodic re-join of same channel.
        if (oldCid != -1 && oldCid != newCid) {
            std::string username = session->GetUsername();
            m_VoiceChannels[oldCid].erase(session);
            if (!username.empty()) m_UdpBindings.erase(username);
            RefreshChannelControlLockFree(oldCid, username, false);
        }

        if (newCid != -1) {
            auto& channelSessions = m_VoiceChannels[newCid];
            std::string username = session->GetUsername();
            for (auto it = channelSessions.begin(); it != channelSessions.end(); ) {
                if ((*it)->GetUsername() == username && *it != session) {
                    it = channelSessions.erase(it);
                }
                else ++it;
            }
            channelSessions.insert(session);
            RefreshChannelControlLockFree(newCid, username, true);
        }
    }

    void BroadcastToAll(TalkMe::PacketType type, const std::string& data) {
        auto buffer = CreateBuffer(type, data);
        std::shared_lock<std::shared_mutex> lock(m_RoomMutex);
        for (auto& s : m_AllSessions) s->SendShared(buffer, false);
    }

    void BroadcastVoice(int cid, std::shared_ptr<ChatSession> sender, TalkMe::PacketHeader h, const std::vector<uint8_t>& body) {
        auto buffer = CreateBufferRaw(h, body);
        std::shared_lock<std::shared_mutex> lock(m_RoomMutex);
        auto& channelMembers = m_VoiceChannels[cid];
        for (auto& s : channelMembers) {
            if (s != sender && s->GetVoiceChannelId() == cid)
                s->SendShared(buffer, true);
        }
    }

    void HandleVoiceUdpPacket(const std::vector<uint8_t>& packet, const udp::endpoint& from) {
        if (packet.empty()) return;
        uint8_t kind = packet[0];

        // Link probe echo: client sends [0xEE][seq:4][timestamp_us:8] = 13 bytes.
        // Server echoes immediately, unchanged.  No auth required — the probe runs
        // before the Hello handshake and the packet is too small to be a voice frame.
        if (kind == 0xEE && packet.size() == 13) {
            m_VoiceUdpSocket.async_send_to(
                asio::buffer(packet), from,
                [](const std::error_code&, std::size_t) {});
            return;
        }

        if (kind == kUdpPingPacket && packet.size() >= 1 + kPingPayloadSize) {
            std::vector<uint8_t> pong;
            pong.reserve(1 + kPingPayloadSize);
            pong.push_back(kUdpPongPacket);
            pong.insert(pong.end(), packet.begin() + 1, packet.begin() + 1 + kPingPayloadSize);
            m_VoiceUdpSocket.async_send_to(asio::buffer(pong), from, [](const std::error_code&, std::size_t) {});
            return;
        }

        if (kind == kUdpHelloPacket) {
            if (packet.size() < 1 + 1 + 4) return;
            size_t offset = 1;
            uint8_t ulen = packet[offset++];
            if (packet.size() < offset + ulen + 4 || ulen == 0) return;
            std::string username(reinterpret_cast<const char*>(packet.data() + offset), ulen);
            offset += ulen;
            int32_t voiceCid = ReadI32BE(packet.data() + offset);

            std::unique_lock<std::shared_mutex> lock(m_RoomMutex);
            if (voiceCid < 0 || username.empty()) {
                m_UdpBindings.erase(username);
                VoiceTrace::log("step=udp_hello_drop reason=invalid_cid_or_user user=" + username + " cid=" + std::to_string(voiceCid));
                return;
            }
            auto sit = std::find_if(m_AllSessions.begin(), m_AllSessions.end(),
                [&username](const std::shared_ptr<ChatSession>& s) { return s->GetUsername() == username; });
            if (sit == m_AllSessions.end()) {
                VoiceTrace::log("step=udp_hello_drop reason=session_not_found user=" + username);
                return;
            }
            if ((*sit)->GetVoiceChannelId() != voiceCid) {
                VoiceTrace::log("step=udp_hello_drop reason=channel_mismatch user=" + username + " cid=" + std::to_string(voiceCid));
                return;
            }
            auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
            auto& binding = m_UdpBindings[username];
            binding.endpoint = from;
            binding.lastSeenMs.store(nowMs);
            binding.voiceCid = voiceCid;
            VoiceTrace::log("step=udp_hello_ok user=" + username + " cid=" + std::to_string(voiceCid) + " bindings=" + std::to_string(m_UdpBindings.size()));
            return;
        }

        if (kind != kUdpVoicePacket) return;
        if (packet.size() < 2) return;
        std::vector<uint8_t> voicePayload(packet.begin() + 1, packet.end());
        auto parsed = ParseVoicePayloadOpus(voicePayload);
        if (!parsed.valid || parsed.sender.empty()) {
            VoiceTrace::log("step=server_drop reason=parse_fail size=" + std::to_string(packet.size()) + " relay_ok=0 quality_pct=0");
            return;
        }
        static std::atomic<uint32_t> s_recvCount{ 0 };
        if (++s_recvCount <= 10 || (s_recvCount % 50) == 0)
            VoiceTrace::log(
                "step=server_recv sender=" + parsed.sender +
                " payload_bytes=" + std::to_string(voicePayload.size()) +
                " parse_ok=1 quality_pct=100");

        std::vector<udp::endpoint> udpTargets;
        std::vector<std::shared_ptr<ChatSession>> tcpFallback;
        int cid = -1;
        auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        {
            std::shared_lock<std::shared_mutex> lock(m_RoomMutex);
            auto senderIt = m_UdpBindings.find(parsed.sender);
            if (senderIt == m_UdpBindings.end()) {
                VoiceTrace::log("step=server_drop reason=sender_not_bound sender=" + parsed.sender + " bindings=" + std::to_string(m_UdpBindings.size()) + " relay_ok=0 quality_pct=0");
                return;
            }
            if (EndpointKey(senderIt->second.endpoint) != EndpointKey(from)) {
                VoiceTrace::log("step=server_drop reason=endpoint_mismatch sender=" + parsed.sender + " relay_ok=0 quality_pct=0");
                return;
            }
            auto sit = std::find_if(m_AllSessions.begin(), m_AllSessions.end(),
                [&parsed](const std::shared_ptr<ChatSession>& s) { return s->GetUsername() == parsed.sender; });
            if (sit == m_AllSessions.end()) return;
            cid = (*sit)->GetVoiceChannelId();
            if (cid < 0) return;

            senderIt->second.lastSeenMs.store(nowMs);
            senderIt->second.voiceCid = cid;

            auto& members = m_VoiceChannels[cid];
            {
                std::lock_guard<std::mutex> speakerLock(m_SpeakerMutex);
                auto& recent = m_RecentSpeakersByChannel[cid];
                if (members.size() > kActiveSpeakerChannelThreshold) {
                    bool inRecent = std::find(recent.begin(), recent.end(), parsed.sender) != recent.end();
                    if (!inRecent && recent.size() >= kActiveSpeakerMax) return;
                }
                recent.erase(std::remove(recent.begin(), recent.end(), parsed.sender), recent.end());
                recent.push_front(parsed.sender);
                while (recent.size() > kActiveSpeakerMax) recent.pop_back();
            }

            constexpr int64_t kActiveSec = 15;
            const int64_t cutoffMs = nowMs - kActiveSec * 1000;
            for (const auto& s : members) {
                if (!s || s->GetUsername() == parsed.sender) continue;
                tcpFallback.push_back(s);
                auto bit = m_UdpBindings.find(s->GetUsername());
                if (bit != m_UdpBindings.end() && bit->second.voiceCid == cid && bit->second.lastSeenMs.load(std::memory_order_relaxed) >= cutoffMs)
                    udpTargets.push_back(bit->second.endpoint);
            }
        }

        static std::atomic<uint32_t> s_udpVoiceCount{ 0 };
        uint32_t n = ++s_udpVoiceCount;
        if (n <= 10 || (n % 50) == 0)
            VoiceTrace::log(
                "step=server_relay sender=" + parsed.sender +
                " cid=" + std::to_string(cid) +
                " relay_tcp=" + std::to_string(tcpFallback.size()) +
                " relay_udp=" + std::to_string(udpTargets.size()) +
                " payload_bytes=" + std::to_string(voicePayload.size()) +
                " relay_ok=1 quality_pct=100");

        TalkMe::PacketHeader h{ TalkMe::PacketType::Voice_Data_Opus, static_cast<uint32_t>(voicePayload.size()) };
        auto tcpBuffer = CreateBufferRaw(h, voicePayload);
        for (const auto& s : tcpFallback) s->SendShared(tcpBuffer, true);

        if (!udpTargets.empty()) {
            std::vector<uint8_t> udpPacket;
            udpPacket.reserve(voicePayload.size() + 1);
            udpPacket.push_back(kUdpVoicePacket);
            udpPacket.insert(udpPacket.end(), voicePayload.begin(), voicePayload.end());
            auto sharedUdpPacket = std::make_shared<std::vector<uint8_t>>(std::move(udpPacket));
            for (const auto& ep : udpTargets) {
                m_VoiceUdpSocket.async_send_to(asio::buffer(*sharedUdpPacket), ep,
                    [sharedUdpPacket](const std::error_code&, std::size_t) {});
            }
        }
    }

private:
    void RefreshChannelControlLockFree(int cid, const std::string& targetUser = "", bool isJoin = true) {
        if (cid == -1) return;
        auto& membersSet = m_VoiceChannels[cid];
        size_t memberCount = membersSet.size();
        auto profile = BuildVoiceProfile(memberCount);
        for (auto& s : membersSet) s->SetVoiceLoad(memberCount);

        json cfg;
        cfg["keepalive_interval_ms"] = profile.keepaliveIntervalMs;
        cfg["voice_state_request_interval_sec"] = profile.voiceStateRequestIntervalSec;
        cfg["jitter_buffer_target_ms"] = profile.jitterTargetMs;
        cfg["jitter_buffer_min_ms"] = profile.jitterMinMs;
        cfg["jitter_buffer_max_ms"] = profile.jitterMaxMs;
        cfg["codec_target_kbps"] = profile.codecTargetKbps;
        cfg["prefer_udp"] = profile.preferUdp;
        cfg["server_version"] = "1.1";
        auto cfgBuffer = CreateBuffer(TalkMe::PacketType::Voice_Config, cfg.dump());

        if (!targetUser.empty()) {
            json deltaPayload;
            deltaPayload["cid"] = cid;
            deltaPayload["u"] = targetUser;
            deltaPayload["action"] = isJoin ? "join" : "leave";
            auto deltaBuffer = CreateBuffer(TalkMe::PacketType::Voice_State_Update, deltaPayload.dump());
            if (isJoin) {
                json members = json::array();
                for (auto& s : membersSet) members.push_back(s->GetUsername());
                json fullPayload;
                fullPayload["cid"] = cid;
                fullPayload["members"] = members;
                auto fullStateBuffer = CreateBuffer(TalkMe::PacketType::Voice_State_Update, fullPayload.dump());
                for (auto& s : membersSet) {
                    if (s->GetUsername() == targetUser)
                        s->SendShared(fullStateBuffer, false);
                    else
                        s->SendShared(deltaBuffer, false);
                    s->SendShared(cfgBuffer, false);
                }
            }
            else {
                for (auto& s : membersSet) {
                    s->SendShared(deltaBuffer, false);
                    s->SendShared(cfgBuffer, false);
                }
            }
        }
        else {
            json members = json::array();
            for (auto& s : membersSet) members.push_back(s->GetUsername());
            json payload;
            payload["cid"] = cid;
            payload["members"] = members;
            auto stateBuffer = CreateBuffer(TalkMe::PacketType::Voice_State_Update, payload.dump());
            for (auto& s : membersSet) {
                s->SendShared(stateBuffer, false);
                s->SendShared(cfgBuffer, false);
            }
        }
    }

    void StartVoiceUdpReceive() {
        m_VoiceUdpSocket.async_receive_from(
            asio::buffer(m_VoiceRecvBuffer), m_VoiceRecvFrom,
            [this](const std::error_code& ec, std::size_t bytes) {
                if (!ec && bytes > 0) {
                    std::vector<uint8_t> packet(m_VoiceRecvBuffer.begin(), m_VoiceRecvBuffer.begin() + bytes);
                    auto from = m_VoiceRecvFrom;
                    HandleVoiceUdpPacket(packet, from);
                }
                StartVoiceUdpReceive();
            });
    }

    void StartConnectionHealthCheck() {
        auto timer = std::make_shared<asio::steady_timer>(m_IoContext, std::chrono::seconds(5));
        timer->async_wait([this, timer](const std::error_code& ec) {
            if (!ec) {
                auto now = std::chrono::steady_clock::now();
                auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
                std::unique_lock<std::shared_mutex> lock(m_RoomMutex);

                std::vector<std::shared_ptr<ChatSession>> sessionsToRemove;

                for (auto& session : m_AllSessions) {
                    if (!session->IsHealthy()) {
                        sessionsToRemove.push_back(session);
                        continue;
                    }
                    if (session->GetVoiceChannelId() != -1) {
                        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - session->GetLastActivityTime());
                        if (elapsed.count() > 15) {
                            int cid = session->GetVoiceChannelId();
                            m_VoiceChannels[cid].erase(session);
                            RefreshChannelControlLockFree(cid);
                        }
                    }
                }

                const int64_t cutoffMs = nowMs - 30 * 1000;
                for (auto it = m_UdpBindings.begin(); it != m_UdpBindings.end();) {
                    if (it->second.lastSeenMs.load(std::memory_order_relaxed) < cutoffMs || it->second.voiceCid < 0)
                        it = m_UdpBindings.erase(it);
                    else ++it;
                }

                for (auto& session : sessionsToRemove) {
                    m_AllSessions.erase(session);
                    int cid = session->GetVoiceChannelId();
                    if (cid != -1) {
                        m_VoiceChannels[cid].erase(session);
                        RefreshChannelControlLockFree(cid);
                    }
                    m_UdpBindings.erase(session->GetUsername());
                }

                StartConnectionHealthCheck();
            }
            });
    }

    void StartVoiceOptimizationTimer() {
        auto timer = std::make_shared<asio::steady_timer>(m_IoContext, std::chrono::seconds(30));
        timer->async_wait([this, timer](const std::error_code& ec) {
            if (!ec) {
                std::unique_lock<std::shared_mutex> lock(m_RoomMutex);
                std::lock_guard<std::mutex> speakerLock(m_SpeakerMutex);
                for (auto it = m_VoiceChannels.begin(); it != m_VoiceChannels.end();) {
                    if (it->second.empty()) {
                        int cid = it->first;
                        it = m_VoiceChannels.erase(it);
                        m_RecentSpeakersByChannel.erase(cid);
                    }
                    else ++it;
                }
                StartVoiceOptimizationTimer();
            }
            });
    }

    void StartVoiceStatsWriteTimer() {
        auto timer = std::make_shared<asio::steady_timer>(m_IoContext, std::chrono::seconds(10));
        timer->async_wait([this, timer](const std::error_code& ec) {
            if (!ec) {
                AggVoiceSample sample;
                sample.ts = std::chrono::system_clock::now().time_since_epoch() / std::chrono::seconds(1);
                {
                    std::lock_guard<std::mutex> lock(m_StatsMutex);
                    if (!m_LastVoiceStats.empty()) {
                        float sum_ping = 0, sum_loss = 0, sum_jitter = 0;
                        int sum_buffer = 0;
                        for (const auto& p : m_LastVoiceStats) {
                            sum_ping += p.second.ping_ms;
                            sum_loss += p.second.loss_pct;
                            sum_jitter += p.second.jitter_ms;
                            sum_buffer += p.second.buffer_ms;
                        }
                        size_t n = m_LastVoiceStats.size();
                        sample.avg_ping_ms = sum_ping / n;
                        sample.avg_loss_pct = sum_loss / n;
                        sample.avg_jitter_ms = sum_jitter / n;
                        sample.avg_buffer_ms = (int)(sum_buffer / (int)n);
                        sample.clients = (int)n;
                        m_VoiceStatsHistory.push_back(sample);
                        while (m_VoiceStatsHistory.size() > kMaxVoiceStatsSamples)
                            m_VoiceStatsHistory.pop_front();
                    }
                }
                {
                    json arr = json::array();
                    std::lock_guard<std::mutex> lock(m_StatsMutex);
                    for (const auto& s : m_VoiceStatsHistory) {
                        arr.push_back({ {"ts", s.ts}, {"avg_ping_ms", s.avg_ping_ms}, {"avg_loss_pct", s.avg_loss_pct},
                                        {"avg_jitter_ms", s.avg_jitter_ms}, {"avg_buffer_ms", s.avg_buffer_ms}, {"clients", s.clients} });
                    }
                    json out; out["samples"] = arr;
                    std::ofstream f("voice_stats.json", std::ios::out);
                    if (f) f << out.dump();
                }
                StartVoiceStatsWriteTimer();
            }
            });
    }

    std::shared_ptr<std::vector<uint8_t>> CreateBuffer(TalkMe::PacketType type, const std::string& data) {
        uint32_t size = (uint32_t)data.size();
        TalkMe::PacketHeader header = { type, size };
        header.ToNetwork();
        auto buffer = std::make_shared<std::vector<uint8_t>>(sizeof(header) + size);
        std::memcpy(buffer->data(), &header, sizeof(header));
        if (size > 0) std::memcpy(buffer->data() + sizeof(header), data.data(), size);
        return buffer;
    }

    std::shared_ptr<std::vector<uint8_t>> CreateBufferRaw(TalkMe::PacketHeader h, const std::vector<uint8_t>& body) {
        h.ToNetwork();
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
    udp::socket m_VoiceUdpSocket;
    std::array<uint8_t, 65536> m_VoiceRecvBuffer{};
    udp::endpoint m_VoiceRecvFrom;
    asio::io_context& m_IoContext;
    std::shared_mutex m_RoomMutex;
    std::mutex m_SpeakerMutex;
    std::unordered_set<std::shared_ptr<ChatSession>> m_AllSessions;
    std::unordered_map<int, std::unordered_set<std::shared_ptr<ChatSession>>> m_VoiceChannels;
    struct UdpBinding {
        udp::endpoint endpoint;
        std::atomic<int64_t> lastSeenMs{0};  // milliseconds since steady_clock epoch
        int voiceCid = -1;
    };
    std::unordered_map<std::string, UdpBinding> m_UdpBindings;
    static constexpr size_t kActiveSpeakerMax = 8;
    static constexpr size_t kActiveSpeakerChannelThreshold = 20;
    std::unordered_map<int, std::deque<std::string>> m_RecentSpeakersByChannel;

    struct VoiceClientStats { float ping_ms = 0, loss_pct = 0, jitter_ms = 0; int buffer_ms = 0; int cid = -1; };
    std::mutex m_StatsMutex;
    std::unordered_map<std::string, VoiceClientStats> m_LastVoiceStats;
    struct AggVoiceSample { int64_t ts = 0; float avg_ping_ms = 0, avg_loss_pct = 0, avg_jitter_ms = 0; int avg_buffer_ms = 0; int clients = 0; };
    std::deque<AggVoiceSample> m_VoiceStatsHistory;
    static constexpr size_t kMaxVoiceStatsSamples = 200;
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
            if (!ec) {
                m_Header.ToHost();
                if (m_Header.size > 10 * 1024 * 1024) { Disconnect(); return; }
                m_Body.resize(m_Header.size);
                ReadBody();
            }
            else Disconnect();
            }));
}

void ChatSession::ReadBody() {
    asio::async_read(m_Socket, asio::buffer(m_Body),
        asio::bind_executor(m_Strand, [this, self = shared_from_this()](std::error_code ec, std::size_t) {
            if (!ec) { ProcessPacket(); ReadHeader(); }
            else Disconnect();
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
            }
            else {
                m_VoicePacketCount = 0;
            }

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

    // Intercept binary telemetry before JSON parsing
    if (m_Header.type == PacketType::Receiver_Report) {
        if (m_Body.size() >= sizeof(ReceiverReportPayload)) {
            ReceiverReportPayload rr;
            std::memcpy(&rr, m_Body.data(), sizeof(rr));
            rr.ToHost();

            SenderReportPayload sr{};
            double jitterGradient = static_cast<double>(rr.jitterMs) - m_LastJitterMs;
            m_LastJitterMs = rr.jitterMs;

            // Multiplicative Decrease: Cut bitrate immediately on loss or sharp delay spikes (bufferbloat)
            if (rr.fractionLost > 10 || jitterGradient > 30.0) {
                m_CurrentAssignedBitrateKbps = std::max(16u, m_CurrentAssignedBitrateKbps / 2);
                m_ConsecutiveStableReports = 0;
                sr.networkState = 2; // Congested
            }
            // Additive Increase: Probe for more bandwidth after sustained stability
            else if (rr.fractionLost == 0 && jitterGradient < 10.0 && rr.jitterMs < 60) {
                m_ConsecutiveStableReports++;
                if (m_ConsecutiveStableReports >= 3) {
                    m_CurrentAssignedBitrateKbps = std::min(64u, m_CurrentAssignedBitrateKbps + 4);
                    m_ConsecutiveStableReports = 0;
                }
                sr.networkState = 0; // Excellent
            }
            // Hold: Network is fluctuating mildly, maintain current bitrate
            else {
                m_ConsecutiveStableReports = 0;
                sr.networkState = 1; // Good
            }

            // Phase 5: SFU-Driven Global Bandwidth Budgeting
            // Clamp the client's requested upstream bitrate by the server's global channel limit
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
        else if (m_Header.type == PacketType::Voice_Stats_Report) {
            m_Server.RecordVoiceStats(m_Username, j.value("cid", -1),
                j.value("ping_ms", 0.0), j.value("loss_pct", 0.0),
                j.value("jitter_ms", 0.0), j.value("buffer_ms", 0));
        }
    }
    catch (const json::exception& e) {
        VoiceTrace::log(std::string("step=json_error msg=") + e.what());
    }
    catch (const std::exception& e) {
        VoiceTrace::log(std::string("step=std_error msg=") + e.what());
    }
    catch (...) {
        VoiceTrace::log("step=unknown_error msg=unknown_exception_caught");
    }
}

int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;
    try {
        VoiceTrace::init();
        asio::io_context io_context;
        TalkMeServer server(io_context, 5555);

        asio::signal_set signals(io_context, SIGINT, SIGTERM);
        signals.async_wait([&io_context](const std::error_code& ec, int) {
            if (ec) return;
            VoiceTrace::log("step=server_shutdown status=graceful");
            io_context.stop();
        });

        unsigned int thread_count = std::thread::hardware_concurrency();
        if (thread_count == 0) thread_count = 4;

        std::cout << "TalkMe Server running on 5555 with " << thread_count << " threads...\n";

        std::vector<std::thread> threads;
        for (unsigned int i = 0; i < thread_count; ++i) {
            threads.emplace_back([&io_context]() { io_context.run(); });
        }
        for (auto& t : threads) t.join();

        // io_context stopped: server and sessions destroyed here, then Database singleton at process exit
    }
    catch (std::exception& e) { std::cerr << e.what() << "\n"; }
    return 0;
}