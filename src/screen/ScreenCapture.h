#pragma once
#include <vector>
#include <cstdint>
#include <atomic>
#include <thread>
#include <functional>
#include <windows.h>

namespace TalkMe {

struct CaptureSettings {
    int fps = 30;
    int quality = 75;
    int maxWidth = 1920;
    int maxHeight = 1080;
};

class ScreenCapture {
public:
    using FrameCallback = std::function<void(const std::vector<uint8_t>& jpegData, int width, int height)>;

    ScreenCapture() = default;
    ~ScreenCapture() { Stop(); }

    void Start(const CaptureSettings& settings, FrameCallback onFrame);
    void Stop();
    bool IsRunning() const { return m_Running.load(); }

private:
    void CaptureLoop();
    std::vector<uint8_t> CaptureFrame(int& outWidth, int& outHeight);
    std::vector<uint8_t> CompressBMP(const uint8_t* bmpData, int width, int height, int quality);

    std::atomic<bool> m_Running{ false };
    std::thread m_Thread;
    CaptureSettings m_Settings;
    FrameCallback m_OnFrame;
};

} // namespace TalkMe
