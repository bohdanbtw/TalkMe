#include "AppWindow.h"
#include "resource.h"
#include "../core/ConfigManager.h"
#include <imgui.h>
#include <imgui_impl_win32.h>
#include <tchar.h>
#include <cstring>

// imgui_impl_win32.h keeps this in #if 0 to avoid windows.h; forward-declare so we can call it.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace TalkMe {

namespace {

static bool s_classRegistered = false;
static constexpr wchar_t kClassName[] = L"TalkMeClass";

// Unpack a single RCDATA resource from the exe to a file. Returns true on success.
static bool ExtractResourceToFile(int resourceId, const char* destPath) {
    HMODULE hMod = ::GetModuleHandleA(nullptr);
    HRSRC hRes = ::FindResourceA(hMod, MAKEINTRESOURCEA(resourceId), (LPCSTR)RT_RCDATA);
    if (!hRes) return false;
    HGLOBAL hGlob = ::LoadResource(hMod, hRes);
    if (!hGlob) return false;
    const void* p = ::LockResource(hGlob);
    if (!p) return false;
    DWORD size = ::SizeofResource(hMod, hRes);
    if (size == 0) return false;
    HANDLE hFile = ::CreateFileA(destPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    BOOL ok = ::WriteFile(hFile, p, size, &written, nullptr);
    ::CloseHandle(hFile);
    return (ok && written == size);
}

static const struct { int id; const char* name; } kPackedIcons[] = {
#ifdef IDR_ICON_16
    { IDR_ICON_16,  "app_16x16.ico" },
    { IDR_ICON_32,  "app_32x32.ico" },
    { IDR_ICON_48,  "app_48x48.ico" },
    { IDR_ICON_64,  "app_64x64.ico" },
    { IDR_ICON_128, "app_128x128.ico" },
    { IDR_ICON_256, "app_256x256.ico" },
#endif
};
static const int kPackedIconsCount = sizeof(kPackedIcons) / sizeof(kPackedIcons[0]);

// Ensure %LOCALAPPDATA%\TalkMe\assets has window/taskbar icons. Unpack from resources or copy from exe-relative assets.
static void EnsureIconsInAppData() {
    std::string configDir = ConfigManager::GetConfigDirectory();
    ::CreateDirectoryA(configDir.c_str(), nullptr);
    std::string assetsDir = configDir + "\\assets";
    ::CreateDirectoryA(assetsDir.c_str(), nullptr);

#ifdef IDR_ICON_16
    int extracted = 0;
    for (int i = 0; i < kPackedIconsCount; ++i) {
        if (ExtractResourceToFile(kPackedIcons[i].id, (assetsDir + "\\" + kPackedIcons[i].name).c_str()))
            ++extracted;
    }
    if (extracted >= 2)
        return;
#endif

    char exePath[MAX_PATH];
    if (::GetModuleFileNameA(nullptr, exePath, MAX_PATH) == 0) return;
    std::string exeDir(exePath);
    size_t last = exeDir.find_last_of("\\/");
    if (last != std::string::npos) exeDir.resize(last);
    else return;
    const char* candidates[] = {
        "\\assets",
        "\\..\\src\\assets",
        "\\..\\..\\src\\assets",
        "\\..\\..\\..\\src\\assets",
    };
    std::string sourceAssets;
    WIN32_FIND_DATAA fd = {};
    for (const char* sub : candidates) {
        std::string p = exeDir + sub;
        DWORD attr = ::GetFileAttributesA(p.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
            std::string search = p + "\\*.ico";
            HANDLE hFind = ::FindFirstFileA(search.c_str(), &fd);
            if (hFind != INVALID_HANDLE_VALUE) {
                ::FindClose(hFind);
                sourceAssets = p;
                break;
            }
        }
    }
    if (sourceAssets.empty()) return;
    std::string search = sourceAssets + "\\*.ico";
    HANDLE h = ::FindFirstFileA(search.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            std::string src = sourceAssets + "\\" + fd.cFileName;
            std::string dst = assetsDir + "\\" + fd.cFileName;
            ::CopyFileA(src.c_str(), dst.c_str(), FALSE);
        }
    } while (::FindNextFileA(h, &fd));
    ::FindClose(h);
}

} // namespace

LRESULT CALLBACK AppWindow::WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    AppWindow* w = reinterpret_cast<AppWindow*>(::GetWindowLongPtrW(hWnd, GWLP_USERDATA));
    if (msg == WM_CREATE) {
        LPCREATESTRUCTW cs = reinterpret_cast<LPCREATESTRUCTW>(lParam);
        w = reinterpret_cast<AppWindow*>(cs->lpCreateParams);
        if (w) ::SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(w));
    }
    if (w)
        return w->HandleMessage(hWnd, msg, wParam, lParam);
    return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}

LRESULT AppWindow::HandleMessage(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_DESTROY) {
        m_Hwnd = nullptr;
        if (m_OnDestroy) m_OnDestroy();
        ::PostQuitMessage(0);
        return 0;
    }
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) return 0;
    switch (msg) {
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED && m_OnResize)
            m_OnResize((UINT)LOWORD(lParam), (UINT)HIWORD(lParam));
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) return 0;
        break;
    case WM_DESTROY:
        return 0;
    }
    return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}

bool AppWindow::Create(int width, int height, const std::string& title) {
    EnsureIconsInAppData();
    std::string assetsPath = ConfigManager::GetConfigDirectory() + "\\assets";
    std::string icon16 = assetsPath + "\\app_16x16.ico";
    std::string icon32 = assetsPath + "\\app_32x32.ico";
    m_HiconSmall = reinterpret_cast<HICON>(::LoadImageA(nullptr, icon16.c_str(), IMAGE_ICON, 16, 16, LR_LOADFROMFILE));
    m_HiconBig = reinterpret_cast<HICON>(::LoadImageA(nullptr, icon32.c_str(), IMAGE_ICON, 32, 32, LR_LOADFROMFILE));

    if (!s_classRegistered) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(WNDCLASSEXW);
        wc.style = CS_CLASSDC;
        wc.lpfnWndProc = &AppWindow::WndProc;
        wc.hInstance = ::GetModuleHandle(nullptr);
        wc.hIcon = m_HiconBig;
        wc.hIconSm = m_HiconSmall;
        wc.lpszClassName = kClassName;
        if (!::RegisterClassExW(&wc)) return false;
        s_classRegistered = true;
    }

    // Use narrow title for display; CreateWindowExW expects wide string
    wchar_t titleW[256] = {};
    if (title.size() < 255) {
        for (size_t i = 0; i < title.size(); ++i) titleW[i] = (wchar_t)(unsigned char)title[i];
    } else {
        wcscpy_s(titleW, L"TalkMe");
    }

    m_Hwnd = ::CreateWindowExW(0, kClassName, titleW, WS_OVERLAPPEDWINDOW, 0, 0, width, height, nullptr, nullptr, ::GetModuleHandle(nullptr), this);
    if (!m_Hwnd) return false;
    // GWLP_USERDATA set in WndProc WM_CREATE
    if (m_HiconBig) ::SendMessageW(m_Hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(m_HiconBig));
    if (m_HiconSmall) ::SendMessageW(m_Hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(m_HiconSmall));
    ::ShowWindow(m_Hwnd, SW_HIDE);
    ::UpdateWindow(m_Hwnd);
    return true;
}

void AppWindow::Destroy() {
    if (m_Hwnd) {
        ::DestroyWindow(m_Hwnd);
        m_Hwnd = nullptr;
    }
    if (s_classRegistered) {
        ::UnregisterClassW(kClassName, ::GetModuleHandle(nullptr));
        s_classRegistered = false;
    }
    if (m_HiconSmall) {
        ::DestroyIcon(m_HiconSmall);
        m_HiconSmall = nullptr;
    }
    if (m_HiconBig) {
        ::DestroyIcon(m_HiconBig);
        m_HiconBig = nullptr;
    }
}

void AppWindow::Show(int nCmdShow) {
    if (m_Hwnd) ::ShowWindow(m_Hwnd, nCmdShow);
}

AppWindow::~AppWindow() {
    Destroy();
}

} // namespace TalkMe
