#pragma once
#include "../../vendor/imgui.h"

namespace TalkMe::UI {

    // High-visibility button
    bool AccentButton(const char* label, const ImVec2& size = ImVec2(0, 0));

    // Low-visibility button
    bool SubtleButton(const char* label, const ImVec2& size = ImVec2(0, 0));

    void CenteredText(const char* text);
    bool ModernInput(const char* label, char* buf, size_t buf_size, ImGuiInputTextFlags flags = 0);
}