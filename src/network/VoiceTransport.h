#pragma once

#include <string>
#include <vector>
#include <deque>
#include <functional>
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>
#include <array>
#include <asio.hpp>

namespace TalkMe {

    class VoiceTransport {
    public:
        VoiceTransport();
        ~VoiceTransport();

        VoiceTransport(const VoiceTransport&) = delete;
        VoiceTransport& operator=(const VoiceTransport&) = delete;

        // Start the UDP receive loop. Safe to call again after Stop() — the io_context
        // is restarted internally so a second Start() behaves identically to the first.
        bool Start(const std::string& serverIp, uint16_t port);
        void Stop();

        // All Send* methods are synchronous (blocking send_to). They run on the
        // calling thread — do not call from a UI or audio callback expecting low jitter.
        void SendVoicePacket(const std::vector<uint8_t>& payload);
        void SendRaw(const std::vector<uint8_t>& payload);
        void SendHello(const std::string& username, int voiceChannelId);
        void SendPing();

        bool IsRunning() const { return m_Running.load(std::memory_order_relaxed); }

        // Thread-safe RTT accessors (EWMA-smoothed and last raw sample).
        float GetRttMs() const;
        float GetLastRttMs() const;
        std::vector<float> GetPingHistory() const;

        // Callback receives a raw pointer + length into the receive buffer.
        // The data is valid only for the duration of the call — copy if it must outlive it.
        using ReceiveCallback = std::function<void(const uint8_t* data, size_t length)>;
        void SetReceiveCallback(ReceiveCallback cb) { m_Callback = std::move(cb); }

        // Optional: called on the ASIO thread whenever a UDP packet arrives.
        void SetWakeCallback(std::function<void()> cb) { m_WakeCallback = std::move(cb); }

    private:
        void DoReceive();

        static constexpr size_t kRecvBufferSize = 65536;
        static constexpr size_t kPingHistorySize = 60;

        asio::io_context                     m_Context;
        asio::ip::udp::socket                m_Socket;
        asio::ip::udp::endpoint              m_RemoteEndpoint;
        std::array<uint8_t, kRecvBufferSize> m_RecvArray{};
        asio::ip::udp::endpoint              m_SenderEndpoint;
        std::thread                          m_Thread;
        ReceiveCallback                      m_Callback;
        std::function<void()>                m_WakeCallback;
        std::atomic<bool>                    m_Running{ false };

        mutable std::mutex m_RttMutex;
        float              m_LastRttMs = 0.0f;
        float              m_LastRawRttMs = 0.0f;
        std::deque<float>  m_PingHistory;
    };

} // namespace TalkMe