#include "AudioEngine.h"
#include "AudioEngineInternal.h"
#include <mutex>

namespace TalkMe {

AudioEngine::Telemetry AudioEngine::GetTelemetry() {
    Telemetry t;
    if (!m_Internal) return t;

    int received = m_Internal->totalPacketsReceived.load(std::memory_order_relaxed);
    int lost = m_Internal->totalPacketsLost.load(std::memory_order_relaxed);
    t.totalPacketsReceived = received;
    t.totalPacketsLost = lost;
    t.totalPacketsDuplicated =
        m_Internal->totalPacketsDuplicated.load(std::memory_order_relaxed);

    int totalSamples = received + lost;
    t.packetLossPercentage = (totalSamples > 0)
        ? (100.0f * lost) / static_cast<float>(totalSamples)
        : 0.0f;

    {
        std::lock_guard<std::mutex> lock(m_Internal->m_TelemetryMutex);
        t.avgJitterMs = static_cast<float>(m_Internal->avgJitterMs);
        t.currentLatencyMs = static_cast<float>(m_Internal->currentLatencyMs);
    }

    t.bufferUnderruns =
        m_Internal->bufferUnderruns.load(std::memory_order_relaxed);
    t.bufferOverflows =
        m_Internal->bufferOverflows.load(std::memory_order_relaxed);
    t.currentBufferMs = m_Internal->adaptiveBufferLevel;
    t.targetBufferMs = m_Internal->targetBufferMs;
    t.remoteMemberCount = m_Internal->remoteMemberCount;
    t.adaptiveBufferLevel = m_Internal->adaptiveBufferLevel;
    t.currentEncoderBitrateKbps = m_Internal->currentEncoderBitrate / 1000;
    t.currentVoiceActivityLevel = m_Internal->currentVoiceActivityLevel;

    return t;
}

} // namespace TalkMe
