#include "Logger.h"
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <cstdio>

namespace TalkMe {

    std::ofstream VoiceTrace::s_file;
    std::mutex    VoiceTrace::s_mutex;
    bool          VoiceTrace::s_enabled = false;

    void VoiceTrace::init() {
        if (const char* e = std::getenv("VOICE_TRACE"))
            s_enabled = (e[0] == '1' || e[0] == 'y' || e[0] == 'Y');
        if (s_enabled)
            s_file.open("voice_trace.log", std::ios::out | std::ios::trunc);
    }

    void VoiceTrace::log(const std::string& msg) {
        if (!s_enabled || !s_file.is_open()) return;

        const auto now = std::chrono::system_clock::now();
        const std::time_t t = std::chrono::system_clock::to_time_t(now);
        struct tm tm_buf {};
#ifdef _WIN32
        if (localtime_s(&tm_buf, &t) != 0) return;
#else
        if (localtime_r(&t, &tm_buf) == nullptr) return;
#endif
        char timeBuf[32];
        if (std::strftime(timeBuf, sizeof(timeBuf), "%H:%M:%S", &tm_buf) == 0) return;

        char lineBuf[4096];
        const int n = std::snprintf(lineBuf, sizeof(lineBuf), "%s [TRACE] %s\n", timeBuf, msg.c_str());
        if (n <= 0 || static_cast<size_t>(n) >= sizeof(lineBuf)) return;

        std::lock_guard<std::mutex> lock(s_mutex);
        s_file.write(lineBuf, static_cast<std::streamsize>(n));
        s_file.flush();
    }

} // namespace TalkMe