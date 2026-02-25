#pragma once
#include <string>
#include <fstream>
#include <mutex>

namespace TalkMe {

    struct VoiceTrace {
        static void init();
        static void log(const std::string& msg);

    private:
        static std::ofstream s_file;
        static std::mutex s_mutex;
        static bool s_enabled;
    };

} // namespace TalkMe