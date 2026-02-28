#pragma once
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

struct sqlite3;

namespace TalkMe {
    struct ChatMessage;

    class MessageCacheDb {
    public:
        MessageCacheDb();
        ~MessageCacheDb();

        MessageCacheDb(const MessageCacheDb&) = delete;
        MessageCacheDb& operator=(const MessageCacheDb&) = delete;

        bool Open(const std::string& path);
        void Close();

        bool UpsertMessages(const std::vector<ChatMessage>& msgs);
        std::vector<ChatMessage> LoadAround(int channelId, int anchorMid, int before, int after);
        std::vector<ChatMessage> LoadLatest(int channelId, int limit);
        std::vector<ChatMessage> LoadOlder(int channelId, int beforeMid, int limit);
        std::vector<ChatMessage> LoadNewer(int channelId, int afterMid, int limit);

        int GetLastReadMid(int channelId);
        bool AdvanceLastReadMid(int channelId, int mid);

        bool PruneKeepLast(int channelId, int keepLastN);

    private:
        bool InitSchema();
        std::vector<ChatMessage> QueryMessages(const char* sql, int channelId, int a, int b);
        bool Exec(const char* sql);

        sqlite3* m_Db = nullptr;
        std::mutex m_Mutex;
    };
}

