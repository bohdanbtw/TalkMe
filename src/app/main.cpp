#include "Application.h"
#include "../core/ConfigManager.h"
#include "../screen/ScreenCaptureProcess.h"
#include <Windows.h>
#include <objbase.h>
#include <cstdio>
#include <iostream>
#include <exception>
#include <fstream>
#include <string>
#include <cstdlib>

// Link with Shcore.lib for DPI awareness
#pragma comment(lib, "Shcore.lib")
#pragma comment(lib, "ole32.lib")
#include <ShellScalingApi.h>

namespace {
    const char kRelaunchArg[] = "--relaunch-instead";
    const char kScreenCaptureArg[] = "--screen-capture";
    const char kSingleInstanceMutex[] = "TalkMe_SingleInstance_Mutex";
    const wchar_t kTalkMeClass[] = L"TalkMeClass";

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

namespace {
    std::terminate_handler g_prevTerminate = nullptr;

    void TalkMeTerminateHandler() {
        // Log that std::terminate() was called so we can confirm the crash cause.
        // Do not allocate or throw; use fixed buffers and Win32 API.
        wchar_t pathW[MAX_PATH] = {};
        if (::GetModuleFileNameW(nullptr, pathW, MAX_PATH) > 0) {
            wchar_t* lastSlash = std::wcsrchr(pathW, L'\\');
            if (lastSlash) lastSlash[1] = L'\0';
            wcscat_s(pathW, L"talkme_terminate.txt");
            char pathA[MAX_PATH * 2] = {};
            const int n = ::WideCharToMultiByte(CP_UTF8, 0, pathW, -1, pathA, sizeof(pathA), nullptr, nullptr);
            if (n > 0) {
                HANDLE h = ::CreateFileA(pathA, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (h != INVALID_HANDLE_VALUE) {
                    char msg[128];
                    const int len = std::snprintf(msg, sizeof(msg),
                        "std::terminate() called (thread id %lu)\r\n",
                        (unsigned long)::GetCurrentThreadId());
                    if (len > 0) {
                        DWORD written = 0;
                        ::WriteFile(h, msg, (DWORD)len, &written, nullptr);
                    }
                    ::CloseHandle(h);
                }
            }
        }
        ::OutputDebugStringA("TalkMe: std::terminate() called\n");
        if (g_prevTerminate) g_prevTerminate();
        else std::abort();
    }
}

int WINAPI WinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance, _In_ LPSTR lpCmdLine, _In_ int nShowCmd) {
    g_prevTerminate = std::set_terminate(TalkMeTerminateHandler);

    // 1. Set DPI awareness for high-resolution screens
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // 2. Initialize COM STA on main thread so WIC (GIF decode) works in ProcessPendingGifDecodes
    (void)CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    std::string screenPipe;
    int screenFps = 30, screenW = 1920, screenH = 1080, screenQuality = 70;
    if (ParseScreenCaptureArgs(GetCommandLineA(), screenPipe, screenFps, screenW, screenH, screenQuality)) {
        int code = TalkMe::RunScreenCaptureProcess(screenPipe, screenFps, screenQuality, screenW, screenH);
        return code;
    }

    bool relaunchInstead = (lpCmdLine && std::strstr(lpCmdLine, kRelaunchArg) != nullptr);

    // 3. Single instance: only one TalkMe in tray. Second launch brings existing window to front.
    //    If launched with --relaunch-instead, wait for the old process to exit then take the mutex.
    HANDLE hSingleMutex = ::CreateMutexA(nullptr, TRUE, kSingleInstanceMutex);
    if (hSingleMutex != nullptr && ::GetLastError() == ERROR_ALREADY_EXISTS) {
        ::CloseHandle(hSingleMutex);
        if (relaunchInstead) {
            for (int i = 0; i < 100; ++i) {
                ::Sleep(200);
                hSingleMutex = ::CreateMutexA(nullptr, TRUE, kSingleInstanceMutex);
                if (hSingleMutex != nullptr && ::GetLastError() != ERROR_ALREADY_EXISTS)
                    break;
                if (hSingleMutex) ::CloseHandle(hSingleMutex);
            }
            if (hSingleMutex == nullptr || ::GetLastError() == ERROR_ALREADY_EXISTS) {
                if (hSingleMutex) ::CloseHandle(hSingleMutex);
                return 0;
            }
        } else {
            for (int retry = 0; retry < 50; ++retry) {
                HWND existing = ::FindWindowW(kTalkMeClass, nullptr);
                if (existing != nullptr) {
                    ::PostMessageW(existing, WM_APP + 10, 0, 0);  // WM_TALKME_RESTORE
                    return 0;
                }
                ::Sleep(100);
            }
            return 0;
        }
    }

    int winW = 1920, winH = 1080, restoreX = -1, restoreY = -1;
    if (relaunchInstead) {
        std::string path = TalkMe::ConfigManager::GetConfigDirectory() + "\\relaunch_rect.txt";
        std::ifstream f(path);
        if (f && (f >> restoreX >> restoreY >> winW >> winH)) {
            if (winW < 320) winW = 320;
            if (winH < 240) winH = 240;
        } else {
            restoreX = restoreY = -1;
        }
        (void)DeleteFileA(path.c_str());
    }

    // 4. Start the application
    TalkMe::Application app("TalkMe", winW, winH, restoreX, restoreY);
    if (app.Initialize()) {
        app.Run();
    }

    return 0;
}

// Provide a console-friendly entry point that forwards to WinMain so the
// project can be linked as a Console application in project settings.
int main(int argc, char** argv) {
    return WinMain(GetModuleHandle(NULL), nullptr, GetCommandLineA(), SW_SHOWDEFAULT);
}
