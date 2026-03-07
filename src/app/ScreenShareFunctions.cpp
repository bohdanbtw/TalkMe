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

void Application::StartScreenShareProcess(int fps, int quality, int width, int height) {
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
    settings.maxWidth = (std::max)(1, (std::min)(4096, width));
    settings.maxHeight = (std::max)(1, (std::min)(4096, height));

    auto onFrame = [this](const std::vector<uint8_t>& data, int w, int h, bool) {
        if (data.empty() || !m_NetClient.IsConnected())
            return;
        const auto now = std::chrono::steady_clock::now();
        {
            std::lock_guard<std::mutex> lock(m_ScreenShareStreamMutex);
            auto& si = m_ScreenShare.activeStreams[m_CurrentUser.username];
            si.username = m_CurrentUser.username;
            si.lastFrameData = data;
            si.frameWidth = w;
            si.frameHeight = h;
            si.frameUpdated = true;
            UpdateFpsWindow(si.streamFpsWindowStart, si.streamFramesInWindow, si.streamFps, now);
        }
        if (m_VoiceMembers.size() > 1)
            m_NetClient.SendRaw(PacketType::Screen_Share_Frame, data);
    };

    m_DXGICapture.Start(settings, std::move(onFrame));
}

void Application::StopScreenShareProcess() {
    m_DXGICapture.Stop();
    m_AudioLoopback.Stop();
    m_ScreenShare.iAmSharing = false;
}

} // namespace TalkMe
