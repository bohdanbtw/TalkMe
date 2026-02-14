#include "Components.h"
#include "../../vendor/imgui_internal.h" // Needed for advanced styling if we get fancy

namespace TalkMe::UI {

    bool AccentButton(const char* label, const ImVec2& size) {
        // Push Mint Teal Colors (#00C896)
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.00f, 0.78f, 0.59f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.00f, 0.88f, 0.69f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.00f, 0.68f, 0.49f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.05f, 0.05f, 0.05f, 1.00f)); // Dark Text

        bool pressed = ImGui::Button(label, size);

        ImGui::PopStyleColor(4);
        return pressed;
    }

    bool SubtleButton(const char* label, const ImVec2& size) {
        // Push Dark Grey Colors (#262626)
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.15f, 0.15f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.20f, 0.20f, 0.20f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.25f, 0.25f, 0.25f, 1.00f));

        // We don't push text color here, so it uses the default (Off-White)

        bool pressed = ImGui::Button(label, size);

        ImGui::PopStyleColor(3);
        return pressed;
    }

    void CenteredText(const char* text) {
        float windowWidth = ImGui::GetWindowSize().x;
        float textWidth = ImGui::CalcTextSize(text).x;

        ImGui::SetCursorPosX((windowWidth - textWidth) * 0.5f);
        ImGui::Text("%s", text);
    }

    bool ModernInput(const char* label, char* buf, size_t buf_size, ImGuiInputTextFlags flags) {
        // Add a slight vertical padding for inputs
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10, 8));
        bool result = ImGui::InputText(label, buf, buf_size, flags);
        ImGui::PopStyleVar();
        return result;
    }

}