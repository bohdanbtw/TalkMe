#include "Components.h"
#include "Styles.h"

namespace TalkMe::UI {

    bool AccentButton(const char* label, const ImVec2& size) {
        ImGui::PushStyleColor(ImGuiCol_Button, Styles::Accent());
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Styles::AccentHover());
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, Styles::AccentDim());
        ImGui::PushStyleColor(ImGuiCol_Text, Styles::TextOnAccent());

        bool pressed = ImGui::Button(label, size);

        ImGui::PopStyleColor(4);
        return pressed;
    }

    bool SubtleButton(const char* label, const ImVec2& size) {
        ImGui::PushStyleColor(ImGuiCol_Button, Styles::ButtonSubtle());
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Styles::ButtonSubtleHover());
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, Styles::ButtonSubtleHover());

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