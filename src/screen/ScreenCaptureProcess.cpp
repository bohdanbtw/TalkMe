#include "ScreenCaptureProcess.h"
#include "DXGICapture.h"
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <objidl.h>
#include <gdiplus.h>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "gdiplus.lib")

namespace {

// Create a window titled "Screen Recording". Shown minimized so it appears on the taskbar
// with that name. (Task Manager uses the exe's FileDescription, so it still shows "TalkMe"
// unless you use the separate ScreenRecording.exe build.)
HWND CreateScreenRecordingIdentityWindow() {
    const wchar_t kClassName[] = L"TalkMe_ScreenCapture_Wnd";
    const wchar_t kTitle[] = L"Screen Recording";
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kClassName;
    if (!RegisterClassExW(&wc)) return nullptr;
    HWND hwnd = CreateWindowExW(0, kClassName, kTitle, WS_OVERLAPPED, 0, 0, 1, 1, nullptr, nullptr, wc.hInstance, nullptr);
    if (hwnd)
        SetWindowTextW(hwnd, kTitle);
    return hwnd;
}

} // namespace

namespace TalkMe {

int RunScreenCaptureProcess(const std::string& pipeName, int fps, int quality, int maxWidth, int maxHeight) {
    if (pipeName.empty()) return 1;

    SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);

    HWND hIdentityWnd = CreateScreenRecordingIdentityWindow();
    if (hIdentityWnd)
        ShowWindow(hIdentityWnd, SW_MINIMIZE);  // Minimized so taskbar shows "Screen Recording"

    HANDLE hPipe = CreateFileA(pipeName.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (hPipe == INVALID_HANDLE_VALUE) {
        std::fprintf(stderr, "[ScreenCaptureProcess] CreateFile(pipe) failed: %lu\n", GetLastError());
        return 1;
    }

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        CloseHandle(hPipe);
        return 1;
    }

    Gdiplus::GdiplusStartupInput gdipInput;
    ULONG_PTR gdipToken = 0;
    if (Gdiplus::GdiplusStartup(&gdipToken, &gdipInput, nullptr) != Gdiplus::Ok) {
        CoUninitialize();
        CloseHandle(hPipe);
        return 1;
    }

    std::atomic<bool> stopFlag{ false };
    DXGICaptureSettings settings;
    settings.fps = fps;
    settings.quality = quality;
    settings.maxWidth = maxWidth;
    settings.maxHeight = maxHeight;

    auto writeFrame = [&](const std::vector<uint8_t>& data, int w, int h, bool) {
        if (data.empty()) return;
        uint32_t len = static_cast<uint32_t>(data.size());
        DWORD written = 0;
        if (!WriteFile(hPipe, &len, 4, &written, nullptr) || written != 4) {
            stopFlag.store(true);
            return;
        }
        uint32_t uw = static_cast<uint32_t>(w), uh = static_cast<uint32_t>(h);
        if (!WriteFile(hPipe, &uw, 4, &written, nullptr) || written != 4) { stopFlag.store(true); return; }
        if (!WriteFile(hPipe, &uh, 4, &written, nullptr) || written != 4) { stopFlag.store(true); return; }
        if (!WriteFile(hPipe, data.data(), len, &written, nullptr) || written != len) {
            stopFlag.store(true);
        }
    };

    TalkMe::DXGICapture capture;
    capture.RunLoopSynchronous(settings, &stopFlag, writeFrame);

    if (hIdentityWnd)
        DestroyWindow(hIdentityWnd);

    Gdiplus::GdiplusShutdown(gdipToken);
    CoUninitialize();
    CloseHandle(hPipe);
    return 0;
}

} // namespace TalkMe
