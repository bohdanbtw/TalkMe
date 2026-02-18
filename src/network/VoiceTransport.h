#pragma once
#include <string>
#include <vector>
#include <functional>
#include <asio.hpp>

namespace TalkMe {
    class VoiceTransport {
    public:
        VoiceTransport();
        ~VoiceTransport();

        bool Start(const std::string& serverIp, uint16_t port);
        void Stop();

        void SendVoicePacket(const std::vector<uint8_t>& payload);

        void SetReceiveCallback(std::function<void(const std::vector<uint8_t>&)> cb) { m_Callback = cb; }

    private:
        void DoReceive();

        asio::io_context m_Context;
        asio::ip::udp::socket m_Socket{m_Context};
        asio::ip::udp::endpoint m_RemoteEndpoint;
        std::vector<uint8_t> m_RecvBuffer;
        std::thread m_Thread;
        std::function<void(const std::vector<uint8_t>&)> m_Callback;
        bool m_Running = false;
    };
}
