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
        std::string GetMessageHistoryJSON(int channelId, int beforeId = 0, int limit = 50);
        // Envelope response for efficient paging. Supports:
        // - latest: beforeId=0, afterId=0, anchorId=0
        // - older:  beforeId>0
        // - newer:  afterId>0
        // - around: anchorId>0 with beforeLimit/afterLimit
        std::string GetMessageHistoryEnvelopeJSON(
            int channelId,
            int beforeId = 0,
            int afterId = 0,
            int anchorId = 0,
            int beforeLimit = 0,
            int afterLimit = 0,
            int limit = 50);
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

        bool BlockUser(const std::string& blocker, const std::string& blocked);
        bool UnblockUser(const std::string& blocker, const std::string& blocked);
        bool IsBlocked(const std::string& blocker, const std::string& blocked);
        void AddAuditLog(int serverId, const std::string& actor, const std::string& action, const std::string& target, const std::string& details);
        std::string GetAuditLogJSON(int serverId, int limit = 50);

        bool SetAvatar(const std::string& username, const std::string& avatarBase64);
        std::string GetAvatar(const std::string& username);

        std::string RegisterBot(int serverId, const std::string& owner, const std::string& botName);
        std::string GetServerBotsJSON(int serverId);

        bool AddSanction(int serverId, const std::string& username, const std::string& type, const std::string& reason, int durationMinutes, const std::string& createdBy);
        bool IsUserSanctioned(int serverId, const std::string& username, const std::string& type);
        bool RemoveSanction(int serverId, const std::string& username, const std::string& type);
        int CreateRole(int serverId, const std::string& name, uint32_t permissions, const std::string& color);
        bool AssignRole(const std::string& username, int roleId);
        std::string GetServerRolesJSON(int serverId);
        bool IsUserAdmin(int serverId, const std::string& username);

        bool RenameServer(int serverId, const std::string& newName, const std::string& username);
        bool DeleteServer(int serverId, const std::string& username);
        bool LeaveServer(const std::string& username, int serverId);
        bool SetMemberPermissions(int serverId, const std::string& targetUser, uint32_t permissions, const std::string& requestingUser);
        std::string GetServerOwner(int serverId);

        int SaveDirectMessage(const std::string& sender, const std::string& receiver, const std::string& content);
        std::string GetDMHistoryJSON(const std::string& user1, const std::string& user2);
        bool AreFriends(const std::string& user1, const std::string& user2);

        bool SendFriendRequest(const std::string& from, const std::string& toUsername);
        bool AcceptFriendRequest(const std::string& user, const std::string& friendUser);
        bool RejectOrRemoveFriend(const std::string& user, const std::string& friendUser);
        std::string GetFriendListJSON(const std::string& username);

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