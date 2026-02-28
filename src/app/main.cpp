#include "Application.h"
#include <Windows.h>
#include <cstdio>
#include <iostream>
#include <exception>

// Link with Shcore.lib for DPI awareness
#pragma comment(lib, "Shcore.lib")
#include <ShellScalingApi.h>

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

    // 1. Force a console window to open for debugging traces (Debug build)
#ifdef _DEBUG
    if (AllocConsole()) {
        FILE* fp = nullptr;
        freopen_s(&fp, "CONOUT$", "w", stdout);
        freopen_s(&fp, "CONOUT$", "w", stderr);
        std::ios::sync_with_stdio(true);
        std::cout.clear();
        std::cerr.clear();
        std::clog.clear();
    }
#endif

    // 2. Set DPI awareness for high-resolution screens
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // 3. Single instance: only one TalkMe in tray. Second launch brings existing window to front.
    static const char kSingleInstanceMutex[] = "TalkMe_SingleInstance_Mutex";
    HANDLE hSingleMutex = ::CreateMutexA(nullptr, TRUE, kSingleInstanceMutex);
    if (hSingleMutex != nullptr && ::GetLastError() == ERROR_ALREADY_EXISTS) {
        ::CloseHandle(hSingleMutex);
        // Find existing TalkMe window (class name from AppWindow) and ask it to show
        const wchar_t kTalkMeClass[] = L"TalkMeClass";
        for (int retry = 0; retry < 50; ++retry) {
            HWND existing = ::FindWindowW(kTalkMeClass, nullptr);
            if (existing != nullptr) {
                ::PostMessageW(existing, WM_APP + 10, 0, 0);  // WM_TALKME_RESTORE
                return 0;
            }
            ::Sleep(100);
        }
        return 0;  // existing instance not ready, exit without starting another
    }
    // First instance: keep mutex (do not close) so it stays owned until process exits

    // 4. Start the application
    TalkMe::Application app("TalkMe", 1920, 1080);
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
