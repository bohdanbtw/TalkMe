#include "VoiceTransport.h"
#include "../core/Logger.h"
#include <thread>
#include <cstdint>
#include <cstring>

using asio::ip::udp;

namespace TalkMe {
    namespace {
        constexpr uint8_t kVoicePacket = 0;
        constexpr uint8_t kHelloPacket = 1;
        constexpr uint8_t kPingPacket = 2;
        constexpr uint8_t kPongPacket = 3;
        constexpr size_t kPingPayloadSize = 8;

        void AppendI32BE(std::vector<uint8_t>& out, int32_t v) {
            out.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
            out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
            out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
            out.push_back(static_cast<uint8_t>(v & 0xFF));
        }
        void AppendU64BE(std::vector<uint8_t>& out, uint64_t v) {
            for (int i = 7; i >= 0; --i)
                out.push_back(static_cast<uint8_t>((v >> (i * 8)) & 0xFF));
        }
        uint64_t ReadU64BE(const uint8_t* p) {
            uint64_t v = 0;
            for (int i = 0; i < 8; ++i) v = (v << 8) | p[i];
            return v;
        }
    } // namespace

    VoiceTransport::VoiceTransport() : m_Socket(m_Context), m_RecvBuffer(65536) {}
    VoiceTransport::~VoiceTransport() { Stop(); }

    bool VoiceTransport::Start(const std::string& serverIp, uint16_t port) {
        try {
            udp::endpoint local_endpoint(udp::v4(), 0);
            m_Socket.open(udp::v4());
            m_Socket.bind(local_endpoint);
            m_RemoteEndpoint = udp::endpoint(asio::ip::address::from_string(serverIp), port);
            m_Running.store(true);
            m_Thread = std::thread([this]() {
                try {
                    DoReceive();
                    m_Context.run();
                }
                catch (const std::exception& e) {
                    Logger::Instance().LogVoiceTrace("step=udp_error msg=" + std::string(e.what()));
                    m_Running.store(false);
                }
                catch (...) {
                    Logger::Instance().LogVoiceTrace("step=udp_error msg=unknown_exception");
                    m_Running.store(false);
                }
            });
            return true;
        }
        catch (const std::exception& e) {
            Logger::Instance().LogVoiceTrace("step=udp_start_error msg=" + std::string(e.what()));
            return false;
        }
        catch (...) {
            Logger::Instance().LogVoiceTrace("step=udp_start_error msg=unknown_exception");
            return false;
        }
    }

    void VoiceTransport::Stop() {
        m_Running.store(false);
        try {
            if (m_Socket.is_open()) m_Socket.close();
            m_Context.stop();
            if (m_Thread.joinable()) m_Thread.join();
        }
        catch (const std::exception& e) {
            Logger::Instance().LogVoiceTrace("step=udp_stop_error msg=" + std::string(e.what()));
        }
        catch (...) {
            Logger::Instance().LogVoiceTrace("step=udp_stop_error msg=unknown_exception");
        }
    }

    void VoiceTransport::SendVoicePacket(const std::vector<uint8_t>& payload) {
        if (!m_Socket.is_open()) return;
        try {
            std::vector<uint8_t> udpPacket;
            udpPacket.reserve(payload.size() + 1);
            udpPacket.push_back(kVoicePacket);
            udpPacket.insert(udpPacket.end(), payload.begin(), payload.end());
            m_Socket.send_to(asio::buffer(udpPacket), m_RemoteEndpoint);
        }
        catch (const std::exception& e) {
            Logger::Instance().LogVoiceTrace("step=udp_send_error msg=" + std::string(e.what()));
        }
        catch (...) {
            Logger::Instance().LogVoiceTrace("step=udp_send_error msg=unknown_exception");
        }
    }

    void VoiceTransport::SendRaw(const std::vector<uint8_t>& payload) {
        if (!m_Socket.is_open() || payload.empty()) return;
        try {
            m_Socket.send_to(asio::buffer(payload), m_RemoteEndpoint);
        }
        catch (const std::exception& e) {
            Logger::Instance().LogVoiceTrace("step=udp_send_error msg=" + std::string(e.what()));
        }
        catch (...) {
            Logger::Instance().LogVoiceTrace("step=udp_send_error msg=unknown_exception");
        }
    }

    void VoiceTransport::SendHello(const std::string& username, int voiceChannelId) {
        if (!m_Socket.is_open() || username.empty() || username.size() > 255) return;
        try {
            std::vector<uint8_t> hello;
            hello.reserve(1 + 1 + username.size() + 4);
            hello.push_back(kHelloPacket);
            hello.push_back(static_cast<uint8_t>(username.size()));
            hello.insert(hello.end(), username.begin(), username.end());
            AppendI32BE(hello, voiceChannelId);
            m_Socket.send_to(asio::buffer(hello), m_RemoteEndpoint);
        }
        catch (const std::exception& e) {
            Logger::Instance().LogVoiceTrace("step=udp_send_error msg=" + std::string(e.what()));
        }
        catch (...) {
            Logger::Instance().LogVoiceTrace("step=udp_send_error msg=unknown_exception");
        }
    }

    void VoiceTransport::SendPing() {
        if (!m_Socket.is_open()) return;
        try {
            uint64_t ts = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
            std::vector<uint8_t> ping;
            ping.reserve(1 + kPingPayloadSize);
            ping.push_back(kPingPacket);
            AppendU64BE(ping, ts);
            m_Socket.send_to(asio::buffer(ping), m_RemoteEndpoint);
        }
        catch (const std::exception& e) {
            Logger::Instance().LogVoiceTrace("step=udp_send_error msg=" + std::string(e.what()));
        }
        catch (...) {
            Logger::Instance().LogVoiceTrace("step=udp_send_error msg=unknown_exception");
        }
    }

    float VoiceTransport::GetRttMs() const {
        std::lock_guard<std::mutex> lock(m_RttMutex);
        return m_LastRttMs;
    }

    float VoiceTransport::GetLastRttMs() const {
        std::lock_guard<std::mutex> lock(m_RttMutex);
        return m_LastRawRttMs;
    }

    std::vector<float> VoiceTransport::GetPingHistory() const {
        std::lock_guard<std::mutex> lock(m_RttMutex);
        return std::vector<float>(m_PingHistory.begin(), m_PingHistory.end());
    }

    void VoiceTransport::DoReceive() {
        while (m_Running.load(std::memory_order_relaxed)) {
            try {
                asio::ip::udp::endpoint sender;
                size_t n = m_Socket.receive_from(asio::buffer(m_RecvBuffer), sender);
                if (n > 1) {
                    uint8_t packetKind = m_RecvBuffer[0];
                    if (packetKind == kVoicePacket) {
                        std::vector<uint8_t> packet(m_RecvBuffer.begin() + 1, m_RecvBuffer.begin() + n);
                        if (m_Callback) m_Callback(packet);
                    }
                    else if (packetKind == kPongPacket && n >= 1 + kPingPayloadSize) {
                        uint64_t sentTs = ReadU64BE(&m_RecvBuffer[1]);
                        uint64_t nowMs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch()).count());
                        float rtt = static_cast<float>(nowMs - sentTs);
                        if (rtt >= 0.0f && rtt <= 60000.0f) {
                            std::lock_guard<std::mutex> lock(m_RttMutex);
                            m_LastRttMs = (m_LastRttMs <= 0.0f) ? rtt : (m_LastRttMs * 0.85f + rtt * 0.15f);
                            m_LastRawRttMs = rtt;
                            m_PingHistory.push_back(rtt);
                            while (m_PingHistory.size() > kPingHistorySize) m_PingHistory.pop_front();
                        }
                    }
                }
            }
            catch (const std::exception& e) {
                m_Running.store(false);
                Logger::Instance().LogVoiceTrace("step=udp_recv_error msg=" + std::string(e.what()));
            }
            catch (...) {
                m_Running.store(false);
                Logger::Instance().LogVoiceTrace("step=udp_recv_error msg=unknown_exception");
            }
        }
    }

}
