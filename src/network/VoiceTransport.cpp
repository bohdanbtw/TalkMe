#include "VoiceTransport.h"
#include "../core/Logger.h"
#include "../shared/Protocol.h"
#include <thread>
#include <cstdint>
#include <cstring>
#include <cstdio>

using asio::ip::udp;

namespace TalkMe {

    namespace {

        constexpr uint8_t kVoicePacket = 0;
        constexpr uint8_t kHelloPacket = 1;
        constexpr uint8_t kPingPacket = 2;
        constexpr uint8_t kPongPacket = 3;
        constexpr size_t  kPingPayloadSize = 8;

        // Single-point error logger shared by all send/receive paths.
        inline void LogError(const char* step, const char* detail) noexcept {
            char buf[256];
            std::snprintf(buf, sizeof(buf), "step=%s msg=%.200s", step, detail);
            Logger::Instance().LogVoiceTraceBuf(buf);
        }

        inline void LogError(const char* step, const std::error_code& ec) noexcept {
            LogError(step, ec.message().c_str());
        }

    } // namespace

    VoiceTransport::VoiceTransport() : m_Socket(m_Context) {}
    VoiceTransport::~VoiceTransport() { Stop(); }

    bool VoiceTransport::Start(const std::string& serverIp, uint16_t port) {
        try {
            // Restart the context so Start() is safe to call after a previous Stop().
            // io_context::restart() is a no-op if the context was never run.
            m_Context.restart();

            if (!m_Socket.is_open()) m_Socket.open(udp::v4());
            m_Socket.bind(udp::endpoint(udp::v4(), 0));
            m_Socket.set_option(asio::socket_base::receive_buffer_size(256 * 1024));
            m_Socket.set_option(asio::socket_base::send_buffer_size(64 * 1024));

            m_RemoteEndpoint = udp::endpoint(
                asio::ip::address::from_string(serverIp), port);

            m_Running.store(true);

            m_Thread = std::thread([this] {
                try {
                    auto guard = asio::make_work_guard(m_Context);
                    DoReceive();
                    m_Context.run();
                }
                catch (const std::exception& e) { LogError("udp_error", e.what()); }
                catch (...) { LogError("udp_error", "unknown_exception"); }
                m_Running.store(false);
                });
            return true;
        }
        catch (const std::exception& e) { LogError("udp_start_error", e.what());          return false; }
        catch (...) { LogError("udp_start_error", "unknown_exception"); return false; }
    }

    void VoiceTransport::Stop() {
        m_Running.store(false);
        try {
            std::error_code ec;
            if (m_Socket.is_open()) m_Socket.close(ec);
            m_Context.stop();
            if (m_Thread.joinable()) m_Thread.join();
        }
        catch (const std::exception& e) { LogError("udp_stop_error", e.what()); }
        catch (...) { LogError("udp_stop_error", "unknown_exception"); }
    }

    void VoiceTransport::SendVoicePacket(const std::vector<uint8_t>& payload) {
        if (!m_Socket.is_open()) return;
        try {
            // Scatter-gather: send the type byte and payload as separate buffers to
            // avoid a heap allocation on this hot path (50-100 calls/sec per speaker).
            static constexpr uint8_t kTypeVoice = kVoicePacket;
            const std::array<asio::const_buffer, 2> bufs = {
                asio::buffer(&kTypeVoice, 1),
                asio::buffer(payload)
            };
            std::error_code ec;
            m_Socket.send_to(bufs, m_RemoteEndpoint, 0, ec);
            if (ec) LogError("udp_send_error", ec);
        }
        catch (const std::exception& e) { LogError("udp_send_error", e.what()); }
        catch (...) { LogError("udp_send_error", "unknown_exception"); }
    }

    void VoiceTransport::SendRaw(const std::vector<uint8_t>& payload) {
        if (!m_Socket.is_open() || payload.empty()) return;
        try {
            std::error_code ec;
            m_Socket.send_to(asio::buffer(payload), m_RemoteEndpoint, 0, ec);
            if (ec) LogError("udp_send_error", ec);
        }
        catch (const std::exception& e) { LogError("udp_send_error", e.what()); }
        catch (...) { LogError("udp_send_error", "unknown_exception"); }
    }

    void VoiceTransport::SendHello(const std::string& username, int voiceChannelId) {
        if (!m_Socket.is_open() || username.empty() || username.size() > 255) return;
        try {
            // Max hello packet: 1 (type) + 1 (ulen) + 255 (name) + 4 (cid) = 261 bytes.
            // Stack allocation only — no heap.
            uint8_t buf[261];
            size_t  off = 0;
            buf[off++] = kHelloPacket;
            buf[off++] = static_cast<uint8_t>(username.size());
            std::memcpy(buf + off, username.data(), username.size());
            off += username.size();
            const uint32_t cid = HostToNet32(static_cast<uint32_t>(voiceChannelId));
            std::memcpy(buf + off, &cid, 4);
            off += 4;

            std::error_code ec;
            m_Socket.send_to(asio::buffer(buf, off), m_RemoteEndpoint, 0, ec);
            if (ec) LogError("udp_send_error", ec);
        }
        catch (const std::exception& e) { LogError("udp_send_error", e.what()); }
        catch (...) { LogError("udp_send_error", "unknown_exception"); }
    }

    void VoiceTransport::SendPing() {
        if (!m_Socket.is_open()) return;
        try {
            // Fixed-size ping: 1 byte type + 8 bytes timestamp = 9 bytes total.
            // Stack allocation only — no heap.
            uint8_t buf[9];
            buf[0] = kPingPacket;
            const uint64_t ts = HostToNet64(static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count()));
            std::memcpy(buf + 1, &ts, 8);

            std::error_code ec;
            m_Socket.send_to(asio::buffer(buf, 9), m_RemoteEndpoint, 0, ec);
            if (ec) LogError("udp_send_error", ec);
        }
        catch (const std::exception& e) { LogError("udp_send_error", e.what()); }
        catch (...) { LogError("udp_send_error", "unknown_exception"); }
    }

    float VoiceTransport::GetRttMs() const {
        std::lock_guard<std::mutex> lk(m_RttMutex);
        return m_LastRttMs;
    }

    float VoiceTransport::GetLastRttMs() const {
        std::lock_guard<std::mutex> lk(m_RttMutex);
        return m_LastRawRttMs;
    }

    std::vector<float> VoiceTransport::GetPingHistory() const {
        std::lock_guard<std::mutex> lk(m_RttMutex);
        return { m_PingHistory.begin(), m_PingHistory.end() };
    }

    void VoiceTransport::DoReceive() {
        if (!m_Running.load(std::memory_order_relaxed)) return;

        m_Socket.async_receive_from(
            asio::buffer(m_RecvArray), m_SenderEndpoint,
            [this](std::error_code ec, std::size_t n) {
                if (ec) {
                    if (ec != asio::error::operation_aborted) {
                        m_Running.store(false);
                        LogError("udp_recv_error", ec);
                    }
                    return; // do not reschedule after a hard error
                }

                try {
                if (n > 1) {
                    const uint8_t kind = m_RecvArray[0];

                    if (kind == kVoicePacket && m_Callback) {
                        // Pass a raw pointer directly into the receive buffer.
                        // The callback must not hold the pointer past its own stack frame.
                        m_Callback(m_RecvArray.data() + 1, n - 1);
                        // Do NOT wake the main thread for every voice packet (~100/sec). Voice is
                        // handled here on the UDP thread; waking main 100/sec caused ~46% CPU on VM.
                    }
                    else if (kind == kPongPacket && n >= 1 + kPingPayloadSize) {
                        const uint64_t sentTs = ReadU64BE(m_RecvArray.data() + 1);
                        const uint64_t nowMs = static_cast<uint64_t>(
                            std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now().time_since_epoch()).count());
                        const float rtt = static_cast<float>(nowMs - sentTs);
                        if (rtt >= 0.0f && rtt <= 60000.0f) {
                            std::lock_guard<std::mutex> lk(m_RttMutex);
                            m_LastRttMs = (m_LastRttMs <= 0.0f)
                                ? rtt
                                : (m_LastRttMs * 0.85f + rtt * 0.15f);
                            m_LastRawRttMs = rtt;
                            m_PingHistory.push_back(rtt);
                            while (m_PingHistory.size() > kPingHistorySize)
                                m_PingHistory.pop_front();
                        }
                        if (m_WakeCallback) m_WakeCallback();
                    }
                }
                }
                catch (...) {
                    // Do not let callback or wake escape — would call std::terminate().
                }

                DoReceive();
            });
    }

} // namespace TalkMe