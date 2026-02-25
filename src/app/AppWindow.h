#pragma once

#include <string>
#include <functional>
#include <windows.h>

namespace TalkMe {

/// Win32 window and taskbar/title bar icons. Owns window creation, WndProc, and icon loading.
/// Call SetOnDestroy/SetOnResize before Create(). On WM_DESTROY the window handle is cleared and onDestroy is invoked.
class AppWindow {
public:
    AppWindow() = default;
    ~AppWindow();

    AppWindow(const AppWindow&) = delete;
    AppWindow& operator=(const AppWindow&) = delete;

    /// Register callbacks before Create. onDestroy is called on WM_DESTROY (after clearing handle).
    void SetOnDestroy(std::function<void()> fn) { m_OnDestroy = std::move(fn); }
    void SetOnResize(std::function<void(UINT, UINT)> fn) { m_OnResize = std::move(fn); }

    /// Create window and load icons. Ensures assets dir exists and has icons. Returns false on failure.
    bool Create(int width, int height, const std::string& title);

    /// Destroy window, unregister class, destroy icons. Safe to call multiple times.
    void Destroy();

    HWND GetHwnd() const { return m_Hwnd; }
    bool IsValid() const { return m_Hwnd != nullptr; }

    void Show(int nCmdShow);

private:
    static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

    HWND m_Hwnd = nullptr;
    HICON m_HiconSmall = nullptr;
    HICON m_HiconBig = nullptr;
    std::function<void()> m_OnDestroy;
    std::function<void(UINT, UINT)> m_OnResize;
};

} // namespace TalkMe
