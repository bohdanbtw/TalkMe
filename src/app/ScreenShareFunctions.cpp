#include "Application.h"
#include "../shared/Protocol.h"
#include <cstdio>
#include <algorithm>
#include <chrono>

namespace TalkMe {

int Application::GetEffectiveShareFps() const {
    const int uiCap = (std::max)(10, (std::min)(1000, m_TargetFps));
    const int shareCap = (std::max)(1, (std::min)(120, m_ScreenShare.fps));
    return (std::max)(1, (std::min)(uiCap, shareCap));
}

void Application::UpdateFpsWindow(
    std::chrono::steady_clock::time_point& windowStart,
    uint32_t& framesInWindow,
    float& outFps,
    const std::chrono::steady_clock::time_point& now) {
    if (framesInWindow == 0)
        windowStart = now;
    framesInWindow++;
    const double windowSec = std::chrono::duration<double>(now - windowStart).count();
    if (windowSec >= 1.0) {
        outFps = static_cast<float>(framesInWindow / windowSec);
        framesInWindow = 0;
        windowStart = now;
    }
}

void Application::StartScreenShareProcess(int fps, int quality, int width, int height, void* targetWindow) {
    (void)fps; // m_ScreenShare.fps already set by caller
    if (m_DXGICapture.IsRunning())
        return;

    const int effectiveFps = GetEffectiveShareFps();
    std::fprintf(stderr, "[TalkMe] StartScreenShareProcess: effectiveFps=%d (targetFps=%d, shareFps=%d)\n",
        effectiveFps, m_TargetFps, m_ScreenShare.fps);
    std::fflush(stderr);

    DXGICaptureSettings settings;
    settings.fps = effectiveFps;
    settings.quality = (std::max)(1, (std::min)(100, quality));
    m_AdaptiveQualityLevel.store(settings.quality);
    settings.maxWidth = (std::max)(1, (std::min)(4096, width));
    settings.maxHeight = (std::max)(1, (std::min)(4096, height));
    settings.targetWindow = targetWindow;
    settings.keyframeOnlyMode = m_ScreenShare.keyframeOnlyMode;
    settings.keyframeIntervalFrames = m_ScreenShare.keyframeIntervalFrames;
    settings.pAdaptiveQuality = &m_AdaptiveQualityLevel;  // NEW: Pass pointer for adaptive bitrate
    {
        std::lock_guard<std::mutex> lock(m_ScreenShareSendQueueMutex);
        while (!m_ScreenShareSendQueue.empty())
            m_ScreenShareSendQueue.pop();
    }
    m_ScreenShareQueueDepth.store(0);

    // Start send thread if not already running
    if (!m_ScreenShareSendThreadRunning.load()) {
        m_ScreenShareSendThreadRunning.store(true);
        if (m_ScreenShareSendThread.joinable())
            m_ScreenShareSendThread.join();
        m_ScreenShareSendThread = std::thread([this]() { RunScreenShareSendThread(); });
    }

    auto onFrame = [this](const std::vector<uint8_t>& data, int w, int h, bool isKeyFrame) {
        (void)isKeyFrame;
        if (data.empty() || !m_NetClient.IsConnected())
            return;

        // Queue frame for async network send.
        // Keep queue short to preserve low latency under load.
        {
            std::lock_guard<std::mutex> queueLock(m_ScreenShareSendQueueMutex);
            while (m_ScreenShareSendQueue.size() > 2)
                m_ScreenShareSendQueue.pop();
            m_ScreenShareSendQueue.push(data);
        }
        m_ScreenShareSendCV.notify_one();

        // Update display info in background (lower priority)
        {
            std::lock_guard<std::mutex> lock(m_ScreenShareStreamMutex);
            auto& si = m_ScreenShare.activeStreams[m_CurrentUser.username];
            si.username = m_CurrentUser.username;
            si.lastFrameData = data;
            si.frameWidth = w;
            si.frameHeight = h;
            si.frameUpdated = true;

            const auto now = std::chrono::steady_clock::now();
            UpdateFpsWindow(si.streamFpsWindowStart, si.streamFramesInWindow, si.streamFps, now);
        }
    };

    m_DXGICapture.Start(settings, std::move(onFrame));
}

void Application::StopScreenShareProcess() {
    m_DXGICapture.Stop();
    m_AudioLoopback.Stop();
    m_ScreenShare.iAmSharing = false;

    // Drain send queue
    m_ScreenShareSendThreadRunning.store(false);
    m_ScreenShareSendCV.notify_one();
    if (m_ScreenShareSendThread.joinable())
        m_ScreenShareSendThread.join();
    {
        std::lock_guard<std::mutex> lock(m_ScreenShareSendQueueMutex);
        while (!m_ScreenShareSendQueue.empty())
            m_ScreenShareSendQueue.pop();
    }
}

void Application::RunScreenShareSendThread() {
    int framesSent = 0;
    auto lastLogTime = std::chrono::steady_clock::now();
    auto lastAdaptiveCheck = std::chrono::steady_clock::now();

    // Adaptive quality state machine
    int currentQuality = m_ScreenShare.quality;
    int minQuality = 30;  // Never go below 30%
    int qualityStep = 10; // Reduce by 10% increments

    while (m_ScreenShareSendThreadRunning.load()) {
        std::vector<uint8_t> frameData;
        int queueSize = 0;

        {
            std::unique_lock<std::mutex> lock(m_ScreenShareSendQueueMutex);

            // Wait for frame or shutdown signal
            m_ScreenShareSendCV.wait_for(lock, std::chrono::milliseconds(50), [this] { 
                return !m_ScreenShareSendQueue.empty() || !m_ScreenShareSendThreadRunning.load(); 
            });

            // Check queue depth for adaptive bitrate
            queueSize = (int)m_ScreenShareSendQueue.size();
            m_ScreenShareQueueDepth.store(queueSize);

            // If we have frames, get the latest one and drop older ones
            if (!m_ScreenShareSendQueue.empty()) {
                // If queue is deep (>3 frames), skip old frames to catch up
                if (queueSize > 3) {
                    int dropped = 0;
                    while (m_ScreenShareSendQueue.size() > 1) {
                        m_ScreenShareSendQueue.pop();
                        dropped++;
                    }
                    if (dropped > 0) {
                        std::fprintf(stderr, "[SendThread] Queue depth %d → Dropped %d frames\n", 
                                   queueSize, dropped);
                    }
                }

                frameData = std::move(m_ScreenShareSendQueue.front());
                m_ScreenShareSendQueue.pop();
            }
        }

        // Adaptive quality check (every 500ms)
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastAdaptiveCheck).count() >= 500) {
            lastAdaptiveCheck = now;

            // Strategy: Reduce quality if queue consistently > 5, increase if queue < 2
            if (queueSize > 5) {
                // Encoder overwhelmed - reduce quality to take pressure off
                if (currentQuality > minQuality) {
                    currentQuality = (std::max)(minQuality, currentQuality - qualityStep);
                    m_AdaptiveQualityLevel.store(currentQuality);
                    std::fprintf(stderr, 
                        "[SendThread] ADAPTIVE: Queue depth %d → Reducing quality to %d%%\n",
                        queueSize, currentQuality);
                    std::fflush(stderr);
                }
            } 
            else if (queueSize < 2 && currentQuality < m_ScreenShare.quality) {
                // Network caught up - restore quality gradually
                currentQuality = (std::min)(m_ScreenShare.quality, currentQuality + qualityStep);
                m_AdaptiveQualityLevel.store(currentQuality);
                std::fprintf(stderr, 
                    "[SendThread] ADAPTIVE: Queue depth %d → Restoring quality to %d%%\n",
                    queueSize, currentQuality);
                std::fflush(stderr);
            }
        }

        // Send outside the lock (this might block a bit, that's OK)
        if (!frameData.empty() && m_NetClient.IsConnected()) {
            m_NetClient.SendRaw(PacketType::Screen_Share_Frame, frameData);
            framesSent++;

            // Log every second
            auto logNow = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::seconds>(logNow - lastLogTime).count() >= 1) {
                std::fprintf(stderr, 
                    "[SendThread] Sent %d frames/sec, Queue depth: %d, Adaptive quality: %d%%\n",
                    framesSent, queueSize, m_AdaptiveQualityLevel.load());
                std::fflush(stderr);
                framesSent = 0;
                lastLogTime = logNow;
            }
        }
    }
}

} // namespace TalkMe
