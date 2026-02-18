#include "Application.h"
#include <Windows.h>
#include <cstdio>
#include <iostream>

// Link with Shcore.lib for DPI awareness
#pragma comment(lib, "Shcore.lib")
#include <ShellScalingApi.h>

int WINAPI WinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance, _In_ LPSTR lpCmdLine, _In_ int nShowCmd) {
    // 1. Force a console window to open for debugging traces
#ifdef _DEBUG
    if (AllocConsole()) {
        FILE* fp;
        freopen_s(&fp, "CONOUT$", "w", stdout);
        freopen_s(&fp, "CONOUT$", "w", stderr);
        std::ios::sync_with_stdio();
        std::clog.clear();
        std::cerr.clear();
        std::cout.clear();
        std::cout << "--- TalkMe Debug Console Enabled ---\n";
    }
#endif

    // 2. Set DPI awareness for high-resolution screens
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // 3. Start the application
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
