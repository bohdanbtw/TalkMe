#include "MessageCacheDb.h"
#include "../app/Application.h"
#include <sqlite3.h>
#include <nlohmann/json.hpp>

namespace TalkMe {

    namespace {
        int Clamp(int v, int lo, int hi) {
            if (v < lo) return lo;
            if (v > hi) return hi;
            return v;
        }

        void BindText(sqlite3_stmt* stmt, int idx, const std::string& s) {
            sqlite3_bind_text(stmt, idx, s.c_str(), -1, SQLITE_TRANSIENT);
        }

        std::string ReactionsToJson(const std::map<std::string, std::vector<std::string>>& reactions) {
            if (reactions.empty()) return "{}";
            nlohmann::json j = nlohmann::json::object();
            for (const auto& [emoji, users] : reactions) j[emoji] = users;
            return j.dump();
        }

        void ReactionsFromJson(const std::string& s, std::map<std::string, std::vector<std::string>>& out) {
            out.clear();
            if (s.empty()) return;
            nlohmann::json j = nlohmann::json::parse(s, nullptr, false);
            if (!j.is_object()) return;
            for (auto& [emoji, users] : j.items()) {
                if (!users.is_array()) continue;
                std::vector<std::string> list;
                for (const auto& u : users) if (u.is_string()) list.push_back(u.get<std::string>());
                out[emoji] = std::move(list);
            }
        }
    }

    MessageCacheDb::MessageCacheDb() = default;

    MessageCacheDb::~MessageCacheDb() {
        Close();
    }

    bool MessageCacheDb::Open(const std::string& path) {
        std::lock_guard<std::mutex> g(m_Mutex);
        Close();

        if (sqlite3_open_v2(path.c_str(), &m_Db,
            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK)
        {
            Close();
            return false;
        }

        sqlite3_busy_timeout(m_Db, 5000);
        sqlite3_exec(m_Db, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
        sqlite3_exec(m_Db, "PRAGMA synchronous=NORMAL;", nullptr, nullptr, nullptr);
        sqlite3_exec(m_Db, "PRAGMA temp_store=MEMORY;", nullptr, nullptr, nullptr);

        return InitSchema();
    }

    void MessageCacheDb::Close() {
        if (m_Db) {
            sqlite3_close(m_Db);
            m_Db = nullptr;
        }
    }

    bool MessageCacheDb::Exec(const char* sql) {
        if (!m_Db) return false;
        return sqlite3_exec(m_Db, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
    }

    bool MessageCacheDb::InitSchema() {
        if (!m_Db) return false;
        const char* sql =
            "CREATE TABLE IF NOT EXISTS messages ("
            "  channel_id INTEGER NOT NULL,"
            "  mid INTEGER NOT NULL,"
            "  sender TEXT NOT NULL,"
            "  content TEXT NOT NULL,"
            "  time TEXT NOT NULL,"
            "  edited_at TEXT NOT NULL DEFAULT '',"
            "  pinned INTEGER NOT NULL DEFAULT 0,"
            "  reply_to INTEGER NOT NULL DEFAULT 0,"
            "  attachment_id TEXT NOT NULL DEFAULT '',"
            "  reactions_json TEXT NOT NULL DEFAULT '{}',"
            "  PRIMARY KEY(channel_id, mid)"
            ");"
            "CREATE INDEX IF NOT EXISTS idx_messages_channel_mid ON messages(channel_id, mid);"
            "CREATE TABLE IF NOT EXISTS read_anchor ("
            "  channel_id INTEGER PRIMARY KEY,"
            "  last_read_mid INTEGER NOT NULL DEFAULT 0"
            ");";

        return Exec(sql);
    }

    bool MessageCacheDb::UpsertMessages(const std::vector<ChatMessage>& msgs) {
        if (msgs.empty()) return true;
        std::lock_guard<std::mutex> g(m_Mutex);
        if (!m_Db) return false;

        sqlite3_exec(m_Db, "BEGIN;", nullptr, nullptr, nullptr);

        sqlite3_stmt* stmt = nullptr;
        const char* q =
            "INSERT INTO messages (channel_id, mid, sender, content, time, edited_at, pinned, reply_to, attachment_id, reactions_json) "
            "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?) "
            "ON CONFLICT(channel_id, mid) DO UPDATE SET "
            "  sender=excluded.sender,"
            "  content=excluded.content,"
            "  time=excluded.time,"
            "  edited_at=excluded.edited_at,"
            "  pinned=excluded.pinned,"
            "  reply_to=excluded.reply_to,"
            "  attachment_id=excluded.attachment_id,"
            "  reactions_json=excluded.reactions_json;";

        bool ok = (sqlite3_prepare_v2(m_Db, q, -1, &stmt, nullptr) == SQLITE_OK);
        if (ok) {
            for (const auto& m : msgs) {
                sqlite3_reset(stmt);
                sqlite3_clear_bindings(stmt);
                sqlite3_bind_int(stmt, 1, m.channelId);
                sqlite3_bind_int(stmt, 2, m.id);
                BindText(stmt, 3, m.sender);
                BindText(stmt, 4, m.content);
                BindText(stmt, 5, m.timestamp);
                BindText(stmt, 6, ""); // edited_at currently not tracked on client
                sqlite3_bind_int(stmt, 7, m.pinned ? 1 : 0);
                sqlite3_bind_int(stmt, 8, m.replyToId);
                BindText(stmt, 9, m.attachmentId);
                BindText(stmt, 10, ReactionsToJson(m.reactions));
                if (sqlite3_step(stmt) != SQLITE_DONE) { ok = false; break; }
            }
            sqlite3_finalize(stmt);
        }

        sqlite3_exec(m_Db, ok ? "COMMIT;" : "ROLLBACK;", nullptr, nullptr, nullptr);
        return ok;
    }

    std::vector<ChatMessage> MessageCacheDb::QueryMessages(const char* sql, int channelId, int a, int b) {
        std::vector<ChatMessage> out;
        if (!m_Db) return out;
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(m_Db, sql, -1, &stmt, nullptr) != SQLITE_OK) return out;

        sqlite3_bind_int(stmt, 1, channelId);
        sqlite3_bind_int(stmt, 2, a);
        sqlite3_bind_int(stmt, 3, b);

        while (sqlite3_step(stmt) == SQLITE_ROW) {
            ChatMessage m{};
            m.channelId = sqlite3_column_int(stmt, 0);
            m.id = sqlite3_column_int(stmt, 1);
            const char* sender = (const char*)sqlite3_column_text(stmt, 2);
            const char* content = (const char*)sqlite3_column_text(stmt, 3);
            const char* time = (const char*)sqlite3_column_text(stmt, 4);
            const char* attachment = (const char*)sqlite3_column_text(stmt, 5);
            m.sender = sender ? sender : "";
            m.content = content ? content : "";
            m.timestamp = time ? time : "";
            m.attachmentId = attachment ? attachment : "";
            m.replyToId = sqlite3_column_int(stmt, 6);
            m.pinned = sqlite3_column_int(stmt, 7) != 0;
            const char* reactions = (const char*)sqlite3_column_text(stmt, 8);
            ReactionsFromJson(reactions ? reactions : "", m.reactions);
            out.push_back(std::move(m));
        }
        sqlite3_finalize(stmt);
        return out;
    }

    std::vector<ChatMessage> MessageCacheDb::LoadAround(int channelId, int anchorMid, int before, int after) {
        std::lock_guard<std::mutex> g(m_Mutex);
        before = Clamp(before, 0, 5000);
        after = Clamp(after, 0, 5000);
        if (!m_Db || channelId <= 0) return {};

        // Older (incl anchor)
        sqlite3_stmt* stmt = nullptr;
        std::vector<ChatMessage> older;
        const char* qOld =
            "SELECT channel_id, mid, sender, content, time, attachment_id, reply_to, pinned, reactions_json "
            "FROM messages WHERE channel_id = ? AND mid <= ? ORDER BY mid DESC LIMIT ?;";
        if (sqlite3_prepare_v2(m_Db, qOld, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int(stmt, 1, channelId);
            sqlite3_bind_int(stmt, 2, anchorMid);
            sqlite3_bind_int(stmt, 3, before + 1);
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                ChatMessage m{};
                m.channelId = sqlite3_column_int(stmt, 0);
                m.id = sqlite3_column_int(stmt, 1);
                const char* sender = (const char*)sqlite3_column_text(stmt, 2);
                const char* content = (const char*)sqlite3_column_text(stmt, 3);
                const char* time = (const char*)sqlite3_column_text(stmt, 4);
                const char* attachment = (const char*)sqlite3_column_text(stmt, 5);
                m.sender = sender ? sender : "";
                m.content = content ? content : "";
                m.timestamp = time ? time : "";
                m.attachmentId = attachment ? attachment : "";
                m.replyToId = sqlite3_column_int(stmt, 6);
                m.pinned = sqlite3_column_int(stmt, 7) != 0;
                const char* reactions = (const char*)sqlite3_column_text(stmt, 8);
                ReactionsFromJson(reactions ? reactions : "", m.reactions);
                older.push_back(std::move(m));
            }
            sqlite3_finalize(stmt);
        }
        std::reverse(older.begin(), older.end());

        // Newer
        std::vector<ChatMessage> newer;
        const char* qNew =
            "SELECT channel_id, mid, sender, content, time, attachment_id, reply_to, pinned, reactions_json "
            "FROM messages WHERE channel_id = ? AND mid > ? ORDER BY mid ASC LIMIT ?;";
        if (sqlite3_prepare_v2(m_Db, qNew, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int(stmt, 1, channelId);
            sqlite3_bind_int(stmt, 2, anchorMid);
            sqlite3_bind_int(stmt, 3, after);
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                ChatMessage m{};
                m.channelId = sqlite3_column_int(stmt, 0);
                m.id = sqlite3_column_int(stmt, 1);
                const char* sender = (const char*)sqlite3_column_text(stmt, 2);
                const char* content = (const char*)sqlite3_column_text(stmt, 3);
                const char* time = (const char*)sqlite3_column_text(stmt, 4);
                const char* attachment = (const char*)sqlite3_column_text(stmt, 5);
                m.sender = sender ? sender : "";
                m.content = content ? content : "";
                m.timestamp = time ? time : "";
                m.attachmentId = attachment ? attachment : "";
                m.replyToId = sqlite3_column_int(stmt, 6);
                m.pinned = sqlite3_column_int(stmt, 7) != 0;
                const char* reactions = (const char*)sqlite3_column_text(stmt, 8);
                ReactionsFromJson(reactions ? reactions : "", m.reactions);
                newer.push_back(std::move(m));
            }
            sqlite3_finalize(stmt);
        }

        older.insert(older.end(), newer.begin(), newer.end());
        return older;
    }

    std::vector<ChatMessage> MessageCacheDb::LoadLatest(int channelId, int limit) {
        std::lock_guard<std::mutex> g(m_Mutex);
        limit = Clamp(limit, 1, 5000);
        if (!m_Db || channelId <= 0) return {};

        sqlite3_stmt* stmt = nullptr;
        std::vector<ChatMessage> out;
        const char* q =
            "SELECT channel_id, mid, sender, content, time, attachment_id, reply_to, pinned, reactions_json "
            "FROM messages WHERE channel_id = ? ORDER BY mid DESC LIMIT ?;";
        if (sqlite3_prepare_v2(m_Db, q, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int(stmt, 1, channelId);
            sqlite3_bind_int(stmt, 2, limit);
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                ChatMessage m{};
                m.channelId = sqlite3_column_int(stmt, 0);
                m.id = sqlite3_column_int(stmt, 1);
                const char* sender = (const char*)sqlite3_column_text(stmt, 2);
                const char* content = (const char*)sqlite3_column_text(stmt, 3);
                const char* time = (const char*)sqlite3_column_text(stmt, 4);
                const char* attachment = (const char*)sqlite3_column_text(stmt, 5);
                m.sender = sender ? sender : "";
                m.content = content ? content : "";
                m.timestamp = time ? time : "";
                m.attachmentId = attachment ? attachment : "";
                m.replyToId = sqlite3_column_int(stmt, 6);
                m.pinned = sqlite3_column_int(stmt, 7) != 0;
                const char* reactions = (const char*)sqlite3_column_text(stmt, 8);
                ReactionsFromJson(reactions ? reactions : "", m.reactions);
                out.push_back(std::move(m));
            }
            sqlite3_finalize(stmt);
        }
        std::reverse(out.begin(), out.end());
        return out;
    }

    std::vector<ChatMessage> MessageCacheDb::LoadOlder(int channelId, int beforeMid, int limit) {
        std::lock_guard<std::mutex> g(m_Mutex);
        limit = Clamp(limit, 1, 5000);
        if (!m_Db || channelId <= 0) return {};

        sqlite3_stmt* stmt = nullptr;
        std::vector<ChatMessage> out;
        const char* q =
            "SELECT channel_id, mid, sender, content, time, attachment_id, reply_to, pinned, reactions_json "
            "FROM messages WHERE channel_id = ? AND mid < ? ORDER BY mid DESC LIMIT ?;";
        if (sqlite3_prepare_v2(m_Db, q, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int(stmt, 1, channelId);
            sqlite3_bind_int(stmt, 2, beforeMid);
            sqlite3_bind_int(stmt, 3, limit);
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                ChatMessage m{};
                m.channelId = sqlite3_column_int(stmt, 0);
                m.id = sqlite3_column_int(stmt, 1);
                const char* sender = (const char*)sqlite3_column_text(stmt, 2);
                const char* content = (const char*)sqlite3_column_text(stmt, 3);
                const char* time = (const char*)sqlite3_column_text(stmt, 4);
                const char* attachment = (const char*)sqlite3_column_text(stmt, 5);
                m.sender = sender ? sender : "";
                m.content = content ? content : "";
                m.timestamp = time ? time : "";
                m.attachmentId = attachment ? attachment : "";
                m.replyToId = sqlite3_column_int(stmt, 6);
                m.pinned = sqlite3_column_int(stmt, 7) != 0;
                const char* reactions = (const char*)sqlite3_column_text(stmt, 8);
                ReactionsFromJson(reactions ? reactions : "", m.reactions);
                out.push_back(std::move(m));
            }
            sqlite3_finalize(stmt);
        }
        std::reverse(out.begin(), out.end());
        return out;
    }

    std::vector<ChatMessage> MessageCacheDb::LoadNewer(int channelId, int afterMid, int limit) {
        std::lock_guard<std::mutex> g(m_Mutex);
        limit = Clamp(limit, 1, 5000);
        if (!m_Db || channelId <= 0) return {};

        sqlite3_stmt* stmt = nullptr;
        std::vector<ChatMessage> out;
        const char* q =
            "SELECT channel_id, mid, sender, content, time, attachment_id, reply_to, pinned, reactions_json "
            "FROM messages WHERE channel_id = ? AND mid > ? ORDER BY mid ASC LIMIT ?;";
        if (sqlite3_prepare_v2(m_Db, q, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int(stmt, 1, channelId);
            sqlite3_bind_int(stmt, 2, afterMid);
            sqlite3_bind_int(stmt, 3, limit);
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                ChatMessage m{};
                m.channelId = sqlite3_column_int(stmt, 0);
                m.id = sqlite3_column_int(stmt, 1);
                const char* sender = (const char*)sqlite3_column_text(stmt, 2);
                const char* content = (const char*)sqlite3_column_text(stmt, 3);
                const char* time = (const char*)sqlite3_column_text(stmt, 4);
                const char* attachment = (const char*)sqlite3_column_text(stmt, 5);
                m.sender = sender ? sender : "";
                m.content = content ? content : "";
                m.timestamp = time ? time : "";
                m.attachmentId = attachment ? attachment : "";
                m.replyToId = sqlite3_column_int(stmt, 6);
                m.pinned = sqlite3_column_int(stmt, 7) != 0;
                const char* reactions = (const char*)sqlite3_column_text(stmt, 8);
                ReactionsFromJson(reactions ? reactions : "", m.reactions);
                out.push_back(std::move(m));
            }
            sqlite3_finalize(stmt);
        }
        return out;
    }

    int MessageCacheDb::GetLastReadMid(int channelId) {
        std::lock_guard<std::mutex> g(m_Mutex);
        if (!m_Db || channelId <= 0) return 0;
        sqlite3_stmt* stmt = nullptr;
        int out = 0;
        if (sqlite3_prepare_v2(m_Db, "SELECT last_read_mid FROM read_anchor WHERE channel_id = ?;", -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int(stmt, 1, channelId);
            if (sqlite3_step(stmt) == SQLITE_ROW) out = sqlite3_column_int(stmt, 0);
            sqlite3_finalize(stmt);
        }
        return out;
    }

    bool MessageCacheDb::AdvanceLastReadMid(int channelId, int mid) {
        std::lock_guard<std::mutex> g(m_Mutex);
        if (!m_Db || channelId <= 0 || mid <= 0) return false;
        sqlite3_stmt* stmt = nullptr;
        const char* q =
            "INSERT INTO read_anchor (channel_id, last_read_mid) VALUES (?, ?) "
            "ON CONFLICT(channel_id) DO UPDATE SET last_read_mid = MAX(last_read_mid, excluded.last_read_mid);";
        bool ok = (sqlite3_prepare_v2(m_Db, q, -1, &stmt, nullptr) == SQLITE_OK);
        if (ok) {
            sqlite3_bind_int(stmt, 1, channelId);
            sqlite3_bind_int(stmt, 2, mid);
            ok = (sqlite3_step(stmt) == SQLITE_DONE);
            sqlite3_finalize(stmt);
        }
        return ok;
    }

    bool MessageCacheDb::PruneKeepLast(int channelId, int keepLastN) {
        std::lock_guard<std::mutex> g(m_Mutex);
        if (!m_Db || channelId <= 0) return false;
        keepLastN = Clamp(keepLastN, 0, 50000);

        sqlite3_stmt* stmt = nullptr;
        const char* q =
            "DELETE FROM messages WHERE channel_id = ? AND mid < ("
            "  SELECT IFNULL(MIN(mid), 0) FROM ("
            "    SELECT mid FROM messages WHERE channel_id = ? ORDER BY mid DESC LIMIT ?"
            "  )"
            ");";
        bool ok = (sqlite3_prepare_v2(m_Db, q, -1, &stmt, nullptr) == SQLITE_OK);
        if (ok) {
            sqlite3_bind_int(stmt, 1, channelId);
            sqlite3_bind_int(stmt, 2, channelId);
            sqlite3_bind_int(stmt, 3, keepLastN);
            ok = (sqlite3_step(stmt) == SQLITE_DONE);
            sqlite3_finalize(stmt);
        }
        return ok;
    }

} // namespace TalkMe

