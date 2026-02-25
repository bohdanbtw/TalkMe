// EchoLive: TCP echo requests for INF panel (packet loss). Implementations only; state lives in Application.
#include "Application.h"
#include "../shared/Protocol.h"
#include <cstring>

namespace TalkMe {

void Application::ToggleEchoLive() {
    if (!m_NetClient.IsConnected()) return;
    if (m_EchoLiveEnabled) { m_EchoLiveEnabled = false; return; }
    EnsureEchoLiveEnabled();
}

void Application::EnsureEchoLiveEnabled() {
    if (!m_NetClient.IsConnected() || m_EchoLiveEnabled) return;
    m_EchoLiveEnabled = true;
    m_EchoLiveHistory.clear();
    m_EchoLiveBucketStartTime = std::chrono::steady_clock::now();
    m_EchoLiveBucketStartSeq = m_EchoLiveNextSeq;
    m_EchoLiveRecvThisBucket = 0;
    for (int i = 0; i < kEchoLivePacketsPerSecond; i++) {
        std::vector<uint8_t> payload(4);
        uint32_t seq = m_EchoLiveNextSeq++;
        uint32_t netSeq = TalkMe::HostToNet32(seq);
        std::memcpy(payload.data(), &netSeq, 4);
        m_NetClient.SendRaw(PacketType::Echo_Request, payload);
    }
}

void Application::OnEchoResponse(uint32_t seq) {
    if (m_EchoLiveEnabled && seq >= m_EchoLiveBucketStartSeq && seq < m_EchoLiveBucketStartSeq + kEchoLivePacketsPerSecond)
        m_EchoLiveRecvThisBucket++;
}

void Application::UpdateEchoLive() {
    if (!m_EchoLiveEnabled || !m_NetClient.IsConnected()) return;
    auto now = std::chrono::steady_clock::now();
    auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_EchoLiveBucketStartTime).count();
    if (elapsedMs < 1000) return;
    float lossPct = (kEchoLivePacketsPerSecond > 0)
        ? (100.f * (kEchoLivePacketsPerSecond - m_EchoLiveRecvThisBucket) / (float)kEchoLivePacketsPerSecond)
        : 0.f;
    m_EchoLiveHistory.push_back(lossPct);
    while (m_EchoLiveHistory.size() > kEchoLiveHistorySize) m_EchoLiveHistory.pop_front();
    m_EchoLiveBucketStartSeq = m_EchoLiveNextSeq;
    m_EchoLiveRecvThisBucket = 0;
    m_EchoLiveBucketStartTime = now;
    for (int i = 0; i < kEchoLivePacketsPerSecond; i++) {
        std::vector<uint8_t> payload(4);
        uint32_t seq = m_EchoLiveNextSeq++;
        uint32_t netSeq = TalkMe::HostToNet32(seq);
        std::memcpy(payload.data(), &netSeq, 4);
        m_NetClient.SendRaw(PacketType::Echo_Request, payload);
    }
}

} // namespace TalkMe
