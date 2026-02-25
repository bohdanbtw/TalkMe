// Link probe: UDP echo packets for RTT / loss / jitter measurement.
// State lives in Application; only method implementations are defined here.
#include "Application.h"
#include "../shared/Protocol.h"
#include <windows.h>
#include <timeapi.h>
#pragma comment(lib, "winmm.lib")
#include <algorithm>
#include <cstring>
#include <thread>

namespace TalkMe {

void Application::StartLinkProbe() {
    if (m_ProbeRunning.exchange(true)) return;

    std::thread([this] {
        static constexpr int kProbeCount         = 40;  // packets per burst
        static constexpr int kProbeIntervalMs    = 10;  // spacing between sends
        static constexpr int kWaitForEchoMs      = 400; // collection window after last send
        static constexpr int kRepeatIntervalSec  = 15;  // idle period between bursts

        try {
        while (true) {
            struct ProbeSent { uint32_t seq; int64_t sentUs; };
            std::vector<ProbeSent> sent;
            sent.reserve(kProbeCount);

            // ── Send burst ────────────────────────────────────────────────
            ::timeBeginPeriod(1);
            bool aborted = false;
            for (int i = 0; i < kProbeCount; ++i) {
                if (m_ShuttingDown.load()) { aborted = true; break; }

                const int64_t now_us = std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
                const uint32_t seq    = static_cast<uint32_t>(i);
                const uint32_t netSeq = TalkMe::HostToNet32(seq);

                std::vector<uint8_t> pkt(13);
                pkt[0] = 0xEE;
                std::memcpy(&pkt[1], &netSeq, 4);
                // sentUs is stored in native byte order: the server echoes it back
                // to this same host without inspecting it, so no conversion is needed.
                std::memcpy(&pkt[5], &now_us, 8);

                m_VoiceTransport.SendRaw(pkt);
                sent.push_back({ seq, now_us });

                std::this_thread::sleep_for(std::chrono::milliseconds(kProbeIntervalMs));
            }
            ::timeEndPeriod(1);

            if (aborted) break;

            // ── Collect echoes ────────────────────────────────────────────
            std::this_thread::sleep_for(std::chrono::milliseconds(kWaitForEchoMs));
            if (m_ShuttingDown.load()) break;

            std::vector<float> rtts;
            {
                std::lock_guard<std::mutex> lk(m_ProbeEchoMutex);
                rtts.reserve(sent.size());
                for (const auto& probe : sent) {
                    auto it = m_ProbeEchoTimes.find(probe.seq);
                    if (it != m_ProbeEchoTimes.end()) {
                        const float rttMs = static_cast<float>(it->second - probe.sentUs) / 1000.0f;
                        if (rttMs > 0.0f && rttMs < 2000.0f)
                            rtts.push_back(rttMs);
                    }
                }
                m_ProbeEchoTimes.clear();
            }

            if (!rtts.empty()) {
                std::sort(rtts.begin(), rtts.end());
                const float medianRtt = rtts[rtts.size() / 2];

                float meanRtt = 0.0f;
                for (float r : rtts) meanRtt += r;
                meanRtt /= static_cast<float>(rtts.size());

                float jitterMs = 0.0f;
                for (float r : rtts) jitterMs += std::abs(r - meanRtt);
                jitterMs /= static_cast<float>(rtts.size());

                const float lossPct = 100.0f * (1.0f -
                    static_cast<float>(rtts.size()) / static_cast<float>(kProbeCount));

                m_AudioEngine.ApplyProbeResults(medianRtt, jitterMs, lossPct);
            }

            // ── Idle period before next burst ─────────────────────────────
            for (int i = 0; i < kRepeatIntervalSec * 10; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                if (m_ShuttingDown.load() || !m_UseUdpVoice || !m_NetClient.IsConnected())
                    break;
            }
            if (m_ShuttingDown.load() || !m_UseUdpVoice || !m_NetClient.IsConnected())
                break;
        }
        }
        catch (...) {
            // Do not let probe thread throw — would call std::terminate() (thread is detached).
        }

        m_ProbeRunning = false;
    }).detach();
}

void Application::HandleProbeEcho(const std::vector<uint8_t>& pkt) {
    if (pkt.size() < 13) return;

    uint32_t seq   = 0;
    int64_t  sentUs = 0;
    std::memcpy(&seq,    &pkt[1], 4);
    std::memcpy(&sentUs, &pkt[5], 8);
    seq = TalkMe::NetToHost32(seq);

    const int64_t now_us = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    std::lock_guard<std::mutex> lk(m_ProbeEchoMutex);
    // Store round-trip arrival time; RTT = now_us - sentUs (computed in the probe thread).
    m_ProbeEchoTimes[seq] = now_us;
}

} // namespace TalkMe