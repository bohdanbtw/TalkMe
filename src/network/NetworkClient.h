#pragma once
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <deque>
#include <functional>
#include <memory>
#include "../shared/Protocol.h"

namespace TalkMe {

    struct IncomingMessage {
        PacketType type;
        std::string data;
    };

    class NetworkClient {
    public:
        NetworkClient();
        ~NetworkClient();

        bool Connect(const std::string& host, int port);
        void ConnectAsync(const std::string& host, int port, std::function<void(bool)> onResult);
        void Disconnect();
        bool IsConnected() const;
        void Send(PacketType type, const std::string& data);

        std::vector<IncomingMessage> FetchMessages();

        // ✅ NEW: Direct callback for voice (runs on network thread)
        void SetVoiceCallback(std::function<void(const std::string&)> callback);

    private:
        // PIMPL to avoid including heavy headers in this public header (asio.hpp)
        struct Impl;
        std::unique_ptr<Impl> m_Impl;

        // Stored in header because it's small and used by UI code
        std::function<void(const std::string&)> m_VoiceCallback; // ✅

        // Internal helpers (keep declarations here so cpp definitions compile)
        void StartWorker();
        void ReadHeader();
        void ReadBody();
    };
}