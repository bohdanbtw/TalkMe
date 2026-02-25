#include "VoiceSendPacer.h"
#include <windows.h>

namespace TalkMe {

namespace {

static constexpr size_t kMaxQueue        = 20;
static constexpr LONG   kPeriodMs        = 10;
static constexpr DWORD  kHighResTimer    = 0x00000002; // CREATE_WAITABLE_TIMER_HIGH_RESOLUTION

struct HandleDeleter {
    void operator()(void* h) const noexcept {
        if (h && h != INVALID_HANDLE_VALUE)
            ::CloseHandle(static_cast<HANDLE>(h));
    }
};

// RAII wrapper: owns a Windows HANDLE, closes on destruction.
using UniqueHandle = std::unique_ptr<void, HandleDeleter>;

[[nodiscard]] static UniqueHandle MakeWaitableTimer() noexcept {
    return UniqueHandle(
        ::CreateWaitableTimerExW(nullptr, nullptr, kHighResTimer, TIMER_ALL_ACCESS));
}

[[nodiscard]] static UniqueHandle MakeEvent() noexcept {
    return UniqueHandle(::CreateEventW(nullptr, /*manualReset=*/TRUE, FALSE, nullptr));
}

} // namespace

void VoiceSendPacer::Start(SendFn fn) {
    m_SendFn  = std::move(fn);
    m_Running = true;

    UniqueHandle timer = MakeWaitableTimer();
    UniqueHandle stopEv = MakeEvent();

    if (timer) {
        LARGE_INTEGER due;
        due.QuadPart = -static_cast<LONGLONG>(kPeriodMs) * 10'000LL; // 100 ns units
        ::SetWaitableTimerEx(static_cast<HANDLE>(timer.get()), &due, kPeriodMs,
                             nullptr, nullptr, nullptr, 0);
    }

    // Store raw handles; UniqueHandle ownership is transferred to the thread lambda.
    m_Timer     = timer.get();
    m_StopEvent = stopEv.get();

    m_Thread = std::thread([this,
                            timerOwner  = std::move(timer),
                            stopOwner   = std::move(stopEv)] {
        ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

        HANDLE timerH = static_cast<HANDLE>(m_Timer);
        HANDLE stopH  = static_cast<HANDLE>(m_StopEvent);

        while (m_Running.load(std::memory_order_relaxed)) {
            HANDLE handles[2] = { timerH, stopH };
            DWORD  wr = ::WaitForMultipleObjects(2, handles, FALSE, INFINITE);

            if (wr == WAIT_OBJECT_0 + 1 || wr == WAIT_FAILED) break; // stop event or error
            if (wr != WAIT_OBJECT_0) break;                           // unexpected

            // Drain all packets queued since the last tick. Under normal voice
            // load (one packet/tick) this is exactly one iteration; after a
            // burst it catches up without stalling the timer period.
            while (true) {
                std::vector<uint8_t> pkt;
                {
                    std::lock_guard<std::mutex> lk(m_Mutex);
                    if (m_Queue.empty()) break;
                    pkt = std::move(m_Queue.front());
                    m_Queue.pop();
                }
                if (m_SendFn) {
                    try {
                        m_SendFn(pkt);
                    }
                    catch (...) {
                        // Do not let send callback throw out of the pacer thread —
                        // that would call std::terminate() and crash the process.
                    }
                }
            }
        }

        // Handles are closed by UniqueHandle destructors at lambda exit.
        m_Timer     = nullptr;
        m_StopEvent = nullptr;
    });
}

void VoiceSendPacer::Stop() {
    if (!m_Running.exchange(false)) return; // already stopped or never started

    if (m_StopEvent)
        ::SetEvent(static_cast<HANDLE>(m_StopEvent));

    if (m_Thread.joinable())
        m_Thread.join();

    // m_Timer / m_StopEvent are now nullptr (cleared inside the thread lambda).

    std::lock_guard<std::mutex> lk(m_Mutex);
    while (!m_Queue.empty()) m_Queue.pop();
}

void VoiceSendPacer::Enqueue(std::vector<uint8_t> payload) {
    std::lock_guard<std::mutex> lk(m_Mutex);
    if (m_Queue.size() >= kMaxQueue)
        m_Queue.pop(); // drop oldest to bound latency
    m_Queue.push(std::move(payload));
}

} // namespace TalkMe