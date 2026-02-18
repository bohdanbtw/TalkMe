#include "VoiceTransport.h"
#include <thread>

using asio::ip::udp;

namespace TalkMe {

    VoiceTransport::VoiceTransport() : m_Socket(m_Context), m_RecvBuffer(65536) {}
    VoiceTransport::~VoiceTransport() { Stop(); }

    bool VoiceTransport::Start(const std::string& serverIp, uint16_t port) {
        try {
            udp::endpoint local_endpoint(udp::v4(), 0);
            m_Socket.open(udp::v4());
            m_Socket.bind(local_endpoint);
            m_RemoteEndpoint = udp::endpoint(asio::ip::address::from_string(serverIp), port);
            m_Running = true;
            m_Thread = std::thread([this]() { try { DoReceive(); m_Context.run(); } catch (...) {} });
            return true;
        }
        catch (const std::exception& e) {
            (void)e;
            return false;
        }
    }

    void VoiceTransport::Stop() {
        m_Running = false;
        try {
            if (m_Socket.is_open()) m_Socket.close();
            m_Context.stop();
            if (m_Thread.joinable()) m_Thread.join();
        }
        catch (...) {}
    }

    void VoiceTransport::SendVoicePacket(const std::vector<uint8_t>& payload) {
        if (!m_Socket.is_open()) return;
        try {
            m_Socket.send_to(asio::buffer(payload), m_RemoteEndpoint);
        }
        catch (...) {}
    }

    void VoiceTransport::DoReceive() {
        while (m_Running) {
            try {
                asio::ip::udp::endpoint sender;
                size_t n = m_Socket.receive_from(asio::buffer(m_RecvBuffer), sender);
                if (n > 0) {
                    std::vector<uint8_t> packet(m_RecvBuffer.begin(), m_RecvBuffer.begin() + n);
                    if (m_Callback) m_Callback(packet);
                }
            }
            catch (...) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
    }

}
