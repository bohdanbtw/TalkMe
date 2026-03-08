#pragma once
#include <vector>
#include <string>
#include <cstdint>
#include <atomic>
#include <thread>
#include <functional>
#include <mutex>

namespace TalkMe {

struct WebcamDeviceInfo {
    std::string name;
    std::string symbolicLink;
};

class WebcamCapture {
public:
    // Callback receives BGRA pixels (4 bytes per pixel).
    using FrameCallback = std::function<void(const std::vector<uint8_t>& bgraData, int width, int height)>;

    WebcamCapture() = default;
    ~WebcamCapture() { Stop(); }

    static std::vector<WebcamDeviceInfo> EnumerateDevices();

    void Start(const std::string& deviceSymLink, int fps, int width, int height, FrameCallback onFrame);
    void Stop();
    bool IsRunning() const { return m_Running.load(); }

private:
    void CaptureLoop();

    std::atomic<bool> m_Running{ false };
    std::thread m_Thread;
    FrameCallback m_OnFrame;
    std::string m_DeviceSymLink;
    int m_Fps = 30;
    int m_Width = 640;
    int m_Height = 480;
};

} // namespace TalkMe
