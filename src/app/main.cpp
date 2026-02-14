#include "Application.h" // <--- CHANGED FROM TalkMeApp.h
#include <Windows.h>

// Link with Shcore.lib for DPI awareness
#pragma comment(lib, "Shcore.lib")
#include <ShellScalingApi.h>

int WINAPI WinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance, _In_ LPSTR lpCmdLine, _In_ int nShowCmd) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    TalkMe::Application app("TalkMe", 1920, 1080);
    if (app.Initialize()) {
        app.Run();
    }
    return 0;
}