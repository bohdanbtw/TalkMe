#include "Database.h"
#include "Protocol.h"
#include <sqlite3.h>
#include <nlohmann/json.hpp>
#include <iostream>
#include <algorithm>
#include <random>
#include <cstdio>
#include <cstring>
#include <array>
#include <vector>
#include <sstream>
#include <iomanip>

using json = nlohmann::json;

namespace {

    // Lightweight SHA-256 (standalone, no OpenSSL). Output 32 bytes.
    void Sha256(const uint8_t* data, size_t len, uint8_t out[32]) {
        static const uint32_t K[64] = {
            0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
            0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
            0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
            0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
            0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
            0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
            0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
            0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2,
        };
        uint32_t H[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
        uint8_t block[64];
        size_t i = 0;
        auto rotr = [](uint32_t x, int n) { return (x >> n) | (x << (32 - n)); };
        auto step = [&](const uint8_t* blk) {
            uint32_t W[64];
            for (int t = 0; t < 16; ++t) W[t] = (uint32_t)blk[t * 4] << 24 | (uint32_t)blk[t * 4 + 1] << 16 | (uint32_t)blk[t * 4 + 2] << 8 | blk[t * 4 + 3];
            for (int t = 16; t < 64; ++t) {
                uint32_t s0 = rotr(W[t - 15], 7) ^ rotr(W[t - 15], 18) ^ (W[t - 15] >> 3);
                uint32_t s1 = rotr(W[t - 2], 17) ^ rotr(W[t - 2], 19) ^ (W[t - 2] >> 10);
                W[t] = W[t - 16] + s0 + W[t - 7] + s1;
            }
            uint32_t a = H[0], b = H[1], c = H[2], d = H[3], e = H[4], f = H[5], g = H[6], h = H[7];
            for (int t = 0; t < 64; ++t) {
                uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
                uint32_t ch = (e & f) ^ ((~e) & g);
                uint32_t t1 = h + S1 + ch + K[t] + W[t];
                uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
                uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
                uint32_t t2 = S0 + maj;
                h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
            }
            H[0] += a; H[1] += b; H[2] += c; H[3] += d; H[4] += e; H[5] += f; H[6] += g; H[7] += h;
            };
        while (i + 64 <= len) { step(data + i); i += 64; }
        std::memset(block, 0, sizeof(block));
        std::memcpy(block, data + i, len - i);
        block[len - i] = 0x80;
        if (len - i >= 56) { step(block); std::memset(block, 0, sizeof(block)); }
        uint64_t bits = len * 8;
        for (int j = 0; j < 8; ++j) block[63 - j] = (uint8_t)(bits >> (j * 8));
        step(block);
        for (int j = 0; j < 8; ++j) {
            out[j * 4 + 0] = (uint8_t)(H[j] >> 24);
            out[j * 4 + 1] = (uint8_t)(H[j] >> 16);
            out[j * 4 + 2] = (uint8_t)(H[j] >> 8);
            out[j * 4 + 3] = (uint8_t)(H[j]);
        }
    }

    std::string BytesToHex(const uint8_t* buf, size_t n) {
        static const char hex[] = "0123456789abcdef";
        std::string s; s.reserve(n * 2);
        for (size_t i = 0; i < n; ++i) { s += hex[buf[i] >> 4]; s += hex[buf[i] & 15]; }
        return s;
    }

    bool HexToBytes(const std::string& hex, uint8_t* buf, size_t maxLen) {
        if (hex.size() % 2 != 0 || hex.size() / 2 > maxLen) return false;
        for (size_t i = 0; i < hex.size(); i += 2) {
            int a = (hex[i] >= 'a') ? hex[i] - 'a' + 10 : (hex[i] >= 'A') ? hex[i] - 'A' + 10 : hex[i] - '0';
            int b = (hex[i + 1] >= 'a') ? hex[i + 1] - 'a' + 10 : (hex[i + 1] >= 'A') ? hex[i + 1] - 'A' + 10 : hex[i + 1] - '0';
            if (a < 0 || a > 15 || b < 0 || b > 15) return false;
            buf[i / 2] = (uint8_t)((a << 4) | b);
        }
        return true;
    }

    // Hash password with salt: SHA256(salt || password). Salt 16 bytes, hash 32 bytes.
    std::string HashPasswordWithSalt(const std::string& password, const uint8_t salt[16]) {
        std::vector<uint8_t> input;
        input.reserve(16 + password.size());
        input.insert(input.end(), salt, salt + 16);
        input.insert(input.end(), password.begin(), password.end());
        uint8_t hash[32];
        Sha256(input.data(), input.size(), hash);
        return BytesToHex(hash, 32);
    }

    bool ConstantTimeEquals(const std::string& a, const std::string& b) {
        if (a.size() != b.size()) return false;
        volatile unsigned char diff = 0;
        for (size_t i = 0; i < a.size(); ++i) diff |= (unsigned char)a[i] ^ (unsigned char)b[i];
        return diff == 0;
    }

} // namespace

namespace TalkMe {

    Database& Database::Get() {
        static Database db;
        return db;
    }

    Database::Database() {
        if (sqlite3_open_v2("talkme.db", &m_Db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK) {
            std::cerr << "Can't open DB\n";
            return;
        }
        sqlite3_busy_timeout(m_Db, 5000);
        sqlite3_exec(m_Db, "PRAGMA journal_mode=WAL;", 0, 0, 0);
        sqlite3_exec(m_Db, "PRAGMA synchronous=NORMAL;", 0, 0, 0);
        // Bug fix: checkStmt was declared without an initialiser. SQLite sets the
        // output to nullptr on prepare failure, but rely on our own explicit init
        // so sqlite3_finalize(checkStmt) below is always safe regardless of
        // which code path is taken.
        sqlite3_stmt* checkStmt = nullptr;
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
        sqlite3_exec(m_Db, "ALTER TABLE messages ADD COLUMN edited_at DATETIME;", 0, 0, 0);
        sqlite3_exec(m_Db, "ALTER TABLE messages ADD COLUMN is_pinned INTEGER DEFAULT 0;", 0, 0, 0);
        sqlite3_exec(m_Db, "ALTER TABLE messages ADD COLUMN attachment_id TEXT DEFAULT '';", 0, 0, 0);
        sqlite3_exec(m_Db, "ALTER TABLE messages ADD COLUMN reply_to INTEGER DEFAULT 0;", 0, 0, 0);
        sqlite3_exec(m_Db, "ALTER TABLE channels ADD COLUMN description TEXT DEFAULT '';", 0, 0, 0);
        sqlite3_exec(m_Db, "CREATE TABLE IF NOT EXISTS reactions (message_id INTEGER, username TEXT, emoji TEXT, PRIMARY KEY(message_id, username, emoji));", 0, 0, 0);
        sqlite3_exec(m_Db, "CREATE TABLE IF NOT EXISTS friends (user1 TEXT, user2 TEXT, status TEXT DEFAULT 'pending', created_at DATETIME DEFAULT CURRENT_TIMESTAMP, PRIMARY KEY(user1, user2));", 0, 0, 0);
        sqlite3_exec(m_Db, "CREATE TABLE IF NOT EXISTS direct_messages (id INTEGER PRIMARY KEY AUTOINCREMENT, sender TEXT, receiver TEXT, content TEXT, time DATETIME DEFAULT CURRENT_TIMESTAMP);", 0, 0, 0);
        sqlite3_exec(m_Db, "ALTER TABLE server_members ADD COLUMN permissions INTEGER DEFAULT 0;", 0, 0, 0);
        sqlite3_exec(m_Db, "ALTER TABLE users ADD COLUMN totp_secret TEXT DEFAULT '';", 0, 0, 0);
        sqlite3_exec(m_Db, "ALTER TABLE users ADD COLUMN is_2fa_enabled INTEGER DEFAULT 0;", 0, 0, 0);
        sqlite3_exec(m_Db, "CREATE TABLE IF NOT EXISTS trusted_devices (username TEXT, device_id TEXT, PRIMARY KEY(username, device_id));", 0, 0, 0);

        sqlite3_stmt* countStmt = nullptr;
        if (sqlite3_prepare_v2(m_Db, "SELECT COUNT(*) FROM servers;", -1, &countStmt, 0) == SQLITE_OK &&
            sqlite3_step(countStmt) == SQLITE_ROW && sqlite3_column_int(countStmt, 0) == 0) {
            sqlite3_finalize(countStmt);
            sqlite3_stmt* ins = nullptr;
            if (sqlite3_prepare_v2(m_Db, "INSERT INTO servers (name, invite_code, owner) VALUES ('Global Hub', 'HUB001', 'system');", -1, &ins, 0) == SQLITE_OK) {
                sqlite3_step(ins);
                sqlite3_finalize(ins);
                int hubId = static_cast<int>(sqlite3_last_insert_rowid(m_Db));
                const char* chSql = "INSERT INTO channels (server_id, name, type) VALUES (?, ?, ?);";
                if (sqlite3_prepare_v2(m_Db, chSql, -1, &ins, 0) == SQLITE_OK) {
                    sqlite3_bind_int(ins, 1, hubId);
                    sqlite3_bind_text(ins, 2, "Welcome", -1, SQLITE_STATIC);
                    sqlite3_bind_text(ins, 3, "text", -1, SQLITE_STATIC);
                    sqlite3_step(ins);
                    sqlite3_reset(ins);
                    sqlite3_bind_int(ins, 1, hubId);
                    sqlite3_bind_text(ins, 2, "Lounge", -1, SQLITE_STATIC);
                    sqlite3_bind_text(ins, 3, "voice", -1, SQLITE_STATIC);
                    sqlite3_step(ins);
                    sqlite3_finalize(ins);
                }
            }
        }
        else if (countStmt) sqlite3_finalize(countStmt);

        m_Worker = std::thread(&Database::WorkerLoop, this);
    }

    Database::~Database() {
        {
            std::lock_guard<std::mutex> lock(m_QueueMutex);
            m_Shutdown = true;
        }
        m_QueueCv.notify_all();
        if (m_Worker.joinable()) m_Worker.join();
        sqlite3_close(m_Db);
    }

    std::string Database::GenerateInviteCode() {
        const char charset[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
        std::default_random_engine rng(std::random_device{}());
        std::uniform_int_distribution<> dist(0, static_cast<int>(sizeof(charset) - 2));
        std::string code;
        for (int i = 0; i < 6; ++i) code += charset[dist(rng)];
        return code;
    }

    std::string Database::RegisterUser(const std::string& email, std::string u, const std::string& p) {
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

        uint8_t salt[16];
        std::uniform_int_distribution<int> dist(0, 255);
        std::random_device rd;
        std::mt19937 gen(rd());
        for (int i = 0; i < 16; ++i) salt[i] = static_cast<uint8_t>(dist(gen));
        std::string storedPassword = BytesToHex(salt, 16) + "$" + HashPasswordWithSalt(p, salt);

        bool success = false;
        if (sqlite3_prepare_v2(m_Db, "INSERT INTO users (email, username, password) VALUES (?, ?, ?);", -1, &stmt, 0) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, email.c_str(), -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 2, final_username.c_str(), -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 3, storedPassword.c_str(), -1, SQLITE_TRANSIENT);
            success = (sqlite3_step(stmt) == SQLITE_DONE);
            sqlite3_finalize(stmt);
        }
        if (success) {
            if (sqlite3_prepare_v2(m_Db, "SELECT id FROM servers WHERE invite_code = 'HUB001' LIMIT 1;", -1, &stmt, 0) == SQLITE_OK) {
                if (sqlite3_step(stmt) == SQLITE_ROW) {
                    int hubId = sqlite3_column_int(stmt, 0);
                    sqlite3_finalize(stmt);
                    if (sqlite3_prepare_v2(m_Db, "INSERT INTO server_members (username, server_id) VALUES (?, ?);", -1, &stmt, 0) == SQLITE_OK) {
                        sqlite3_bind_text(stmt, 1, final_username.c_str(), -1, SQLITE_STATIC);
                        sqlite3_bind_int(stmt, 2, hubId);
                        sqlite3_step(stmt);
                        sqlite3_finalize(stmt);
                    }
                }
                else sqlite3_finalize(stmt);
            }
        }
        return success ? final_username : "";
    }

    int Database::LoginUser(const std::string& email, const std::string& p, const std::string& deviceId, std::string* outUsername) {
        std::fprintf(stderr, "[TalkMe DB] LoginUser: entered (email len=%zu)\n", email.size());
        std::fflush(stderr);
        if (outUsername) outUsername->clear();
        std::fprintf(stderr, "[TalkMe DB] LoginUser: acquiring shared_lock...\n");
        std::fflush(stderr);
        std::shared_lock<std::shared_mutex> lock(m_RwMutex);
        std::fprintf(stderr, "[TalkMe DB] LoginUser: lock acquired, executing query\n");
        std::fflush(stderr);
        sqlite3_stmt* stmt = nullptr;
        int result = 0;
        if (sqlite3_prepare_v2(m_Db, "SELECT username, password, IFNULL(is_2fa_enabled, 0) FROM users WHERE email = ?;", -1, &stmt, 0) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, email.c_str(), -1, SQLITE_STATIC);
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                std::string db_user = (const char*)sqlite3_column_text(stmt, 0);
                std::string db_pass = (const char*)sqlite3_column_text(stmt, 1);
                int is2fa = sqlite3_column_int(stmt, 2);
                bool authOk = false;
                size_t sep = db_pass.find('$');
                if (sep == 32 && db_pass.size() == 32 + 1 + 64) {
                    uint8_t salt[16];
                    if (HexToBytes(db_pass.substr(0, 32), salt, 16)) {
                        std::string computed = HashPasswordWithSalt(p, salt);
                        if (ConstantTimeEquals(computed, db_pass.substr(33))) authOk = true;
                    }
                }
                else if (ConstantTimeEquals(db_pass, p)) authOk = true;
                if (authOk) {
                    if (outUsername) *outUsername = db_user;
                    if (is2fa == 1) {
                        sqlite3_finalize(stmt);
                        stmt = nullptr;
                        if (!deviceId.empty() && sqlite3_prepare_v2(m_Db, "SELECT 1 FROM trusted_devices WHERE username = ? AND device_id = ?;", -1, &stmt, 0) == SQLITE_OK) {
                            sqlite3_bind_text(stmt, 1, db_user.c_str(), -1, SQLITE_STATIC);
                            sqlite3_bind_text(stmt, 2, deviceId.c_str(), -1, SQLITE_STATIC);
                            result = (sqlite3_step(stmt) == SQLITE_ROW) ? 1 : 2;
                            sqlite3_finalize(stmt);
                            stmt = nullptr;
                        }
                        else
                            result = 2;
                    }
                    else
                        result = 1;
                }
            }
            if (stmt) sqlite3_finalize(stmt);
        }
        std::fprintf(stderr, "[TalkMe DB] LoginUser: returning %d\n", result);
        std::fflush(stderr);
        return result;
    }

    void Database::LoginUserAsync(const std::string& email, const std::string& p, const std::string& deviceId,
        std::function<void(int result, std::string username, std::string serversJson, bool has2fa)> onDone) {
        if (!onDone) return;
        Enqueue([this, email, p, deviceId, onDone]() {
            std::string username;
            int result = LoginUser(email, p, deviceId, &username);
            std::string serversJson;
            bool has2fa = false;
            if (result == 1 && !username.empty()) {
                has2fa = !GetUserTOTPSecret(username, nullptr).empty();
                serversJson = GetUserServersJSON(username);
            }
            onDone(result, std::move(username), std::move(serversJson), has2fa);
            });
    }

    void Database::TrustDevice(const std::string& username, const std::string& deviceId) {
        if (deviceId.empty()) return;
        std::unique_lock<std::shared_mutex> lock(m_RwMutex);
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(m_Db, "INSERT OR IGNORE INTO trusted_devices (username, device_id) VALUES (?, ?);", -1, &stmt, 0) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 2, deviceId.c_str(), -1, SQLITE_STATIC);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
    }

    std::string Database::ValidateSession(const std::string& email, const std::string& plainPassword) {
        std::shared_lock<std::shared_mutex> lock(m_RwMutex);
        sqlite3_stmt* stmt = nullptr;
        std::string username;

        if (sqlite3_prepare_v2(m_Db,
            "SELECT username, password FROM users WHERE email = ?;",
            -1, &stmt, 0) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, email.c_str(), -1, SQLITE_STATIC);

            if (sqlite3_step(stmt) == SQLITE_ROW) {
                const char* u = (const char*)sqlite3_column_text(stmt, 0);
                std::string db_pass = (const char*)sqlite3_column_text(stmt, 1);

                bool authOk = false;
                size_t sep = db_pass.find('$');
                if (sep == 32 && db_pass.size() == 97) {
                    uint8_t salt[16];
                    if (HexToBytes(db_pass.substr(0, 32), salt, 16)) {
                        std::string computed = HashPasswordWithSalt(plainPassword, salt);
                        if (ConstantTimeEquals(computed, db_pass.substr(33)))
                            authOk = true;
                    }
                }
                else {
                    if (ConstantTimeEquals(db_pass, plainPassword))
                        authOk = true;
                }

                if (authOk && u) username = u;
            }
            sqlite3_finalize(stmt);
        }
        return username;
    }

    std::string Database::GetUserTOTPSecret(const std::string& email_or_username, std::string* outUsername) {
        if (outUsername) outUsername->clear();
        std::shared_lock<std::shared_mutex> lock(m_RwMutex);
        sqlite3_stmt* stmt = nullptr;
        std::string secret;
        const char* sql =
            "SELECT username, IFNULL(totp_secret, '') "
            "FROM users "
            "WHERE (email = ? OR username = ?) "
            "  AND is_2fa_enabled = 1 "
            "LIMIT 1;";
        if (sqlite3_prepare_v2(m_Db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, email_or_username.c_str(), -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 2, email_or_username.c_str(), -1, SQLITE_STATIC);
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                const char* u = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
                const char* s = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
                if (u && outUsername) *outUsername = u;
                if (s) secret = s;
            }
            sqlite3_finalize(stmt);
        }
        return secret;
    }

    bool Database::EnableUser2FA(const std::string& username, const std::string& secret) {
        std::unique_lock<std::shared_mutex> lock(m_RwMutex);
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(m_Db, "UPDATE users SET totp_secret = ?, is_2fa_enabled = 1 WHERE username = ?;", -1, &stmt, 0) != SQLITE_OK) return false;
        sqlite3_bind_text(stmt, 1, secret.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, username.c_str(), -1, SQLITE_STATIC);
        bool ok = (sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(m_Db) > 0);
        sqlite3_finalize(stmt);
        return ok;
    }

    bool Database::DisableUser2FA(const std::string& username) {
        std::unique_lock<std::shared_mutex> lock(m_RwMutex);
        sqlite3_stmt* stmt = nullptr;

        if (sqlite3_prepare_v2(m_Db, "UPDATE users SET totp_secret = '', is_2fa_enabled = 0 "
            "WHERE username = ?;",
            -1, &stmt, 0) != SQLITE_OK)
            return false;

        sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_STATIC);
        bool ok = (sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(m_Db) > 0);
        sqlite3_finalize(stmt);
        stmt = nullptr;

        if (ok) {
            if (sqlite3_prepare_v2(m_Db,
                "DELETE FROM trusted_devices WHERE username = ?;",
                -1, &stmt, 0) == SQLITE_OK) {
                sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_STATIC);
                sqlite3_step(stmt);
                sqlite3_finalize(stmt);
            }
        }

        return ok;
    }

    int Database::GetDefaultServerId() {
        std::shared_lock<std::shared_mutex> lock(m_RwMutex);
        sqlite3_stmt* stmt = nullptr;
        int id = -1;
        if (sqlite3_prepare_v2(m_Db, "SELECT id FROM servers ORDER BY id ASC LIMIT 1;", -1, &stmt, 0) == SQLITE_OK) {
            if (sqlite3_step(stmt) == SQLITE_ROW) id = sqlite3_column_int(stmt, 0);
            sqlite3_finalize(stmt);
        }
        return id;
    }

    void Database::AddUserToDefaultServer(const std::string& username) {
        int sid = GetDefaultServerId();
        if (sid <= 0) return;
        std::unique_lock<std::shared_mutex> lock(m_RwMutex);
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(m_Db, "INSERT OR IGNORE INTO server_members (username, server_id) VALUES (?, ?);", -1, &stmt, 0) != SQLITE_OK) return;
        sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_int(stmt, 2, sid);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }

    void Database::CreateServer(const std::string& name, const std::string& owner) {
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

    void Database::CreateChannel(int serverId, const std::string& name, const std::string& type) {
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

    int Database::JoinServer(const std::string& username, const std::string& code) {
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

    std::string Database::GetUserServersJSON(const std::string& username) {
        std::shared_lock<std::shared_mutex> lock(m_RwMutex);
        json j = json::array();
        sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(m_Db, "SELECT s.id, s.name, s.invite_code FROM servers s JOIN server_members m ON s.id = m.server_id WHERE m.username = ?;", -1, &stmt, 0) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_STATIC);
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                const char* n = (const char*)sqlite3_column_text(stmt, 1);
                const char* c = (const char*)sqlite3_column_text(stmt, 2);
                j.push_back({ {"id", sqlite3_column_int(stmt, 0)}, {"name", n ? n : ""}, {"code", c ? c : ""} });
            }
            sqlite3_finalize(stmt);
        }
        return j.dump();
    }

    std::string Database::GetServerContentJSON(int serverId) {
        std::shared_lock<std::shared_mutex> lock(m_RwMutex);
        json j = json::array();
        sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(m_Db, "SELECT id, name, type, IFNULL(description, '') FROM channels WHERE server_id = ?;", -1, &stmt, 0) == SQLITE_OK) {
            sqlite3_bind_int(stmt, 1, serverId);
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                const char* n = (const char*)sqlite3_column_text(stmt, 1);
                const char* t = (const char*)sqlite3_column_text(stmt, 2);
                const char* d = (const char*)sqlite3_column_text(stmt, 3);
                json entry = { {"id", sqlite3_column_int(stmt, 0)}, {"name", n ? n : ""}, {"type", t ? t : ""} };
                if (d && d[0] != '\0') entry["desc"] = d;
                j.push_back(entry);
            }
            sqlite3_finalize(stmt);
        }
        return j.dump();
    }

    std::string Database::GetMessageHistoryJSON(int channelId) {
        std::shared_lock<std::shared_mutex> lock(m_RwMutex);
        json j = json::array();
        sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(m_Db, "SELECT id, channel_id, sender, content, time, IFNULL(edited_at, ''), is_pinned, IFNULL(attachment_id, ''), IFNULL(reply_to, 0) FROM messages WHERE channel_id = ? ORDER BY time ASC LIMIT 50;", -1, &stmt, 0) == SQLITE_OK) {
            sqlite3_bind_int(stmt, 1, channelId);
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                const char* u = (const char*)sqlite3_column_text(stmt, 2);
                const char* m = (const char*)sqlite3_column_text(stmt, 3);
                const char* t = (const char*)sqlite3_column_text(stmt, 4);
                const char* editVal = (const char*)sqlite3_column_text(stmt, 5);
                const char* attVal = (const char*)sqlite3_column_text(stmt, 7);
                int replyTo = sqlite3_column_int(stmt, 8);
                int mid = sqlite3_column_int(stmt, 0);
                json entry = { {"mid", mid}, {"cid", sqlite3_column_int(stmt, 1)}, {"u", u ? u : ""}, {"msg", m ? m : ""}, {"time", t ? t : ""}, {"edit", editVal ? editVal : ""}, {"pin", sqlite3_column_int(stmt, 6) != 0}, {"attachment", attVal ? attVal : ""} };
                if (replyTo > 0) entry["reply_to"] = replyTo;

                sqlite3_stmt* rStmt = nullptr;
                if (sqlite3_prepare_v2(m_Db, "SELECT emoji, GROUP_CONCAT(username) FROM reactions WHERE message_id = ? GROUP BY emoji;", -1, &rStmt, 0) == SQLITE_OK) {
                    sqlite3_bind_int(rStmt, 1, mid);
                    json reactions = json::object();
                    while (sqlite3_step(rStmt) == SQLITE_ROW) {
                        const char* em = (const char*)sqlite3_column_text(rStmt, 0);
                        const char* us = (const char*)sqlite3_column_text(rStmt, 1);
                        if (em && us) {
                            json arr = json::array();
                            std::string uStr(us);
                            size_t p = 0;
                            while ((p = uStr.find(',')) != std::string::npos) {
                                arr.push_back(uStr.substr(0, p));
                                uStr.erase(0, p + 1);
                            }
                            if (!uStr.empty()) arr.push_back(uStr);
                            reactions[em] = arr;
                        }
                    }
                    sqlite3_finalize(rStmt);
                    if (!reactions.empty()) entry["reactions"] = reactions;
                }
                j.push_back(entry);
            }
            sqlite3_finalize(stmt);
        }
        if (j.empty())
            j.push_back({ {"cid", channelId} });
        return j.dump();
    }

    void Database::SaveMessage(int cid, const std::string& sender, const std::string& msg, const std::string& attachmentId, int replyTo) {
        int ch = cid;
        std::string s = sender;
        std::string m = msg;
        std::string aid = attachmentId;
        int rt = replyTo;
        Enqueue([this, ch, s, m, aid, rt]() {
            std::unique_lock<std::shared_mutex> lock(m_RwMutex);
            sqlite3_stmt* stmt;
            if (sqlite3_prepare_v2(m_Db, "INSERT INTO messages (channel_id, sender, content, attachment_id, reply_to) VALUES (?, ?, ?, ?, ?);", -1, &stmt, 0) == SQLITE_OK) {
                sqlite3_bind_int(stmt, 1, ch);
                sqlite3_bind_text(stmt, 2, s.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 3, m.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 4, aid.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_int(stmt, 5, rt);
                sqlite3_step(stmt);
                sqlite3_finalize(stmt);
            }
            });
    }

    int Database::SaveMessageReturnId(int cid, const std::string& sender, const std::string& msg, const std::string& attachmentId, int replyTo) {
        std::unique_lock<std::shared_mutex> lock(m_RwMutex);
        sqlite3_stmt* stmt = nullptr;
        int mid = 0;
        if (sqlite3_prepare_v2(m_Db, "INSERT INTO messages (channel_id, sender, content, attachment_id, reply_to) VALUES (?, ?, ?, ?, ?);", -1, &stmt, 0) == SQLITE_OK) {
            sqlite3_bind_int(stmt, 1, cid);
            sqlite3_bind_text(stmt, 2, sender.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 3, msg.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 4, attachmentId.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(stmt, 5, replyTo);
            if (sqlite3_step(stmt) == SQLITE_DONE)
                mid = (int)sqlite3_last_insert_rowid(m_Db);
            sqlite3_finalize(stmt);
        }
        return mid;
    }

    int Database::GetServerIdForChannel(int cid) {
        std::shared_lock<std::shared_mutex> lock(m_RwMutex);
        sqlite3_stmt* stmt = nullptr;
        int out = -1;
        if (sqlite3_prepare_v2(m_Db, "SELECT server_id FROM channels WHERE id = ?;", -1, &stmt, 0) == SQLITE_OK) {
            sqlite3_bind_int(stmt, 1, cid);
            if (sqlite3_step(stmt) == SQLITE_ROW) out = sqlite3_column_int(stmt, 0);
            sqlite3_finalize(stmt);
        }
        return out;
    }

    std::vector<std::string> Database::GetUsersInServerByChannel(int channelId) {
        std::shared_lock<std::shared_mutex> lock(m_RwMutex);
        std::vector<std::string> users;
        sqlite3_stmt* stmt;

        const char* query = "SELECT m.username FROM server_members m "
                            "JOIN channels c ON m.server_id = c.server_id "
                            "WHERE c.id = ?;";

        if (sqlite3_prepare_v2(m_Db, query, -1, &stmt, 0) == SQLITE_OK) {
            sqlite3_bind_int(stmt, 1, channelId);
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                const char* u = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
                if (u) users.push_back(u);
            }
            sqlite3_finalize(stmt);
        }
        return users;
    }

    std::vector<std::string> Database::GetServerMembers(int serverId) {
        std::shared_lock<std::shared_mutex> lock(m_RwMutex);
        std::vector<std::string> users;
        sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(m_Db, "SELECT username FROM server_members WHERE server_id = ?;", -1, &stmt, 0) == SQLITE_OK) {
            sqlite3_bind_int(stmt, 1, serverId);
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                const char* u = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
                if (u) users.push_back(u);
            }
            sqlite3_finalize(stmt);
        }
        return users;
    }

    int Database::SaveDirectMessage(const std::string& sender, const std::string& receiver, const std::string& content) {
        std::unique_lock<std::shared_mutex> lock(m_RwMutex);
        sqlite3_stmt* stmt = nullptr;
        int mid = 0;
        if (sqlite3_prepare_v2(m_Db, "INSERT INTO direct_messages (sender, receiver, content) VALUES (?, ?, ?);", -1, &stmt, 0) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, sender.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, receiver.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 3, content.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(stmt) == SQLITE_DONE) mid = (int)sqlite3_last_insert_rowid(m_Db);
            sqlite3_finalize(stmt);
        }
        return mid;
    }

    std::string Database::GetDMHistoryJSON(const std::string& user1, const std::string& user2) {
        std::shared_lock<std::shared_mutex> lock(m_RwMutex);
        json j = json::array();
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(m_Db,
            "SELECT id, sender, receiver, content, time FROM direct_messages "
            "WHERE (sender=? AND receiver=?) OR (sender=? AND receiver=?) "
            "ORDER BY time ASC LIMIT 100;", -1, &stmt, 0) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, user1.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, user2.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 3, user2.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 4, user1.c_str(), -1, SQLITE_TRANSIENT);
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                const char* s = (const char*)sqlite3_column_text(stmt, 1);
                const char* c = (const char*)sqlite3_column_text(stmt, 3);
                const char* t = (const char*)sqlite3_column_text(stmt, 4);
                j.push_back({ {"mid", sqlite3_column_int(stmt, 0)}, {"u", s ? s : ""}, {"msg", c ? c : ""}, {"time", t ? t : ""} });
            }
            sqlite3_finalize(stmt);
        }
        return j.dump();
    }

    bool Database::AreFriends(const std::string& user1, const std::string& user2) {
        std::shared_lock<std::shared_mutex> lock(m_RwMutex);
        sqlite3_stmt* stmt = nullptr;
        bool result = false;
        if (sqlite3_prepare_v2(m_Db,
            "SELECT 1 FROM friends WHERE ((user1=? AND user2=?) OR (user1=? AND user2=?)) AND status='accepted';",
            -1, &stmt, 0) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, user1.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, user2.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 3, user2.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 4, user1.c_str(), -1, SQLITE_TRANSIENT);
            result = (sqlite3_step(stmt) == SQLITE_ROW);
            sqlite3_finalize(stmt);
        }
        return result;
    }

    bool Database::SendFriendRequest(const std::string& from, const std::string& toUsername) {
        std::unique_lock<std::shared_mutex> lock(m_RwMutex);
        if (from == toUsername) return false;
        sqlite3_stmt* chk = nullptr;
        if (sqlite3_prepare_v2(m_Db, "SELECT 1 FROM friends WHERE (user1=? AND user2=?) OR (user1=? AND user2=?);", -1, &chk, 0) == SQLITE_OK) {
            sqlite3_bind_text(chk, 1, from.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(chk, 2, toUsername.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(chk, 3, toUsername.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(chk, 4, from.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(chk) == SQLITE_ROW) { sqlite3_finalize(chk); return false; }
            sqlite3_finalize(chk);
        }
        sqlite3_stmt* stmt = nullptr;
        bool ok = false;
        if (sqlite3_prepare_v2(m_Db, "INSERT INTO friends (user1, user2, status) VALUES (?, ?, 'pending');", -1, &stmt, 0) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, from.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, toUsername.c_str(), -1, SQLITE_TRANSIENT);
            ok = (sqlite3_step(stmt) == SQLITE_DONE);
            sqlite3_finalize(stmt);
        }
        return ok;
    }

    bool Database::AcceptFriendRequest(const std::string& user, const std::string& friendUser) {
        std::unique_lock<std::shared_mutex> lock(m_RwMutex);
        sqlite3_stmt* stmt = nullptr;
        bool ok = false;
        if (sqlite3_prepare_v2(m_Db, "UPDATE friends SET status='accepted' WHERE user1=? AND user2=? AND status='pending';", -1, &stmt, 0) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, friendUser.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, user.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(stmt);
            ok = (sqlite3_changes(m_Db) > 0);
            sqlite3_finalize(stmt);
        }
        return ok;
    }

    bool Database::RejectOrRemoveFriend(const std::string& user, const std::string& friendUser) {
        std::unique_lock<std::shared_mutex> lock(m_RwMutex);
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(m_Db, "DELETE FROM friends WHERE (user1=? AND user2=?) OR (user1=? AND user2=?);", -1, &stmt, 0) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, user.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, friendUser.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 3, friendUser.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 4, user.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
        return true;
    }

    std::string Database::GetFriendListJSON(const std::string& username) {
        std::shared_lock<std::shared_mutex> lock(m_RwMutex);
        json j = json::array();
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(m_Db,
            "SELECT CASE WHEN user1=? THEN user2 ELSE user1 END AS friend, status, "
            "CASE WHEN user1=? THEN 'sent' ELSE 'received' END AS direction "
            "FROM friends WHERE user1=? OR user2=?;", -1, &stmt, 0) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, username.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 3, username.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 4, username.c_str(), -1, SQLITE_TRANSIENT);
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                const char* f = (const char*)sqlite3_column_text(stmt, 0);
                const char* s = (const char*)sqlite3_column_text(stmt, 1);
                const char* d = (const char*)sqlite3_column_text(stmt, 2);
                if (f && s) j.push_back({ {"u", f}, {"status", s}, {"direction", d ? d : ""} });
            }
            sqlite3_finalize(stmt);
        }
        return j.dump();
    }

    bool Database::AddReaction(int messageId, const std::string& username, const std::string& emoji) {
        std::unique_lock<std::shared_mutex> lock(m_RwMutex);
        sqlite3_stmt* stmt = nullptr;
        bool ok = false;
        if (sqlite3_prepare_v2(m_Db, "INSERT OR IGNORE INTO reactions (message_id, username, emoji) VALUES (?, ?, ?);", -1, &stmt, 0) == SQLITE_OK) {
            sqlite3_bind_int(stmt, 1, messageId);
            sqlite3_bind_text(stmt, 2, username.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 3, emoji.c_str(), -1, SQLITE_TRANSIENT);
            ok = (sqlite3_step(stmt) == SQLITE_DONE);
            sqlite3_finalize(stmt);
        }
        return ok;
    }

    bool Database::RemoveReaction(int messageId, const std::string& username, const std::string& emoji) {
        std::unique_lock<std::shared_mutex> lock(m_RwMutex);
        sqlite3_stmt* stmt = nullptr;
        bool ok = false;
        if (sqlite3_prepare_v2(m_Db, "DELETE FROM reactions WHERE message_id = ? AND username = ? AND emoji = ?;", -1, &stmt, 0) == SQLITE_OK) {
            sqlite3_bind_int(stmt, 1, messageId);
            sqlite3_bind_text(stmt, 2, username.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 3, emoji.c_str(), -1, SQLITE_TRANSIENT);
            ok = (sqlite3_step(stmt) == SQLITE_DONE);
            sqlite3_finalize(stmt);
        }
        return ok;
    }

    std::string Database::GetReactionsJSON(int messageId) {
        std::shared_lock<std::shared_mutex> lock(m_RwMutex);
        json j = json::object();
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(m_Db, "SELECT emoji, GROUP_CONCAT(username) FROM reactions WHERE message_id = ? GROUP BY emoji;", -1, &stmt, 0) == SQLITE_OK) {
            sqlite3_bind_int(stmt, 1, messageId);
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                const char* emoji = (const char*)sqlite3_column_text(stmt, 0);
                const char* users = (const char*)sqlite3_column_text(stmt, 1);
                if (emoji && users) {
                    json arr = json::array();
                    std::string userStr(users);
                    size_t pos = 0;
                    while ((pos = userStr.find(',')) != std::string::npos) {
                        arr.push_back(userStr.substr(0, pos));
                        userStr.erase(0, pos + 1);
                    }
                    if (!userStr.empty()) arr.push_back(userStr);
                    j[emoji] = arr;
                }
            }
            sqlite3_finalize(stmt);
        }
        return j.dump();
    }

    bool Database::DeleteChannel(int channelId, const std::string& username) {
        std::unique_lock<std::shared_mutex> lock(m_RwMutex);
        int serverId = 0;
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(m_Db, "SELECT server_id FROM channels WHERE id = ?;", -1, &stmt, 0) == SQLITE_OK) {
            sqlite3_bind_int(stmt, 1, channelId);
            if (sqlite3_step(stmt) == SQLITE_ROW) serverId = sqlite3_column_int(stmt, 0);
            sqlite3_finalize(stmt);
        }
        if (serverId == 0) return false;

        // Only server owner can delete channels
        sqlite3_stmt* ownerStmt = nullptr;
        bool isOwner = false;
        if (sqlite3_prepare_v2(m_Db, "SELECT 1 FROM servers WHERE id = ? AND owner = ?;", -1, &ownerStmt, 0) == SQLITE_OK) {
            sqlite3_bind_int(ownerStmt, 1, serverId);
            sqlite3_bind_text(ownerStmt, 2, username.c_str(), -1, SQLITE_TRANSIENT);
            isOwner = (sqlite3_step(ownerStmt) == SQLITE_ROW);
            sqlite3_finalize(ownerStmt);
        }
        if (!isOwner) return false;

        sqlite3_stmt* delStmt = nullptr;
        if (sqlite3_prepare_v2(m_Db, "DELETE FROM channels WHERE id = ?;", -1, &delStmt, 0) == SQLITE_OK) {
            sqlite3_bind_int(delStmt, 1, channelId);
            sqlite3_step(delStmt);
            sqlite3_finalize(delStmt);
        }
        sqlite3_stmt* msgStmt = nullptr;
        if (sqlite3_prepare_v2(m_Db, "DELETE FROM messages WHERE channel_id = ?;", -1, &msgStmt, 0) == SQLITE_OK) {
            sqlite3_bind_int(msgStmt, 1, channelId);
            sqlite3_step(msgStmt);
            sqlite3_finalize(msgStmt);
        }
        return true;
    }

    uint32_t Database::GetUserPermissions(int serverId, const std::string& username) {
        std::shared_lock<std::shared_mutex> lock(m_RwMutex);
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(m_Db, "SELECT owner FROM servers WHERE id = ?;", -1, &stmt, 0) == SQLITE_OK) {
            sqlite3_bind_int(stmt, 1, serverId);
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                const char* owner = (const char*)sqlite3_column_text(stmt, 0);
                if (owner && username == owner) { sqlite3_finalize(stmt); return static_cast<uint32_t>(Perm_Admin); }
            }
            sqlite3_finalize(stmt);
        }
        if (sqlite3_prepare_v2(m_Db, "SELECT permissions FROM server_members WHERE server_id = ? AND username = ?;", -1, &stmt, 0) == SQLITE_OK) {
            sqlite3_bind_int(stmt, 1, serverId);
            sqlite3_bind_text(stmt, 2, username.c_str(), -1, SQLITE_STATIC);
            uint32_t perms = 0;
            if (sqlite3_step(stmt) == SQLITE_ROW) perms = static_cast<uint32_t>(sqlite3_column_int(stmt, 0));
            sqlite3_finalize(stmt);
            return perms;
        }
        return 0;
    }

    bool Database::DeleteMessage(int msgId, int cid, const std::string& username) {
        int serverId = GetServerIdForChannel(cid);
        if (serverId < 0) return false;
        std::string sender;
        {
            std::shared_lock<std::shared_mutex> lock(m_RwMutex);
            sqlite3_stmt* stmt = nullptr;
            if (sqlite3_prepare_v2(m_Db, "SELECT sender FROM messages WHERE id = ? AND channel_id = ?;", -1, &stmt, 0) == SQLITE_OK) {
                sqlite3_bind_int(stmt, 1, msgId);
                sqlite3_bind_int(stmt, 2, cid);
                if (sqlite3_step(stmt) == SQLITE_ROW) { const char* s = (const char*)sqlite3_column_text(stmt, 0); if (s) sender = s; }
                sqlite3_finalize(stmt);
            }
        }
        uint32_t perms = GetUserPermissions(serverId, username);
        bool allow = (username == sender) || ((perms & (Perm_Delete_Messages | Perm_Admin)) != 0);
        if (!allow) return false;
        std::unique_lock<std::shared_mutex> lock(m_RwMutex);
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(m_Db, "DELETE FROM messages WHERE id = ? AND channel_id = ?;", -1, &stmt, 0) != SQLITE_OK) return false;
        sqlite3_bind_int(stmt, 1, msgId);
        sqlite3_bind_int(stmt, 2, cid);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        return true;
    }

    bool Database::EditMessage(int msgId, const std::string& username, const std::string& newContent) {
        std::unique_lock<std::shared_mutex> lock(m_RwMutex);
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(m_Db, "UPDATE messages SET content = ?, edited_at = CURRENT_TIMESTAMP WHERE id = ? AND sender = ?;", -1, &stmt, 0) != SQLITE_OK) return false;
        sqlite3_bind_text(stmt, 1, newContent.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 2, msgId);
        sqlite3_bind_text(stmt, 3, username.c_str(), -1, SQLITE_STATIC);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        return sqlite3_changes(m_Db) > 0;
    }

    bool Database::PinMessage(int msgId, int cid, const std::string& username, bool pinState) {
        int serverId = GetServerIdForChannel(cid);
        if (serverId < 0) return false;
        uint32_t perms = GetUserPermissions(serverId, username);
        if ((perms & Perm_Pin_Messages) == 0 && (perms & Perm_Admin) == 0) return false;
        std::unique_lock<std::shared_mutex> lock(m_RwMutex);
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(m_Db, "UPDATE messages SET is_pinned = ? WHERE id = ? AND channel_id = ?;", -1, &stmt, 0) != SQLITE_OK) return false;
        sqlite3_bind_int(stmt, 1, pinState ? 1 : 0);
        sqlite3_bind_int(stmt, 2, msgId);
        sqlite3_bind_int(stmt, 3, cid);
        bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
        sqlite3_finalize(stmt);
        return ok;
    }

    void Database::Enqueue(std::function<void()> task) {
        if (!task) return;
        {
            std::lock_guard<std::mutex> lock(m_QueueMutex);
            if (m_Shutdown) return;
            m_TaskQueue.push(std::move(task));
        }
        m_QueueCv.notify_one();
    }

    void Database::WorkerLoop() {
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

} // namespace TalkMe