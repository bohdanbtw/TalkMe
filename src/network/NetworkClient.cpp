#include "NetworkClient.h"
#include <asio.hpp>
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

        asio::io_context m_Context;
        asio::ip::tcp::socket m_Socket{ m_Context };
        std::thread m_ContextThread;
        std::atomic<bool> m_IsConnected{false};

        PacketHeader m_InHeader;
        std::vector<uint8_t> m_InBody;

        std::deque<IncomingMessage> m_IncomingQueue;
        std::mutex m_QueueMutex;

        std::function<void(const std::vector<uint8_t>&)> m_VoiceCallback;
    };

    NetworkClient::NetworkClient() : m_Impl(std::make_unique<Impl>()) {}

    NetworkClient::~NetworkClient() {
        Disconnect();
    }

    void NetworkClient::SetVoiceCallback(std::function<void(const std::vector<uint8_t>&)> callback) {
        if (m_Impl) {
            std::lock_guard<std::mutex> lock(m_Impl->m_QueueMutex);
            m_Impl->m_VoiceCallback = callback;
        }
    }

    bool NetworkClient::Connect(const std::string& host, int port) {
        std::promise<bool> p;
        auto f = p.get_future();
        ConnectAsync(host, port, [&](bool success) { p.set_value(success); });
        f.wait();
        return f.get();
    }

    void NetworkClient::ConnectAsync(const std::string& host, int port, std::function<void(bool)> onResult) {
        if (m_Impl->m_IsConnected.load()) {
            if (onResult) onResult(true);
            return;
        }

        std::thread([this, host, port, onResult]() {
            try {
                if (m_Impl->m_ContextThread.joinable()) {
                    m_Impl->m_Context.stop();
                    m_Impl->m_ContextThread.join();
                }
                m_Impl->m_Context.restart();

                // Fresh socket for each connection attempt
                m_Impl->m_Socket = asio::ip::tcp::socket(m_Impl->m_Context);

                asio::ip::tcp::resolver resolver(m_Impl->m_Context);
                auto endpoints = resolver.resolve(host, std::to_string(port));
                asio::connect(m_Impl->m_Socket, endpoints);

                ReadHeader();

                m_Impl->m_ContextThread = std::thread([this]() {
                    auto work_guard = asio::make_work_guard(m_Impl->m_Context);
                    try {
                        m_Impl->m_Context.run();
                    }
                    catch (const std::exception&) {
                    }
                    m_Impl->m_IsConnected.store(false);
                });

                // Synchronize: block until run() is actively processing handlers
                auto ready = std::make_shared<std::promise<void>>();
                auto future = ready->get_future();
                asio::post(m_Impl->m_Context, [ready]() { ready->set_value(); });
                future.wait();

                m_Impl->m_IsConnected.store(true);

                // Fire callback on the ASIO thread so Send() works from inside it
                if (onResult) {
                    asio::post(m_Impl->m_Context, [onResult]() { onResult(true); });
                }
            }
            catch (const std::exception&) {
                m_Impl->m_IsConnected.store(false);
                if (onResult) onResult(false);
            }
        }).detach();
    }

    // Safe to call from ASIO callbacks (runs on context thread, never joins itself)
    void NetworkClient::CloseSocket() {
        m_Impl->m_IsConnected.store(false);
        if (m_Impl->m_Socket.is_open()) {
            std::error_code ec;
            m_Impl->m_Socket.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
            m_Impl->m_Socket.close(ec);
        }
    }

    // Call from main/UI thread only
    void NetworkClient::Disconnect() {
        if (!m_Impl) return;
        m_Impl->m_IsConnected.store(false);

        asio::post(m_Impl->m_Context, [this]() { CloseSocket(); });

        if (m_Impl->m_ContextThread.joinable()) {
            m_Impl->m_Context.stop();
            m_Impl->m_ContextThread.join();
        }
    }

    bool NetworkClient::IsConnected() const {
        return m_Impl && m_Impl->m_IsConnected.load();
    }

    void NetworkClient::Send(PacketType type, const std::string& data) {
        if (!IsConnected()) return;

        auto buffer = std::make_shared<std::vector<uint8_t>>();
        uint32_t size = static_cast<uint32_t>(data.size());

        buffer->resize(sizeof(PacketHeader) + size);
        PacketHeader header = { type, size };

        std::memcpy(buffer->data(), &header, sizeof(header));
        if (size > 0)
            std::memcpy(buffer->data() + sizeof(header), data.data(), size);

        asio::post(m_Impl->m_Context, [this, buffer]() {
            asio::async_write(m_Impl->m_Socket, asio::buffer(*buffer),
                [this, buffer](std::error_code ec, std::size_t) {
                    if (ec) CloseSocket();
                });
        });
    }

    void NetworkClient::SendRaw(PacketType type, const std::vector<uint8_t>& data) {
        if (!IsConnected()) return;

        auto buffer = std::make_shared<std::vector<uint8_t>>();
        uint32_t size = static_cast<uint32_t>(data.size());

        buffer->resize(sizeof(PacketHeader) + size);
        PacketHeader header = { type, size };

        std::memcpy(buffer->data(), &header, sizeof(header));
        if (size > 0)
            std::memcpy(buffer->data() + sizeof(header), data.data(), size);

        asio::post(m_Impl->m_Context, [this, buffer]() {
            asio::async_write(m_Impl->m_Socket, asio::buffer(*buffer),
                [this, buffer](std::error_code ec, std::size_t) {
                    if (ec) CloseSocket();
                });
        });
    }

    std::vector<IncomingMessage> NetworkClient::FetchMessages() {
        std::lock_guard<std::mutex> lock(m_Impl->m_QueueMutex);
        if (m_Impl->m_IncomingQueue.empty()) return {};

        std::vector<IncomingMessage> msgs(m_Impl->m_IncomingQueue.begin(), m_Impl->m_IncomingQueue.end());
        m_Impl->m_IncomingQueue.clear();
        return msgs;
    }

    void NetworkClient::ReadHeader() {
        asio::async_read(m_Impl->m_Socket,
            asio::buffer(&m_Impl->m_InHeader, sizeof(PacketHeader)),
            [this](std::error_code ec, std::size_t) {
                if (!ec) {
                    if (m_Impl->m_InHeader.size > 10 * 1024 * 1024) {
                        CloseSocket();
                        return;
                    }
                    m_Impl->m_InBody.resize(m_Impl->m_InHeader.size);
                    ReadBody();
                } else {
                    CloseSocket();
                }
            });
    }

    void NetworkClient::ReadBody() {
        asio::async_read(m_Impl->m_Socket,
            asio::buffer(m_Impl->m_InBody),
            [this](std::error_code ec, std::size_t) {
                if (!ec) {
                    if (m_Impl->m_InHeader.type == PacketType::Voice_Data_Opus ||
                        m_Impl->m_InHeader.type == PacketType::Voice_Data)
                    {
                        if (m_Impl->m_VoiceCallback)
                            m_Impl->m_VoiceCallback(m_Impl->m_InBody);
                    } else {
                        std::lock_guard<std::mutex> lock(m_Impl->m_QueueMutex);
                        IncomingMessage msg;
                        msg.type = m_Impl->m_InHeader.type;
                        msg.data = m_Impl->m_InBody;
                        m_Impl->m_IncomingQueue.push_back(msg);
                    }
                    ReadHeader();
                } else {
                    CloseSocket();
                }
            });
    }

} // namespace TalkMe
