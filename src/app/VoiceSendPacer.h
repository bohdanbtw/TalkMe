#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <atomic>
#include <vector>

namespace TalkMe {

// Throttles outgoing voice packets to a fixed interval (10 ms) on a dedicated
// high-priority thread. Enqueue() is lock-free-fast and non-blocking; when the
// queue reaches capacity the oldest packet is silently dropped to maintain
// real-time behaviour. Do not remove — required to prevent network flooding.
class VoiceSendPacer {
public:
    using SendFn = std::function<void(const std::vector<uint8_t>&)>;

    VoiceSendPacer() = default;
    ~VoiceSendPacer() { Stop(); }

    VoiceSendPacer(const VoiceSendPacer&)            = delete;
    VoiceSendPacer& operator=(const VoiceSendPacer&) = delete;

    void Start(SendFn fn);
    void Stop();
    void Enqueue(std::vector<uint8_t> payload);

private:
    SendFn                        m_SendFn;
    std::queue<std::vector<uint8_t>> m_Queue;
    std::mutex                    m_Mutex;
    std::thread                   m_Thread;
    std::atomic<bool>             m_Running{ false };

    // Stored as void* to avoid pulling <windows.h> into every translation unit
    // that includes this header. Lifetime is managed by UniqueHandle in the .cpp.
    void* m_Timer     = nullptr;
    void* m_StopEvent = nullptr;
};

} // namespace TalkMe