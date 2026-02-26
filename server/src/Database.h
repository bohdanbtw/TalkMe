#pragma once
#include <string>
#include <vector>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <condition_variable>
#include <queue>
#include <functional>
#include <cstdint>

struct sqlite3;

namespace TalkMe {

    class Database {
    public:
        static Database& Get();

        Database();
        ~Database();

        std::string GenerateInviteCode();
        std::string RegisterUser(const std::string& email, std::string u, const std::string& p);
        int LoginUser(const std::string& email, const std::string& p, const std::string& deviceId, std::string* outUsername);
        /// Runs LoginUser on the DB worker thread; onDone(result, username, serversJson, has2fa) is invoked from that thread.
        /// For result!=1, serversJson is empty and has2fa is false.
        void LoginUserAsync(const std::string& email, const std::string& p, const std::string& deviceId,
            std::function<void(int result, std::string username, std::string serversJson, bool has2fa)> onDone);
        void TrustDevice(const std::string& username, const std::string& deviceId);
        std::string ValidateSession(const std::string& email, const std::string& plainPassword);
        std::string GetUserTOTPSecret(const std::string& email_or_username, std::string* outUsername = nullptr);
        bool EnableUser2FA(const std::string& username, const std::string& secret);
        bool DisableUser2FA(const std::string& username);
        int GetDefaultServerId();
        void AddUserToDefaultServer(const std::string& username);
        void CreateServer(const std::string& name, const std::string& owner);
        void CreateChannel(int serverId, const std::string& name, const std::string& type);
        int JoinServer(const std::string& username, const std::string& code);
        std::string GetUserServersJSON(const std::string& username);
        std::string GetServerContentJSON(int serverId);
        std::string GetMessageHistoryJSON(int channelId);
        void SaveMessage(int cid, const std::string& sender, const std::string& msg, const std::string& attachmentId = "", int replyTo = 0);
        int SaveMessageReturnId(int cid, const std::string& sender, const std::string& msg, const std::string& attachmentId = "", int replyTo = 0);
        int GetServerIdForChannel(int cid);
        std::vector<std::string> GetUsersInServerByChannel(int channelId);
        uint32_t GetUserPermissions(int serverId, const std::string& username);
        bool DeleteMessage(int msgId, int cid, const std::string& username);
        bool EditMessage(int msgId, const std::string& username, const std::string& newContent);
        bool PinMessage(int msgId, int cid, const std::string& username, bool pinState);
        std::vector<std::string> GetServerMembers(int serverId);
        bool DeleteChannel(int channelId, const std::string& username);

        bool AddReaction(int messageId, const std::string& username, const std::string& emoji);
        bool RemoveReaction(int messageId, const std::string& username, const std::string& emoji);
        std::string GetReactionsJSON(int messageId);

    private:
        void Enqueue(std::function<void()> task);
        void WorkerLoop();

        sqlite3* m_Db;
        std::shared_mutex m_RwMutex;
        std::thread m_Worker;
        std::mutex m_QueueMutex;
        std::condition_variable m_QueueCv;
        std::queue<std::function<void()>> m_TaskQueue;
        bool m_Shutdown = false;
    };

} // namespace TalkMe