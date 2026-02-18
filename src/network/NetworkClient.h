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
        std::vector<uint8_t> data;
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
        void SendRaw(PacketType type, const std::vector<uint8_t>& data);

        std::vector<IncomingMessage> FetchMessages();

        void SetVoiceCallback(std::function<void(const std::vector<uint8_t>&)> callback);

    private:
        struct Impl;
        std::unique_ptr<Impl> m_Impl;

        std::function<void(const std::vector<uint8_t>&)> m_VoiceCallback;

        void StartWorker();
        void ReadHeader();
        void ReadBody();
        void CloseSocket();
    };
}