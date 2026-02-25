#pragma once
#include <memory>
#include <vector>
#include <deque>
#include <string>
#include <chrono>
#include <atomic>
#include <fstream>
#include "Protocol.h"
#include <asio.hpp>

namespace TalkMe {
    class TalkMeServer;
}

namespace TalkMe {

    class ChatSession : public std::enable_shared_from_this<ChatSession> {
    public:
        ChatSession(asio::ip::tcp::socket socket, TalkMeServer& server);

        void Start();
        int GetVoiceChannelId() const { return m_CurrentVoiceCid.load(std::memory_order_relaxed); }
        const std::string& GetUsername() const { return m_Username; }
        bool IsHealthy() const { return m_IsHealthy.load(std::memory_order_relaxed); }
        std::chrono::steady_clock::time_point GetLastVoicePacketTime() const { return m_LastVoicePacket; }
        int64_t GetLastActivityTimeMs() const { return m_LastActivityTimeMs.load(std::memory_order_relaxed); }
        void SetVoiceLoad(size_t load) { m_CurrentVoiceLoad.store(std::max<size_t>(1, load), std::memory_order_relaxed); }

        void UpdateActivity();
        void TouchVoiceActivity();
        void SendShared(std::shared_ptr<std::vector<uint8_t>> buffer, bool isVoiceData);

    private:
        void ReadHeader();
        void ReadBody();
        void ProcessPacket();
        void DoWrite();
        void Disconnect();

        asio::ip::tcp::socket m_Socket;
        TalkMeServer& m_Server;
        asio::strand<asio::any_io_executor> m_Strand;
        TalkMe::PacketHeader m_Header;
        std::vector<uint8_t> m_Body;
        std::deque<std::shared_ptr<std::vector<uint8_t>>> m_WriteQueue;
        std::atomic<int> m_CurrentVoiceCid{ -1 };
        std::string m_Username;

        std::atomic<bool> m_IsHealthy{ true };
        std::atomic<size_t> m_CurrentVoiceLoad{ 1 };
        std::atomic<int64_t> m_LastActivityTimeMs{ 0 };

        std::chrono::steady_clock::time_point m_LastVoicePacket;
        int m_VoicePacketCount = 0;

        double m_LastJitterMs = 0.0;
        int m_ConsecutiveStableReports = 0;
        uint32_t m_CurrentAssignedBitrateKbps = 48;

        std::ofstream m_UploadFile;
        std::string m_UploadId;
        size_t m_UploadBytesReceived{ 0 };
        size_t m_UploadTargetSize{ 0 };

        std::string m_Pending2FASecret;
        std::string m_PendingHWID;
    };

} // namespace TalkMe