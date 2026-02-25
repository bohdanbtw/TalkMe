#pragma once
#include <fstream>
#include <sstream>
#include <iostream>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <iomanip>
#include <string>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cstdio>
#include <thread>
#include <atomic>
#include <array>

namespace TalkMe {

    static constexpr size_t kVoiceTraceBufSize = 256;
    static constexpr size_t kVoiceTraceQueueSize = 2048;

    class Logger {
    public:
        static Logger& Instance() {
            static Logger instance;
            return instance;
        }

        bool Initialize(const std::string& filePath) {
#ifdef _DEBUG
            std::lock_guard<std::mutex> lock(m_Mutex);
            m_File.open(filePath, std::ios::app);
            return m_File.is_open();
#else
            (void)filePath;
            return true;
#endif
        }

        void Log(const std::string& prefix, const std::string& message) {
#ifdef _DEBUG
            std::lock_guard<std::mutex> lock(m_Mutex);
            if (!m_File.is_open()) return;
            auto now = std::chrono::system_clock::now();
            auto time = std::chrono::system_clock::to_time_t(now);
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
            std::stringstream ss;
            struct tm localTime;
#ifdef _WIN32
            localtime_s(&localTime, &time);
            ss << std::put_time(&localTime, "%H:%M:%S")
#else
            ss << std::put_time(std::localtime(&time), "%H:%M:%S")
#endif
               << "." << std::setfill('0') << std::setw(3) << ms.count()
               << " [" << prefix << "] " << message << "\n";
            m_File << ss.str();
            m_File.flush();
#endif
        }

        /// Overload for hot paths: no heap, stack-friendly. Caller passes pre-formatted buffer.
        void Log(const std::string& prefix, const char* message) {
#ifdef _DEBUG
            std::lock_guard<std::mutex> lock(m_Mutex);
            if (!m_File.is_open() || !message) return;
            auto now = std::chrono::system_clock::now();
            auto time = std::chrono::system_clock::to_time_t(now);
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
            struct tm localTime;
#ifdef _WIN32
            localtime_s(&localTime, &time);
#else
            localtime_r(&time, &localTime);
#endif
            char timeBuf[32];
            std::strftime(timeBuf, sizeof(timeBuf), "%H:%M:%S", &localTime);
            char msBuf[8];
            std::snprintf(msBuf, sizeof(msBuf), ".%03d", static_cast<int>(ms.count()));
            m_File << timeBuf << msBuf << " [" << prefix << "] " << message << "\n";
            m_File.flush();
#endif
        }

        /// Initialize voice pipeline trace log. Starts async writer thread. Enable in Debug, or set env TALKME_VOICE_TRACE=1.
        bool InitializeVoiceTrace(const std::string& filePath, bool enableOverride = false) {
            bool enable = enableOverride;
            if (!enable) {
#ifdef _DEBUG
                enable = true;
#else
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4996)
#endif
                const char* env = std::getenv("TALKME_VOICE_TRACE");
#ifdef _MSC_VER
#pragma warning(pop)
#endif
                enable = (env && (env[0] == '1' || env[0] == 'y' || env[0] == 'Y'));
#endif
            }
            if (!enable) return false;
            {
                std::lock_guard<std::mutex> lock(m_VoiceTraceMutex);
                m_VoiceTraceFile.open(filePath, std::ios::out | std::ios::trunc);
                if (!m_VoiceTraceFile.is_open()) return false;
            }
            m_VoiceTraceStop.store(false);
            m_VoiceTraceThread = std::thread(&Logger::VoiceTraceWorker, this);
            return true;
        }

        /// Async: enqueues message (copy into fixed buffer). No heap on hot path. Safe to call from any thread.
        void LogVoiceTraceBuf(const char* buf) {
            if (!buf) return;
            std::lock_guard<std::mutex> lock(m_VoiceTraceQueueMutex);
            size_t w = m_VoiceTraceWriteIdx.load(std::memory_order_relaxed);
            std::snprintf(m_VoiceTraceQueue[w].data(), kVoiceTraceBufSize, "%.255s", buf);
            m_VoiceTraceWriteIdx.store((w + 1) % kVoiceTraceQueueSize, std::memory_order_release);
            if ((w + 1) % kVoiceTraceQueueSize == m_VoiceTraceReadIdx) {
                m_VoiceTraceReadIdx = (m_VoiceTraceReadIdx + 1) % kVoiceTraceQueueSize;
            }
            m_VoiceTraceCond.notify_one();
        }

        /// Non-blocking: enqueues only if the queue lock is free. Use from encode/voice hot path to never block.
        /// Returns true if the message was enqueued, false if lock was busy (message dropped).
        bool LogVoiceTraceBufNonBlocking(const char* buf) {
            if (!buf) return false;
            std::unique_lock<std::mutex> lock(m_VoiceTraceQueueMutex, std::try_to_lock);
            if (!lock.owns_lock()) return false;
            size_t w = m_VoiceTraceWriteIdx.load(std::memory_order_relaxed);
            std::snprintf(m_VoiceTraceQueue[w].data(), kVoiceTraceBufSize, "%.255s", buf);
            m_VoiceTraceWriteIdx.store((w + 1) % kVoiceTraceQueueSize, std::memory_order_release);
            if ((w + 1) % kVoiceTraceQueueSize == m_VoiceTraceReadIdx) {
                m_VoiceTraceReadIdx = (m_VoiceTraceReadIdx + 1) % kVoiceTraceQueueSize;
            }
            m_VoiceTraceCond.notify_one();
            return true;
        }

        /// Voice trace (non-hot path). Enqueues to async writer; truncation is applied in LogVoiceTraceBuf.
        void LogVoiceTrace(const std::string& message) {
            LogVoiceTraceBuf(message.c_str());
        }

        void Shutdown() {
            if (m_ShutdownDone) return;
            m_ShutdownDone = true;
            {
                std::lock_guard<std::mutex> lock(m_Mutex);
                if (m_File.is_open()) m_File.close();
                if (m_StatsFile.is_open()) m_StatsFile.close();
            }
            m_VoiceTraceStop.store(true);
            m_VoiceTraceCond.notify_all();
            if (m_VoiceTraceThread.joinable()) m_VoiceTraceThread.join();
            {
                std::lock_guard<std::mutex> l2(m_VoiceTraceMutex);
                if (m_VoiceTraceFile.is_open()) m_VoiceTraceFile.close();
            }
        }

        /// Initialize optional stats log (e.g. talkme_voice_stats.log). Call once at startup.
        bool InitializeStatsLog(const std::string& filePath) {
#ifdef _DEBUG
            std::lock_guard<std::mutex> lock(m_StatsMutex);
            m_StatsFile.open(filePath, std::ios::app);
            return m_StatsFile.is_open();
#else
            (void)filePath;
            return true;
#endif
        }

        /// Log with [Voice] prefix (file only; no console output). Thread-safe.
        void LogVoiceDebug(const std::string& message) {
#ifdef _DEBUG
            Log("Voice", message);
#else
            (void)message;
#endif
        }

        /// Log one line of voice statistics (channel, members, telemetry). Thread-safe.
        void LogVoiceStats(int channelId, const std::vector<std::string>& members,
            int packetsRecv, int packetsLost, float packetLossPct, float jitterMs, int bufferMs, int codecKbps) {
#ifdef _DEBUG
            std::lock_guard<std::mutex> lock(m_StatsMutex);
            if (!m_StatsFile.is_open()) return;
            auto now = std::chrono::system_clock::now();
            auto t = std::chrono::system_clock::to_time_t(now);
            struct tm localTime;
#ifdef _WIN32
            localtime_s(&localTime, &t);
#else
            localtime_r(&t, &localTime);
#endif
            std::stringstream ss;
            ss << std::put_time(&localTime, "%Y-%m-%d %H:%M:%S");
            ss << " | ch=" << channelId << " | members=" << members.size();
            for (const auto& m : members) ss << "," << m;
            ss << " | recv=" << packetsRecv << " loss=" << packetsLost << " loss%=" << std::fixed << std::setprecision(1) << packetLossPct;
            ss << "% | jitter_ms=" << jitterMs << " buffer_ms=" << bufferMs << " codec_kbps=" << codecKbps << "\n";
            m_StatsFile << ss.str();
            m_StatsFile.flush();
#endif
        }

    private:
        Logger() = default;
        ~Logger() { Shutdown(); }

        void VoiceTraceWorker() {
            try {
                std::unique_lock<std::mutex> lock(m_VoiceTraceQueueMutex);
                while (!m_VoiceTraceStop.load(std::memory_order_acquire)) {
                    m_VoiceTraceCond.wait_for(lock, std::chrono::milliseconds(100), [this] {
                        return m_VoiceTraceStop.load(std::memory_order_acquire) ||
                               m_VoiceTraceReadIdx != m_VoiceTraceWriteIdx.load(std::memory_order_acquire);
                    });
                    while (m_VoiceTraceReadIdx != m_VoiceTraceWriteIdx.load(std::memory_order_acquire)) {
                        char line[kVoiceTraceBufSize + 64];
                        auto now = std::chrono::system_clock::now();
                        auto t = std::chrono::system_clock::to_time_t(now);
                        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
                        struct tm localTime;
#ifdef _WIN32
                        localtime_s(&localTime, &t);
#else
                        localtime_r(&t, &localTime);
#endif
                        char timeBuf[32];
                        std::strftime(timeBuf, sizeof(timeBuf), "%H:%M:%S", &localTime);
                        std::snprintf(line, sizeof(line), "%s.%03d [TRACE] %s\n", timeBuf, static_cast<int>(ms.count()), m_VoiceTraceQueue[m_VoiceTraceReadIdx].data());
                        m_VoiceTraceReadIdx = (m_VoiceTraceReadIdx + 1) % kVoiceTraceQueueSize;
                        lock.unlock();
                        {
                            std::lock_guard<std::mutex> fl(m_VoiceTraceMutex);
                            if (m_VoiceTraceFile.is_open()) m_VoiceTraceFile << line;
                        }
                        lock.lock();
                    }
                }
            }
            catch (...) {
                /* Do not let VoiceTraceWorker throw — would call std::terminate(). */
            }
        }

        std::mutex m_Mutex;
        std::ofstream m_File;
        std::mutex m_StatsMutex;
        std::ofstream m_StatsFile;
        std::mutex m_VoiceTraceMutex;
        std::ofstream m_VoiceTraceFile;

        std::array<std::array<char, kVoiceTraceBufSize>, kVoiceTraceQueueSize> m_VoiceTraceQueue{};
        size_t m_VoiceTraceReadIdx = 0;
        std::atomic<size_t> m_VoiceTraceWriteIdx{0};
        std::mutex m_VoiceTraceQueueMutex;
        std::condition_variable m_VoiceTraceCond;
        std::atomic<bool> m_VoiceTraceStop{false};
        std::thread m_VoiceTraceThread;
        bool m_ShutdownDone = false;  // so ~Logger() does not double-close when destroyed at process exit
    };

    #define LOG_INFO(msg)       TalkMe::Logger::Instance().Log("INFO", msg)
    #define LOG_ERROR(msg)      TalkMe::Logger::Instance().Log("ERROR", msg)
    #define LOG_AUDIO(msg)      TalkMe::Logger::Instance().Log("AudioEngine", msg)
    #define LOG_NETWORK(msg)    TalkMe::Logger::Instance().Log("Network", msg)
    #define LOG_APP(msg)        TalkMe::Logger::Instance().Log("App", msg)
    #define LOG_ERROR_BUF(buf)  TalkMe::Logger::Instance().Log("ERROR", buf)
    #define LOG_AUDIO_BUF(buf)  TalkMe::Logger::Instance().Log("AudioEngine", buf)
}