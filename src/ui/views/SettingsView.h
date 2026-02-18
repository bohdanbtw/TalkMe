#pragma once
#include <functional>
#include <vector>
#include <string>

namespace TalkMe {
    struct AudioDeviceInfo;
}

namespace TalkMe::UI::Views {

    struct SettingsContext {
        std::function<void()> onLogout;
        std::function<void(int, int)> onDeviceChange;
        int& settingsTab;
        std::vector<int>& keyMuteMic;
        std::vector<int>& keyDeafen;
        int& selectedInputIdx;
        int& selectedOutputIdx;
        std::vector<AudioDeviceInfo> inputDevices;
        std::vector<AudioDeviceInfo> outputDevices;
        bool& overlayEnabled;
        int& overlayCorner;
        float& overlayOpacity;
        std::function<void()> onOverlayChanged;
    };

    void RenderSettings(SettingsContext& ctx);
}
