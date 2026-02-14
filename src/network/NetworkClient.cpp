#include "NetworkClient.h"
#include <asio.hpp>
#include <iostream>
#include <memory>
#include <vector>
#include <cstring>
#include <future>

namespace TalkMe {

    struct NetworkClient::Impl {
        Impl() : m_Socket(m_Context) {}

        asio::io_context m_Context;
        asio::ip::tcp::socket m_Socket{m_Context};
        std::thread m_ContextThread;
        bool m_IsConnected = false;

        PacketHeader m_InHeader;
        std::vector<char> m_InBody;

        std::deque<IncomingMessage> m_IncomingQueue;
        std::mutex m_QueueMutex;

        std::function<void(const std::string&)> m_VoiceCallback;
    };

    NetworkClient::NetworkClient() : m_Impl(std::make_unique<Impl>()) {}
    NetworkClient::~NetworkClient() { Disconnect(); }

    void NetworkClient::SetVoiceCallback(std::function<void(const std::string&)> callback) {
        m_VoiceCallback = callback;
        if (!m_Impl) return;
        // Propagate to impl on the io_context to be thread-safe
        try {
            asio::post(m_Impl->m_Context, [impl = m_Impl.get(), callback]() {
                impl->m_VoiceCallback = callback;
            });
        }
        catch (...) {
            // If posting fails (no context running), set directly
            m_Impl->m_VoiceCallback = callback;
        }
    }

    bool NetworkClient::Connect(const std::string& host, int port) {
        // Wrapper for blocking connect if needed
        std::promise<bool> p;
        auto f = p.get_future();
        ConnectAsync(host, port, [&](bool success) { p.set_value(success); });
        f.wait();
        return f.get();
    }

    void NetworkClient::ConnectAsync(const std::string& host, int port, std::function<void(bool)> onResult) {
        if (m_Impl->m_IsConnected) {
            if (onResult) onResult(true);
            return;
        }

        // Spawn a background thread for the connection process
        std::thread([this, host, port, onResult]() {
            try {
                // 1. Cleanup previous state
                if (m_Impl->m_ContextThread.joinable()) {
                    m_Impl->m_Context.stop();
                    m_Impl->m_ContextThread.join();
                    m_Impl->m_Context.restart();
                }

                // 2. Resolve & Connect (This blocks THIS thread, not the UI)
                asio::ip::tcp::resolver resolver(m_Impl->m_Context);
                auto endpoints = resolver.resolve(host, std::to_string(port));
                asio::connect(m_Impl->m_Socket, endpoints);

                m_Impl->m_IsConnected = true;

                // 3. Restart Context if needed
                if (m_Impl->m_Context.stopped()) m_Impl->m_Context.restart();

                // 4. Start Reading
                ReadHeader();

                // 5. Start IO Worker
                m_Impl->m_ContextThread = std::thread([this]() {
                    try { m_Impl->m_Context.run(); }
                    catch (...) {}
                    });

                // 6. Notify Success
                if (onResult) onResult(true);
            }
            catch (const std::exception& e) {
                std::cerr << "[Network] Connect failed: " << e.what() << std::endl;
                m_Impl->m_IsConnected = false;
                if (onResult) onResult(false);
            }
            }).detach();
    }

    void NetworkClient::Disconnect() {
        if (!m_Impl->m_IsConnected) return;
        m_Impl->m_IsConnected = false;

        asio::post(m_Impl->m_Context, [this]() {
            if (m_Impl->m_Socket.is_open()) m_Impl->m_Socket.close();
            });

        if (m_Impl->m_ContextThread.joinable()) {
            m_Impl->m_Context.stop();
            m_Impl->m_ContextThread.join();
        }
    }

    bool NetworkClient::IsConnected() const { return m_Impl->m_IsConnected; }

    void NetworkClient::Send(PacketType type, const std::string& data) {
        if (!m_Impl->m_IsConnected) return;

        asio::post(m_Impl->m_Context, [this, type, data]() {
            uint32_t size = (uint32_t)data.size();
            PacketHeader header = { type, size };
            auto buffer = std::make_shared<std::vector<uint8_t>>(sizeof(header) + size);
            std::memcpy(buffer->data(), &header, sizeof(header));
            if (size > 0) std::memcpy(buffer->data() + sizeof(header), data.data(), size);

            asio::async_write(m_Impl->m_Socket, asio::buffer(*buffer),
                [this, buffer](std::error_code ec, std::size_t) {
                    if (ec) { m_Impl->m_IsConnected = false; m_Impl->m_Socket.close(); }
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
        asio::async_read(m_Impl->m_Socket, asio::buffer(&m_Impl->m_InHeader, sizeof(PacketHeader)),
            [this](std::error_code ec, std::size_t) {
                if (!ec) {
                    if (m_Impl->m_InHeader.size > 10 * 1024 * 1024) { m_Impl->m_Socket.close(); return; }
                    m_Impl->m_InBody.resize(m_Impl->m_InHeader.size);
                    ReadBody();
                }
                else { m_Impl->m_IsConnected = false; m_Impl->m_Socket.close(); }
            });
    }

    void NetworkClient::ReadBody() {
        asio::async_read(m_Impl->m_Socket, asio::buffer(m_Impl->m_InBody),
            [this](std::error_code ec, std::size_t) {
                if (!ec) {
                    std::string data(m_Impl->m_InBody.begin(), m_Impl->m_InBody.end());

                    // ✅ CRITICAL Fix: Intercept Voice Packets
                    if ((m_Impl->m_InHeader.type == PacketType::Voice_Data_Opus ||
                        m_Impl->m_InHeader.type == PacketType::Voice_Data) && m_Impl->m_VoiceCallback) {
                        m_Impl->m_VoiceCallback(data);
                    }
                    else {
                        std::lock_guard<std::mutex> lock(m_Impl->m_QueueMutex);
                        m_Impl->m_IncomingQueue.push_back({ m_Impl->m_InHeader.type, data });
                    }

                    ReadHeader();
                }
                else {
                    m_Impl->m_IsConnected = false;
                    if (m_Impl->m_Socket.is_open()) m_Impl->m_Socket.close();
                }
            });
    }

}