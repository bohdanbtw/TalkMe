// Entry point for ScreenRecording.exe. Same behavior as TalkMe.exe --screen-capture
// but this exe has FileDescription "Screen Recording" so Task Manager shows that name.
#include "../screen/ScreenCaptureProcess.h"
#include <Windows.h>
#include <ShellScalingApi.h>
#pragma comment(lib, "Shcore.lib")
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {
    const char kScreenCaptureArg[] = "--screen-capture";

    bool ParseScreenCaptureArgs(const char* cmdLine, std::string& pipeName, int& fps, int& width, int& height, int& quality) {
        if (!cmdLine || std::strstr(cmdLine, kScreenCaptureArg) == nullptr) return false;
        auto getArg = [cmdLine](const char* prefix) -> std::string {
            const char* p = std::strstr(cmdLine, prefix);
            if (!p) return {};
            p += std::strlen(prefix);
            while (*p == ' ' || *p == '=') ++p;
            if (*p == '"') { ++p; const char* e = std::strchr(p, '"'); return e ? std::string(p, e - p) : p; }
            const char* e = p;
            while (*e && *e != ' ' && *e != '\t') ++e;
            return std::string(p, e - p);
        };
        pipeName = getArg("--pipe=");
        if (pipeName.empty()) return false;
        fps = std::atoi(getArg("--fps=").c_str()); if (fps <= 0) fps = 30;
        width = std::atoi(getArg("--width=").c_str()); if (width <= 0) width = 1920;
        height = std::atoi(getArg("--height=").c_str()); if (height <= 0) height = 1080;
        quality = std::atoi(getArg("--quality=").c_str()); if (quality <= 0) quality = 70;
        return true;
    }
}

int WINAPI wWinMain(_In_ HINSTANCE, _In_opt_ HINSTANCE, _In_ LPWSTR, _In_ int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    std::string pipeName;
    int fps = 30, width = 1920, height = 1080, quality = 70;
    if (!ParseScreenCaptureArgs(GetCommandLineA(), pipeName, fps, width, height, quality)) {
        std::fprintf(stderr, "[ScreenRecording] Missing --screen-capture --pipe=...\n");
        return 1;
    }

    return TalkMe::RunScreenCaptureProcess(pipeName, fps, quality, width, height);
}
