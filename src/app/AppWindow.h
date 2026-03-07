#pragma once

#include <string>
#include <vector>
#include <functional>
#include <windows.h>
#include <objidl.h>

namespace TalkMe {

/// Win32 window and taskbar/title bar icons. Owns window creation, WndProc, and icon loading.
/// Call SetOnDestroy/SetOnResize before Create(). On WM_DESTROY the window handle is cleared and onDestroy is invoked.
/// Close button minimizes to system tray; right-click tray icon -> Exit to quit.
class AppWindow {
public:
    AppWindow() = default;
    ~AppWindow();

    AppWindow(const AppWindow&) = delete;
    AppWindow& operator=(const AppWindow&) = delete;

    /// Register callbacks before Create. onDestroy is called on WM_DESTROY (after clearing handle).
    void SetOnDestroy(std::function<void()> fn) { m_OnDestroy = std::move(fn); }
    void SetOnResize(std::function<void(UINT, UINT)> fn) { m_OnResize = std::move(fn); }
    void SetOnRenderFrame(std::function<void()> fn) { m_OnRenderFrame = std::move(fn); }
    /// Called when user drops files on the window (e.g. for chat image upload). Paths are UTF-8.
    void SetOnFilesDropped(std::function<void(std::vector<std::string>)> fn) { m_OnFilesDropped = std::move(fn); }
    /// True while the user is dragging files over the window (OLE IDropTarget DragEnter/DragLeave).
    bool IsDragOver() const { return m_DragOver; }
    bool IsResizing() const { return m_IsResizing; }
    bool IsMinimized() const { return m_Minimized; }
    bool IsForeground() const { return m_Foreground; }

    /// Create window and load icons. Ensures assets dir exists and has icons. Returns false on failure.
    bool Create(int width, int height, const std::string& title);

    /// Move window to (x, y). Call after Create for relaunch restore. No-op if not created.
    void SetPosition(int x, int y);

    /// Enter borderless full-screen mode on the monitor that contains the window.
    /// Saves current style/rect so ExitBorderlessFullscreen() can restore them.
    void EnterBorderlessFullscreen();

    /// Restore the window to its pre-fullscreen style and position.
    void ExitBorderlessFullscreen();

    bool IsFullscreen() const { return m_IsFullscreen; }

    /// Destroy window, unregister class, destroy icons. Safe to call multiple times.
    void Destroy();

    HWND GetHwnd() const { return m_Hwnd; }
    bool IsValid() const { return m_Hwnd != nullptr; }

    void Show(int nCmdShow);

private:
    static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
    void AddOrUpdateTrayIcon(HWND hWnd);
    void RemoveTrayIcon();
    void ShowTrayContextMenu(HWND hWnd);

    class DropTargetImpl;
    DropTargetImpl* m_DropTarget = nullptr;
    mutable bool m_DragOver = false;
    friend class DropTargetImpl;

    HWND m_Hwnd = nullptr;
    HICON m_HiconSmall = nullptr;
    HICON m_HiconBig = nullptr;
    std::function<void()> m_OnDestroy;
    std::function<void(UINT, UINT)> m_OnResize;
    std::function<void()> m_OnRenderFrame;
    std::function<void(std::vector<std::string>)> m_OnFilesDropped;
    bool m_IsResizing = false;
    bool m_Minimized = false;
    bool m_Foreground = true;
    bool m_TrayIconAdded = false;
    bool m_ReallyClose = false;
    bool m_IsFullscreen = false;
    RECT  m_PreFullscreenRect  = {};
    DWORD m_PreFullscreenStyle = 0;
    bool  m_PreFullscreenWasMaximized = false;
};

} // namespace TalkMe
