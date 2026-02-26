#pragma once
#include <vector>
#include <cstdint>
#include <atomic>
#include <thread>
#include <functional>
#include <mutex>

namespace TalkMe {

class AudioLoopback {
public:
    using AudioCallback = std::function<void(const float* samples, int frameCount, int sampleRate, int channels)>;

    AudioLoopback() = default;
    ~AudioLoopback() { Stop(); }

    void Start(AudioCallback onAudio);
    void Stop();
    bool IsRunning() const { return m_Running.load(); }

private:
    void CaptureLoop();

    std::atomic<bool> m_Running{ false };
    std::thread m_Thread;
    AudioCallback m_OnAudio;
};

} // namespace TalkMe
