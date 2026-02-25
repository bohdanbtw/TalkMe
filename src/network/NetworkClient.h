#pragma once

#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <deque>
#include <functional>
#include <memory>
#include <atomic>
#include "../shared/Protocol.h"

namespace TalkMe {

    struct IncomingMessage {
        PacketType           type;
        std::vector<uint8_t> data;
    };

    class NetworkClient {
    public:
        NetworkClient();
        ~NetworkClient();

        // Synchronous connect — blocks the calling thread until success or failure.
        bool Connect(const std::string& host, int port);

        // Asynchronous connect — onResult is invoked on the connect thread.
        void ConnectAsync(const std::string& host, int port,
            std::function<void(bool)> onResult);

        void Disconnect();
        bool IsConnected() const;

        void Send(PacketType type, const std::string& data);
        void SendRaw(PacketType type, const std::vector<uint8_t>& data);

        // Drain and return all messages received since the last call.
        std::vector<IncomingMessage> FetchMessages();

        void SetVoiceCallback(std::function<void(const std::vector<uint8_t>&)> callback);

        // Supply a Win32 HANDLE that is signaled whenever TCP data arrives.
        void SetWakeEvent(void* handle);

    private:
        struct Impl;
        std::unique_ptr<Impl> m_Impl;
        std::thread           m_ConnectThread;

        void ReadHeader();
        void ReadBody();
        void CloseSocket();
    };

} // namespace TalkMe