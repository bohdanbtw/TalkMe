#pragma once
#include <fstream>
#include <sstream>
#include <iostream>
#include <mutex>
#include <chrono>
#include <iomanip>
#include <string>

namespace TalkMe {

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

            try { std::cerr << ss.str(); } catch (...) {}
#endif
        }

        void Shutdown() {
            std::lock_guard<std::mutex> lock(m_Mutex);
            if (m_File.is_open()) m_File.close();
            if (m_StatsFile.is_open()) m_StatsFile.close();
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

        std::mutex m_Mutex;
        std::ofstream m_File;
        std::mutex m_StatsMutex;
        std::ofstream m_StatsFile;
    };

    #define LOG_INFO(msg)    TalkMe::Logger::Instance().Log("INFO", msg)
    #define LOG_ERROR(msg)   TalkMe::Logger::Instance().Log("ERROR", msg)
    #define LOG_AUDIO(msg)   TalkMe::Logger::Instance().Log("AudioEngine", msg)
    #define LOG_NETWORK(msg) TalkMe::Logger::Instance().Log("Network", msg)
    #define LOG_APP(msg)     TalkMe::Logger::Instance().Log("App", msg)
    #define LOG_ERROR(msg)   TalkMe::Logger::Instance().Log("ERROR", msg)
}