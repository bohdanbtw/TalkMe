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
        std::function<void()> onResetOverlayToDefaults;
        bool& is2FAEnabled;
        bool& isSettingUp2FA;
        std::string& twoFASecretStr;
        std::string& twoFAUriStr;
        char* twoFASetupCodeBuf;
        size_t twoFASetupCodeBufSize;
        char* twoFASetupStatusMessage;
        std::function<void()> onEnable2FAClick;
        std::function<void()> onVerify2FAClick;
        std::function<void()> onCancel2FAClick;
        std::function<void()> onDisable2FAClick;
        bool& isDisabling2FA;
        char* disable2FACodeBuf;
        size_t disable2FACodeBufSize;
        std::function<void()> onConfirmDisable2FAClick;
        std::function<void()> onCancelDisable2FAClick;
        int noiseSuppressionMode = 1;
        bool testMicEnabled = false;
        float micActivity = 0.0f;  // 0..1 scale for input level bar
        std::function<void(int)> onNoiseSuppressionModeChange;
        std::function<void(bool)> onToggleTestMic;
        std::function<void()> onResetToDefaults;
    };

    void RenderSettings(SettingsContext& ctx);
}
