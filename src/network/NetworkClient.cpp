#include "NetworkClient.h"
#include <asio.hpp>
#include <windows.h>
#include <array>
#include <memory>
#include <vector>
#include <cstring>
#include <future>
#include <deque>
#include <mutex>
#include <thread>
#include <atomic>

namespace TalkMe {

    struct NetworkClient::Impl {
        Impl() : m_Socket(m_Context) {}
        asio::io_context          m_Context;
        asio::ip::tcp::socket     m_Socket{ m_Context };
        std::thread               m_ContextThread;
        std::atomic<bool>         m_IsConnected{ false };
        std::atomic<bool>         m_Connecting{ false };
        PacketHeader              m_InHeader{};
        std::vector<uint8_t>      m_InBody;
        std::deque<IncomingMessage> m_IncomingQueue;
        std::mutex                  m_QueueMutex;
        struct OutPacket {
            PacketHeader         header;
            std::vector<uint8_t> body;
        };
        std::deque<std::shared_ptr<OutPacket>> m_WriteQueue;
        using VoiceCallbackFn = std::function<void(const std::vector<uint8_t>&)>;
        std::atomic<std::shared_ptr<VoiceCallbackFn>> m_VoiceCallback;
        HANDLE m_WakeEvent = nullptr;
        void DoWrite(NetworkClient* parent) {
            if (m_WriteQueue.empty()) return;
            auto pkt = m_WriteQueue.front();
            std::array<asio::const_buffer, 2> bufs = {
                asio::buffer(&pkt->header, sizeof(pkt->header)),
                asio::buffer(pkt->body)
            };
            asio::async_write(m_Socket, bufs,
                [this, parent](std::error_code ec, std::size_t) {
                    if (ec) {
                        parent->CloseSocket();
                    } else {
                        m_WriteQueue.pop_front();
                        DoWrite(parent);
                    }
                });
        }
    };

    NetworkClient::NetworkClient() : m_Impl(std::make_unique<Impl>()) {}

    NetworkClient::~NetworkClient() {
        if (m_ConnectThread.joinable()) m_ConnectThread.join();
        Disconnect();
    }

    void NetworkClient::SetVoiceCallback(
        std::function<void(const std::vector<uint8_t>&)> callback) {
        if (m_Impl)
            m_Impl->m_VoiceCallback.store(
                std::make_shared<Impl::VoiceCallbackFn>(std::move(callback)));
    }

    void NetworkClient::SetWakeEvent(void* handle) {
        if (m_Impl) m_Impl->m_WakeEvent = static_cast<HANDLE>(handle);
    }

    bool NetworkClient::Connect(const std::string& host, int port) {
        std::promise<bool> p;
        auto f = p.get_future();
        ConnectAsync(host, port, [&](bool success) { p.set_value(success); });
        // get() blocks until the connect thread calls set_value(); no extra wait() needed.
        return f.get();
    }

    void NetworkClient::ConnectAsync(const std::string& host, int port,
        std::function<void(bool)> onResult) {
        if (m_Impl->m_IsConnected.load()) {
            if (onResult) onResult(true);
            return;
        }
        if (m_Impl->m_Connecting.exchange(true)) return;

        if (m_ConnectThread.joinable()) m_ConnectThread.join();

        m_ConnectThread = std::thread([this, host, port, onResult = std::move(onResult)] {
            try {
                if (m_Impl->m_ContextThread.joinable()) {
                    m_Impl->m_Context.stop();
                    m_Impl->m_ContextThread.join();
                }
                m_Impl->m_Context.restart();
                m_Impl->m_Socket = asio::ip::tcp::socket(m_Impl->m_Context);
                m_Impl->m_WriteQueue.clear();

                asio::ip::tcp::resolver resolver(m_Impl->m_Context);
                asio::connect(m_Impl->m_Socket, resolver.resolve(host, std::to_string(port)));

                ReadHeader();

                m_Impl->m_ContextThread = std::thread([this] {
                    auto guard = asio::make_work_guard(m_Impl->m_Context);
                    try { m_Impl->m_Context.run(); }
                    catch (...) {}
                    m_Impl->m_IsConnected.store(false);
                    });

                // Flush the post queue so all handlers are registered before we
                // mark the connection live and return to the caller.
                {
                    auto ready = std::make_shared<std::promise<void>>();
                    auto future = ready->get_future();
                    asio::post(m_Impl->m_Context, [ready] { ready->set_value(); });
                    future.wait();
                }

                m_Impl->m_IsConnected.store(true);

                // Invoke directly on this thread — avoids an extra ASIO post round-trip
                // and ensures callers using std::promise (i.e. Connect()) unblock promptly.
                if (onResult) onResult(true);
            }
            catch (...) {
                m_Impl->m_IsConnected.store(false);
                if (onResult) onResult(false);
            }
            m_Impl->m_Connecting.store(false);
            });
    }

    // Safe to call from ASIO callbacks (runs on the context thread; never joins itself).
    void NetworkClient::CloseSocket() {
        m_Impl->m_IsConnected.store(false);
        if (m_Impl->m_Socket.is_open()) {
            std::error_code ec;
            m_Impl->m_Socket.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
            m_Impl->m_Socket.close(ec);
        }
    }

    // Must be called from the main/UI thread only.  Closing the socket cancels all
    // pending async operations, which lets the context thread drain and exit cleanly.
    void NetworkClient::Disconnect() {
        if (!m_Impl) return;
        m_Impl->m_IsConnected.store(false);

        std::error_code ec;
        if (m_Impl->m_Socket.is_open()) {
            m_Impl->m_Socket.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
            m_Impl->m_Socket.close(ec);
        }
        m_Impl->m_Context.stop();
        if (m_Impl->m_ContextThread.joinable())
            m_Impl->m_ContextThread.join();
    }

    bool NetworkClient::IsConnected() const {
        return m_Impl && m_Impl->m_IsConnected.load();
    }

    void NetworkClient::Send(PacketType type, const std::string& data) {
        if (!IsConnected()) return;
        auto pkt = std::make_shared<Impl::OutPacket>();
        pkt->header = { type, static_cast<uint32_t>(data.size()) };
        pkt->header.ToNetwork();
        pkt->body.assign(
            reinterpret_cast<const uint8_t*>(data.data()),
            reinterpret_cast<const uint8_t*>(data.data()) + data.size());
        asio::post(m_Impl->m_Context, [this, pkt] {
            bool writeInProgress = !m_Impl->m_WriteQueue.empty();
            m_Impl->m_WriteQueue.push_back(pkt);
            if (!writeInProgress) m_Impl->DoWrite(this);
        });
    }

    void NetworkClient::SendRaw(PacketType type, const std::vector<uint8_t>& data) {
        if (!IsConnected()) return;
        auto pkt = std::make_shared<Impl::OutPacket>();
        pkt->header = { type, static_cast<uint32_t>(data.size()) };
        pkt->header.ToNetwork();
        pkt->body = data;
        asio::post(m_Impl->m_Context, [this, pkt] {
            bool writeInProgress = !m_Impl->m_WriteQueue.empty();
            m_Impl->m_WriteQueue.push_back(pkt);
            if (!writeInProgress) m_Impl->DoWrite(this);
        });
    }

    std::vector<IncomingMessage> NetworkClient::FetchMessages() {
        std::lock_guard<std::mutex> lock(m_Impl->m_QueueMutex);
        if (m_Impl->m_IncomingQueue.empty()) return {};
        std::vector<IncomingMessage> msgs(
            std::make_move_iterator(m_Impl->m_IncomingQueue.begin()),
            std::make_move_iterator(m_Impl->m_IncomingQueue.end()));
        m_Impl->m_IncomingQueue.clear();
        return msgs;
    }

    void NetworkClient::ReadHeader() {
        asio::async_read(m_Impl->m_Socket,
            asio::buffer(&m_Impl->m_InHeader, sizeof(PacketHeader)),
            [this](std::error_code ec, std::size_t) {
                if (ec) { CloseSocket(); return; }
                m_Impl->m_InHeader.ToHost();
                if (m_Impl->m_InHeader.size > 10u * 1024u * 1024u) {
                    CloseSocket();
                    return;
                }
                m_Impl->m_InBody.resize(m_Impl->m_InHeader.size);
                ReadBody();
            });
    }

    void NetworkClient::ReadBody() {
        asio::async_read(m_Impl->m_Socket,
            asio::buffer(m_Impl->m_InBody),
            [this](std::error_code ec, std::size_t) {
                if (ec) { CloseSocket(); return; }

                if (m_Impl->m_InHeader.type == PacketType::Voice_Data_Opus ||
                    m_Impl->m_InHeader.type == PacketType::Voice_Data)
                {
                    auto cb = m_Impl->m_VoiceCallback.load();
                    if (cb && *cb) {
                        try { (*cb)(m_Impl->m_InBody); }
                        catch (...) { /* do not let callback throw out of ASIO handler */ }
                    }
                }
                else {
                    // Move the body into the queue to avoid a redundant copy.
                    // m_InBody is immediately resized in the next ReadHeader() call.
                    IncomingMessage msg;
                    msg.type = m_Impl->m_InHeader.type;
                    msg.data = std::move(m_Impl->m_InBody);
                    std::lock_guard<std::mutex> lock(m_Impl->m_QueueMutex);
                    m_Impl->m_IncomingQueue.push_back(std::move(msg));
                }

                if (m_Impl->m_WakeEvent) ::SetEvent(m_Impl->m_WakeEvent);
                ReadHeader();
            });
    }

} // namespace TalkMe