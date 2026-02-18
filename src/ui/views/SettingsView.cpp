#include "SettingsView.h"
#include "../Styles.h"
#include "../Components.h"
#include "../../core/ConfigManager.h"
#include "../../audio/AudioEngine.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace TalkMe::UI::Views {

    static void DrawColorSwatch(ImDrawList* dl, ImVec2 pos, float sz, ImVec4 col) {
        dl->AddRectFilled(pos, ImVec2(pos.x + sz, pos.y + sz),
            ImGui::ColorConvertFloat4ToU32(col), 4.0f);
    }

    static const char* SingleKeyName(int vk) {
        if (vk == 0) return "?";
        static char buf[64];
        if (vk == VK_CONTROL) return "Ctrl";
        if (vk == VK_SHIFT) return "Shift";
        if (vk == VK_MENU) return "Alt";
        UINT sc = MapVirtualKeyA(vk, MAPVK_VK_TO_VSC);
        if (GetKeyNameTextA(sc << 16, buf, sizeof(buf)) > 0) return buf;
        snprintf(buf, sizeof(buf), "0x%02X", vk);
        return buf;
    }

    static std::string ComboName(const std::vector<int>& combo) {
        if (combo.empty()) return "None";
        std::string result;
        for (size_t i = 0; i < combo.size(); i++) {
            if (i > 0) result += " + ";
            result += SingleKeyName(combo[i]);
        }
        return result;
    }

    static bool IsModifier(int vk) {
        return vk == VK_CONTROL || vk == VK_SHIFT || vk == VK_MENU
            || vk == VK_LCONTROL || vk == VK_RCONTROL
            || vk == VK_LSHIFT || vk == VK_RSHIFT
            || vk == VK_LMENU || vk == VK_RMENU;
    }

    static std::vector<int> CaptureKeyCombo() {
        std::vector<int> result;
        bool hasNonModifier = false;

        if (GetAsyncKeyState(VK_CONTROL) & 0x8000) result.push_back(VK_CONTROL);
        if (GetAsyncKeyState(VK_SHIFT) & 0x8000) result.push_back(VK_SHIFT);
        if (GetAsyncKeyState(VK_MENU) & 0x8000) result.push_back(VK_MENU);

        for (int vk = 1; vk < 256; vk++) {
            if (vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_ESCAPE) continue;
            if (IsModifier(vk)) continue;
            if (GetAsyncKeyState(vk) & 0x8000) {
                result.push_back(vk);
                hasNonModifier = true;
            }
        }

        if (hasNonModifier) return result;
        return {};
    }

    static void RenderAppearanceTab(float contentW) {
        ImGui::Text("Theme");
        ImGui::TextDisabled("Choose a color theme");
        ImGui::Dummy(ImVec2(0, 16));

        float cardW = 145.0f;
        float cardH = 105.0f;
        float cardGap = 14.0f;
        float swatchSz = 12.0f;
        int themeCount = (int)ThemeId::Count;

        float startX = ImGui::GetCursorPosX();

        for (int i = 0; i < themeCount; i++) {
            ThemeId tid = (ThemeId)i;
            ThemeColors tc = Styles::BuildTheme(tid);
            bool isActive = (Styles::ActiveTheme == tid);

            float cx = startX + i * (cardW + cardGap);
            if (cx + cardW > startX + contentW + 20) {
                ImGui::Dummy(ImVec2(0, cardH + cardGap));
                cx = startX;
            }
            if (i > 0 && cx > startX) ImGui::SameLine(0, cardGap);

            ImGui::BeginGroup();
            ImVec2 cardPos = ImGui::GetCursorScreenPos();
            ImDrawList* dl = ImGui::GetWindowDrawList();

            ImU32 cardBg = isActive
                ? ImGui::ColorConvertFloat4ToU32(Styles::Accent())
                : ImGui::ColorConvertFloat4ToU32(Styles::ButtonSubtle());
            dl->AddRectFilled(cardPos, ImVec2(cardPos.x + cardW, cardPos.y + cardH), cardBg, 8.0f);

            float previewY = cardPos.y + 10;
            float previewH = 46.0f;
            dl->AddRectFilled(ImVec2(cardPos.x + 10, previewY), ImVec2(cardPos.x + cardW - 10, previewY + previewH),
                ImGui::ColorConvertFloat4ToU32(tc.bgMain), 4.0f);

            float sx = cardPos.x + 14;
            float sy = previewY + 6;
            DrawColorSwatch(dl, ImVec2(sx, sy), swatchSz, tc.bgSidebar);
            DrawColorSwatch(dl, ImVec2(sx + 16, sy), swatchSz, tc.accent);
            DrawColorSwatch(dl, ImVec2(sx + 32, sy), swatchSz, tc.textPrimary);
            DrawColorSwatch(dl, ImVec2(sx + 48, sy), swatchSz, tc.error);

            dl->AddRectFilled(ImVec2(cardPos.x + 12, previewY + 24), ImVec2(cardPos.x + 26, previewY + 40),
                ImGui::ColorConvertFloat4ToU32(tc.bgSidebar), 3.0f);
            dl->AddRectFilled(ImVec2(cardPos.x + 30, previewY + 24), ImVec2(cardPos.x + cardW - 12, previewY + 40),
                ImGui::ColorConvertFloat4ToU32(tc.bgChat), 3.0f);

            const char* name = Styles::GetThemeName(tid);
            ImVec2 nameSize = ImGui::CalcTextSize(name);
            dl->AddText(ImVec2(cardPos.x + (cardW - nameSize.x) * 0.5f, cardPos.y + cardH - 24),
                isActive ? ImGui::ColorConvertFloat4ToU32(Styles::TextOnAccent()) : ImGui::ColorConvertFloat4ToU32(Styles::TextPrimary()),
                name);

            ImGui::InvisibleButton(("theme_" + std::to_string(i)).c_str(), ImVec2(cardW, cardH));
            if (ImGui::IsItemClicked()) {
                Styles::SetTheme(tid);
                ConfigManager::Get().SaveTheme((int)tid);
            }
            if (ImGui::IsItemHovered() && !isActive)
                dl->AddRect(cardPos, ImVec2(cardPos.x + cardW, cardPos.y + cardH),
                    ImGui::ColorConvertFloat4ToU32(Styles::AccentDim()), 8.0f, 0, 2.0f);

            ImGui::EndGroup();
        }
    }

    static void RenderVoiceTab(SettingsContext& ctx) {
        ImGui::Text("Input Device (Microphone)");
        ImGui::Dummy(ImVec2(0, 6));

        const char* inputPreview = "System Default";
        if (ctx.selectedInputIdx >= 0 && ctx.selectedInputIdx < (int)ctx.inputDevices.size())
            inputPreview = ctx.inputDevices[ctx.selectedInputIdx].name.c_str();
        else if (!ctx.inputDevices.empty()) {
            for (auto& d : ctx.inputDevices) {
                if (d.isDefault) { inputPreview = d.name.c_str(); break; }
            }
        }

        ImGui::PushItemWidth(400);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, Styles::ButtonSubtle());
        if (ImGui::BeginCombo("##input_dev", inputPreview)) {
            if (ImGui::Selectable("System Default", ctx.selectedInputIdx < 0)) {
                ctx.selectedInputIdx = -1;
                if (ctx.onDeviceChange) ctx.onDeviceChange(ctx.selectedInputIdx, ctx.selectedOutputIdx);
            }
            for (int i = 0; i < (int)ctx.inputDevices.size(); i++) {
                bool sel = (ctx.selectedInputIdx == i);
                std::string label = ctx.inputDevices[i].name;
                if (ctx.inputDevices[i].isDefault) label += " (Default)";
                if (ImGui::Selectable(label.c_str(), sel)) {
                    ctx.selectedInputIdx = i;
                    if (ctx.onDeviceChange) ctx.onDeviceChange(ctx.selectedInputIdx, ctx.selectedOutputIdx);
                }
                if (sel) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::PopStyleColor();
        ImGui::PopItemWidth();

        ImGui::Dummy(ImVec2(0, 20));
        ImGui::Text("Output Device (Headset/Speakers)");
        ImGui::Dummy(ImVec2(0, 6));

        const char* outputPreview = "System Default";
        if (ctx.selectedOutputIdx >= 0 && ctx.selectedOutputIdx < (int)ctx.outputDevices.size())
            outputPreview = ctx.outputDevices[ctx.selectedOutputIdx].name.c_str();
        else if (!ctx.outputDevices.empty()) {
            for (auto& d : ctx.outputDevices) {
                if (d.isDefault) { outputPreview = d.name.c_str(); break; }
            }
        }

        ImGui::PushItemWidth(400);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, Styles::ButtonSubtle());
        if (ImGui::BeginCombo("##output_dev", outputPreview)) {
            if (ImGui::Selectable("System Default", ctx.selectedOutputIdx < 0)) {
                ctx.selectedOutputIdx = -1;
                if (ctx.onDeviceChange) ctx.onDeviceChange(ctx.selectedInputIdx, ctx.selectedOutputIdx);
            }
            for (int i = 0; i < (int)ctx.outputDevices.size(); i++) {
                bool sel = (ctx.selectedOutputIdx == i);
                std::string label = ctx.outputDevices[i].name;
                if (ctx.outputDevices[i].isDefault) label += " (Default)";
                if (ImGui::Selectable(label.c_str(), sel)) {
                    ctx.selectedOutputIdx = i;
                    if (ctx.onDeviceChange) ctx.onDeviceChange(ctx.selectedInputIdx, ctx.selectedOutputIdx);
                }
                if (sel) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::PopStyleColor();
        ImGui::PopItemWidth();

        ImGui::Dummy(ImVec2(0, 16));
        ImGui::TextDisabled("Device changes take effect immediately");
    }

    static void RenderKeybindsTab(std::vector<int>& keyMuteMic, std::vector<int>& keyDeafen) {
        static bool capturingMute = false;
        static bool capturingDeafen = false;

        ImGui::Text("Toggle Mute Microphone");
        ImGui::Dummy(ImVec2(0, 4));
        if (capturingMute) {
            ImGui::PushStyleColor(ImGuiCol_Button, Styles::Accent());
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Styles::AccentHover());
            ImGui::Button("Press keys...", ImVec2(300, 34));
            ImGui::PopStyleColor(2);

            auto combo = CaptureKeyCombo();
            if (!combo.empty()) {
                keyMuteMic = combo;
                capturingMute = false;
                ConfigManager::Get().SaveKeybinds(keyMuteMic, keyDeafen);
            }
        } else {
            std::string label = ComboName(keyMuteMic) + "##bind_mute";
            if (ImGui::Button(label.c_str(), ImVec2(300, 34))) capturingMute = true;
            ImGui::SameLine();
            if (!keyMuteMic.empty() && ImGui::Button("Clear##clr_mute")) {
                keyMuteMic.clear();
                ConfigManager::Get().SaveKeybinds(keyMuteMic, keyDeafen);
            }
        }

        ImGui::Dummy(ImVec2(0, 20));
        ImGui::Text("Toggle Deafen");
        ImGui::Dummy(ImVec2(0, 4));
        if (capturingDeafen) {
            ImGui::PushStyleColor(ImGuiCol_Button, Styles::Accent());
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Styles::AccentHover());
            ImGui::Button("Press keys...", ImVec2(300, 34));
            ImGui::PopStyleColor(2);

            auto combo = CaptureKeyCombo();
            if (!combo.empty()) {
                keyDeafen = combo;
                capturingDeafen = false;
                ConfigManager::Get().SaveKeybinds(keyMuteMic, keyDeafen);
            }
        } else {
            std::string label = ComboName(keyDeafen) + "##bind_deaf";
            if (ImGui::Button(label.c_str(), ImVec2(300, 34))) capturingDeafen = true;
            ImGui::SameLine();
            if (!keyDeafen.empty() && ImGui::Button("Clear##clr_deaf")) {
                keyDeafen.clear();
                ConfigManager::Get().SaveKeybinds(keyMuteMic, keyDeafen);
            }
        }

        ImGui::Dummy(ImVec2(0, 20));
        ImGui::TextDisabled("Click a keybind to reassign. Press a key combination to set.");
        ImGui::TextDisabled("Supports combinations like Ctrl + Shift + M");
    }

    static void RenderOverlayTab(SettingsContext& ctx) {
        ImGui::Text("Game Overlay");
        ImGui::TextDisabled("Show voice chat members on top of games");

        ImGui::Dummy(ImVec2(0, 12));

        bool changed = false;
        if (ImGui::Checkbox("Enable Overlay", &ctx.overlayEnabled)) changed = true;

        ImGui::Dummy(ImVec2(0, 16));
        ImGui::Text("Position");
        ImGui::Dummy(ImVec2(0, 4));

        const char* corners[] = { "Top Left", "Top Right", "Bottom Left", "Bottom Right" };
        ImGui::PushStyleColor(ImGuiCol_FrameBg, Styles::ButtonSubtle());
        ImGui::PushItemWidth(200);
        if (ImGui::BeginCombo("##overlay_pos", corners[ctx.overlayCorner])) {
            for (int i = 0; i < 4; i++) {
                bool sel = (ctx.overlayCorner == i);
                if (ImGui::Selectable(corners[i], sel)) {
                    ctx.overlayCorner = i;
                    changed = true;
                }
                if (sel) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::PopItemWidth();
        ImGui::PopStyleColor();

        ImGui::Dummy(ImVec2(0, 16));
        ImGui::Text("Opacity");
        ImGui::Dummy(ImVec2(0, 4));

        ImGui::PushItemWidth(200);
        if (ImGui::SliderFloat("##overlay_opacity", &ctx.overlayOpacity, 0.2f, 1.0f, "%.0f%%")) {
            changed = true;
        }
        ImGui::PopItemWidth();

        ImGui::Dummy(ImVec2(0, 20));
        ImGui::PushStyleColor(ImGuiCol_Text, Styles::TextMuted());
        ImGui::TextWrapped("The overlay uses a standard Windows topmost window. "
            "It does not inject into game processes or hook any APIs, "
            "making it 100%% safe with all anti-cheat systems (VAC, EAC, BattlEye, etc.).");
        ImGui::PopStyleColor();

        if (changed && ctx.onOverlayChanged)
            ctx.onOverlayChanged();
    }

    static void RenderAccountTab(std::function<void()>& onLogout) {
        ImGui::Text("Sign Out");
        ImGui::TextDisabled("Sign out of your account on this device");

        ImGui::Dummy(ImVec2(0, 12));
        ImGui::PushStyleColor(ImGuiCol_Button, Styles::ButtonDanger());
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Styles::ButtonDangerHover());
        ImGui::PushStyleColor(ImGuiCol_Text, Styles::TextOnAccent());
        if (ImGui::Button("Log Out", ImVec2(200, 38))) {
            if (onLogout) onLogout();
        }
        ImGui::PopStyleColor(3);
    }

    void RenderSettings(SettingsContext& ctx) {
        float windowWidth  = ImGui::GetWindowWidth();
        float windowHeight = ImGui::GetWindowHeight();
        float leftOffset   = Styles::MainContentLeftOffset;
        float areaW        = windowWidth - leftOffset - Styles::ServerRailWidth;

        ImGui::SetCursorPos(ImVec2(leftOffset, 0));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, Styles::BgChat());
        ImGui::BeginChild("SettingsArea", ImVec2(areaW, windowHeight), false, ImGuiWindowFlags_None);

        float navW = 180.0f;
        float dividerX = navW;

        ImGui::PushStyleColor(ImGuiCol_ChildBg, Styles::BgSidebar());
        ImGui::BeginChild("SettingsNav", ImVec2(navW, windowHeight), false, ImGuiWindowFlags_NoScrollbar);

        ImGui::Dummy(ImVec2(0, 28));
        ImGui::Indent(16);
        ImGui::PushStyleColor(ImGuiCol_Text, Styles::TextMuted());
        ImGui::Text("SETTINGS");
        ImGui::PopStyleColor();
        ImGui::Unindent(16);
        ImGui::Dummy(ImVec2(0, 12));

        const char* tabs[] = { "Appearance", "Voice", "Keybinds", "Overlay", "Account" };
        for (int i = 0; i < 5; i++) {
            bool active = (ctx.settingsTab == i);

            if (active) {
                ImGui::PushStyleColor(ImGuiCol_Button, Styles::Accent());
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Styles::AccentHover());
                ImGui::PushStyleColor(ImGuiCol_Text, Styles::TextOnAccent());
            } else {
                ImVec4 transparent = ImVec4(0, 0, 0, 0);
                ImGui::PushStyleColor(ImGuiCol_Button, transparent);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Styles::ButtonSubtle());
                ImGui::PushStyleColor(ImGuiCol_Text, Styles::TextPrimary());
            }
            ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.0f, 0.5f));
            if (ImGui::Button(tabs[i], ImVec2(navW - 24, 32)))
                ctx.settingsTab = i;
            ImGui::PopStyleVar();
            ImGui::PopStyleColor(3);

            ImGui::Dummy(ImVec2(0, 2));
        }

        ImGui::Dummy(ImVec2(0, 20));
        ImGui::Indent(16);
        ImGui::TextDisabled("TalkMe v1.0");
        ImGui::Unindent(16);

        ImGui::EndChild();
        ImGui::PopStyleColor();

        ImGui::SetCursorPos(ImVec2(dividerX, 0));
        ImGui::BeginChild("SettingsContent", ImVec2(areaW - navW, windowHeight), false, ImGuiWindowFlags_None);

        float contentPad = 40.0f;
        float contentW = areaW - navW - contentPad * 2;
        if (contentW < 300.0f) contentW = 300.0f;

        ImGui::SetCursorPos(ImVec2(contentPad, 32));

        ImGui::PushStyleColor(ImGuiCol_Text, Styles::Accent());
        ImGui::SetWindowFontScale(1.4f);
        ImGui::Text("%s", tabs[ctx.settingsTab]);
        ImGui::SetWindowFontScale(1.0f);
        ImGui::PopStyleColor();

        ImGui::Dummy(ImVec2(0, 6));
        ImGui::PushStyleColor(ImGuiCol_Separator, Styles::Separator());
        ImGui::Separator();
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0, 16));

        ImGui::SetCursorPosX(contentPad);
        ImGui::BeginGroup();

        switch (ctx.settingsTab) {
            case 0: RenderAppearanceTab(contentW); break;
            case 1: RenderVoiceTab(ctx); break;
            case 2: RenderKeybindsTab(ctx.keyMuteMic, ctx.keyDeafen); break;
            case 3: RenderOverlayTab(ctx); break;
            case 4: RenderAccountTab(ctx.onLogout); break;
        }

        ImGui::EndGroup();

        ImGui::EndChild();

        ImGui::EndChild();
        ImGui::PopStyleColor();
    }
}
