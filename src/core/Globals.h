#pragma once

namespace TalkMe {
    namespace Globals {
        // App Settings
        constexpr const char* APP_TITLE = "TalkMe";
        constexpr int WINDOW_WIDTH_DEFAULT = 1920;
        constexpr int WINDOW_HEIGHT_DEFAULT = 1080;

        // UI Constants
        constexpr float SIDEBAR_WIDTH = 280.0f;
        constexpr float FONT_SIZE_LARGE = 20.0f;
        constexpr float FONT_SIZE_NORMAL = 16.0f;

        // Limits
        constexpr int MAX_USERNAME_LEN = 32;
        constexpr int MAX_CHANNEL_NAME_LEN = 64;
        constexpr int MAX_MESSAGE_LEN = 2048;
    }
}