#pragma once
#include <string>
#include <vector>
#include <deque>
#include <functional>
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>
#include <asio.hpp>

namespace TalkMe {
    class VoiceTransport {
    public:
        VoiceTransport();
        ~VoiceTransport();

        bool Start(const std::string& serverIp, uint16_t port);
        void Stop();

        void SendVoicePacket(const std::vector<uint8_t>& payload);
        void SendRaw(const std::vector<uint8_t>& payload);
        void SendHello(const std::string& username, int voiceChannelId);
        void SendPing();
        bool IsRunning() const { return m_Running; }
        float GetRttMs() const;
        float GetLastRttMs() const;
        std::vector<float> GetPingHistory() const;

        void SetReceiveCallback(std::function<void(const std::vector<uint8_t>&)> cb) { m_Callback = cb; }

    private:
        void DoReceive();

        asio::io_context m_Context;
        asio::ip::udp::socket m_Socket{m_Context};
        asio::ip::udp::endpoint m_RemoteEndpoint;
        std::vector<uint8_t> m_RecvBuffer;
        std::thread m_Thread;
        std::function<void(const std::vector<uint8_t>&)> m_Callback;
        std::atomic<bool> m_Running{false};

        mutable std::mutex m_RttMutex;
        float m_LastRttMs = 0.0f;
        float m_LastRawRttMs = 0.0f;
        std::deque<float> m_PingHistory;
        static constexpr size_t kPingHistorySize = 60;
    };
}
