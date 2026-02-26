#include "SettingsView.h"
#include "../Styles.h"
#include "../Components.h"
#include "../../core/ConfigManager.h"
#include "../../audio/AudioEngine.h"
#include "Version.h"
#include "qrcodegen.hpp"
#include <cstring>
#include <cstdio>
#include <commdlg.h>
#pragma comment(lib, "comdlg32.lib")
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

    static void RenderAppearanceTab(SettingsContext& ctx, float contentW) {
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

        ImGui::Dummy(ImVec2(0, 24));
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0, 12));

        ImGui::Text("Font Size");
        ImGui::TextDisabled("Adjust the UI text scale");
        ImGui::Dummy(ImVec2(0, 4));
        static float s_fontScale = ImGui::GetIO().FontGlobalScale;
        if (ImGui::SliderFloat("##fontscale", &s_fontScale, 0.7f, 1.5f, "%.1fx")) {
            ImGui::GetIO().FontGlobalScale = s_fontScale;
        }

        ImGui::Dummy(ImVec2(0, 12));
        ImGui::Text("Layout");
        ImGui::Dummy(ImVec2(0, 4));
        static bool s_compact = false;
        if (ImGui::Checkbox("Compact Mode", &s_compact)) {
            ImGuiStyle& st = ImGui::GetStyle();
            if (s_compact) {
                st.ItemSpacing = ImVec2(6, 4);
                st.FramePadding = ImVec2(8, 4);
                st.WindowPadding = ImVec2(8, 8);
            } else {
                st.ItemSpacing = ImVec2(10, 8);
                st.FramePadding = ImVec2(12, 8);
                st.WindowPadding = ImVec2(14, 14);
            }
        }
        ImGui::TextDisabled("Reduce padding between UI elements");

        ImGui::Dummy(ImVec2(0, 24));
        if (ImGui::Button("Reset to defaults")) {
            if (ctx.onResetToDefaults) ctx.onResetToDefaults();
            s_fontScale = 1.0f;
            ImGui::GetIO().FontGlobalScale = 1.0f;
            s_compact = false;
        }
    }

    static void RenderVoiceTab(SettingsContext& ctx) {
        ImGui::Text("Microphone Level");
        ImGui::Dummy(ImVec2(0, 4));
        const int kMicBarSegments = 50;
        const float barHeight = 14.0f;
        const float barWidth = 400.0f;
        const float segWidth = barWidth / kMicBarSegments;
        // 0.08 = 100% bar; raw mic speech typically 0.02–0.12 so bar fills appropriately
        float normalized = ctx.micActivity / 0.08f;
        if (normalized > 1.0f) normalized = 1.0f;
        int fillCount = static_cast<int>(normalized * kMicBarSegments + 0.5f);
        ImVec2 barMin = ImGui::GetCursorScreenPos();
        ImVec2 barMax = ImVec2(barMin.x + barWidth, barMin.y + barHeight);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImU32 colEmpty = ImGui::ColorConvertFloat4ToU32(Styles::ButtonSubtle());
        for (int i = 0; i < kMicBarSegments; ++i) {
            float x0 = barMin.x + i * segWidth;
            float x1 = x0 + segWidth - 2.0f;
            if (x1 < x0 + 1.0f) x1 = x0 + 1.0f;
            ImVec2 s0(x0, barMin.y), s1(x1, barMax.y);
            bool filled = (i < fillCount);
            ImU32 col;
            if (filled) {
                float t = (float)i / (float)kMicBarSegments;
                if (t < 0.4f)
                    col = IM_COL32(255, 180, 60, 255);
                else if (t < 0.7f)
                    col = IM_COL32(200, 220, 80, 255);
                else
                    col = ImGui::ColorConvertFloat4ToU32(Styles::SpeakingGreen());
            } else {
                col = colEmpty;
            }
            dl->AddRectFilled(s0, s1, col, 2.0f);
        }
        ImGui::Dummy(ImVec2(barWidth, barHeight));
        ImGui::Dummy(ImVec2(0, 12));

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

        ImGui::Dummy(ImVec2(0, 20));
        ImGui::Text("Noise Suppression Algorithm");
        ImGui::Dummy(ImVec2(0, 6));

        const char* nsModes[] = { "Disabled", "RNNoise (AI - Recommended)", "SpeexDSP (Statistical)", "WebRTC APM" };
        const int nsModeCount = 4;
        int nsIdx = ctx.noiseSuppressionMode;
        if (nsIdx < 0 || nsIdx >= nsModeCount) nsIdx = 1;
        ImGui::PushItemWidth(300);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, Styles::ButtonSubtle());
        if (ImGui::BeginCombo("##ns_mode", nsModes[nsIdx])) {
            for (int i = 0; i < nsModeCount; i++) {
                bool sel = (ctx.noiseSuppressionMode == i);
                if (ImGui::Selectable(nsModes[i], sel)) {
                    ctx.noiseSuppressionMode = i;
                    if (ctx.onNoiseSuppressionModeChange) ctx.onNoiseSuppressionModeChange(i);
                }
                if (sel) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::PopStyleColor();
        ImGui::PopItemWidth();

        ImGui::Dummy(ImVec2(0, 20));
        ImGui::Text("Microphone Testing");
        ImGui::Dummy(ImVec2(0, 6));

        if (ImGui::Checkbox("Test Microphone (Listen to yourself)", &ctx.testMicEnabled)) {
            if (ctx.onToggleTestMic) ctx.onToggleTestMic(ctx.testMicEnabled);
        }
        if (ctx.testMicEnabled) {
            ImGui::SameLine();
            ImGui::TextColored(Styles::Accent(), "  Testing active...");
        }

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
        if (ImGui::Button("Reset overlay to defaults")) {
            if (ctx.onResetOverlayToDefaults) ctx.onResetOverlayToDefaults();
        }

        ImGui::Dummy(ImVec2(0, 20));
        ImGui::PushStyleColor(ImGuiCol_Text, Styles::TextMuted());
        ImGui::TextWrapped("The overlay uses a standard Windows topmost window. "
            "It does not inject into game processes or hook any APIs, "
            "making it 100%% safe with all anti-cheat systems (VAC, EAC, BattlEye, etc.).");
        ImGui::PopStyleColor();

        if (changed && ctx.onOverlayChanged)
            ctx.onOverlayChanged();
    }

    static void RenderNotificationsTab(SettingsContext& ctx) {
        if (!ctx.notifVolume) return;
        ImGui::Text("Notification Volume");
        ImGui::TextDisabled("Controls the volume of all notification sounds");
        ImGui::Dummy(ImVec2(0, 8));
        ImGui::SliderFloat("##notif_vol", ctx.notifVolume, 0.0f, 1.0f, "%.0f%%");
        ImGui::Dummy(ImVec2(0, 16));

        ImGui::Text("Notification Types");
        ImGui::Dummy(ImVec2(0, 8));
        ImGui::Checkbox("Mute @mention notifications", ctx.notifMuteMentions);
        ImGui::TextDisabled("  When enabled, you won't hear a sound when someone @mentions you");
        ImGui::Dummy(ImVec2(0, 4));
        ImGui::Checkbox("Mute new message notifications", ctx.notifMuteMessages);
        ImGui::TextDisabled("  When enabled, no sound for new messages when app is not focused");
        ImGui::Dummy(ImVec2(0, 4));
        ImGui::Checkbox("Mute join/leave sounds", ctx.notifMuteJoinLeave);
        ImGui::TextDisabled("  When enabled, no sound when users join/leave voice channels");

        ImGui::Dummy(ImVec2(0, 24));
        ImGui::Text("Mention Format");
        ImGui::TextDisabled("Use @username or @username#0001 or @all to mention users in chat");
        ImGui::TextDisabled("Mentioned users will hear a distinct notification sound");
    }

    static void RenderAccountTab(SettingsContext& ctx) {
        ImGui::Text("Security");
        ImGui::TextDisabled("Two-factor authentication (TOTP)");
        ImGui::Dummy(ImVec2(0, 8));
        if (ctx.is2FAEnabled) {
            ImGui::TextColored(ImVec4(0.2f, 0.8f, 0.2f, 1.0f), "Status: Successfully Connected");
            ImGui::Dummy(ImVec2(0, 8));
            if (!ctx.isDisabling2FA) {
                if (ImGui::Button("Disable Two-Factor Authentication", ImVec2(280, 32))) {
                    if (ctx.onDisable2FAClick) ctx.onDisable2FAClick();
                }
            } else {
                ImGui::PushItemWidth(140);
                ImGui::InputText("Confirm 6-Digit Code", ctx.disable2FACodeBuf, ctx.disable2FACodeBufSize, ImGuiInputTextFlags_CharsDecimal);
                ImGui::PopItemWidth();
                ImGui::Dummy(ImVec2(0, 6));
                if (ImGui::Button("Confirm Disable", ImVec2(140, 32))) {
                    if (ctx.onConfirmDisable2FAClick) ctx.onConfirmDisable2FAClick();
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel", ImVec2(80, 32))) {
                    if (ctx.onCancelDisable2FAClick) ctx.onCancelDisable2FAClick();
                }
            }
        } else if (!ctx.isSettingUp2FA) {
            if (ImGui::Button("Enable Two-Factor Authentication", ImVec2(280, 32))) {
                if (ctx.onEnable2FAClick) ctx.onEnable2FAClick();
            }
        } else {
            ImGui::TextWrapped("Scan the QR code below with Google Authenticator, or manually enter this secret:");
            ImGui::TextDisabled("copiable");
            static char secretCopyBuf[128];
            if (ctx.twoFASecretStr.size() < sizeof(secretCopyBuf)) {
                std::memcpy(secretCopyBuf, ctx.twoFASecretStr.c_str(), ctx.twoFASecretStr.size() + 1);
            } else {
                std::memcpy(secretCopyBuf, ctx.twoFASecretStr.c_str(), sizeof(secretCopyBuf) - 1);
                secretCopyBuf[sizeof(secretCopyBuf) - 1] = '\0';
            }
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.5f, 0.5f, 0.6f, 0.8f));
            ImGui::InputText("##2FA_secret", secretCopyBuf, sizeof(secretCopyBuf), ImGuiInputTextFlags_ReadOnly);
            ImGui::SameLine();
            if (ImGui::Button("Copy")) ImGui::SetClipboardText(ctx.twoFASecretStr.c_str());
            ImGui::PopStyleColor();
            ImGui::PopStyleVar();
            ImGui::Dummy(ImVec2(0, 4));
            if (!ctx.twoFAUriStr.empty()) {
                try {
                    qrcodegen::QrCode qr = qrcodegen::QrCode::encodeText(ctx.twoFAUriStr.c_str(), qrcodegen::QrCode::Ecc::LOW);
                    ImDrawList* drawList = ImGui::GetWindowDrawList();
                    ImVec2 p = ImGui::GetCursorScreenPos();
                    const float sz = 6.0f;
                    const int border = 2;
                    const int total = qr.getSize() + border * 2;
                    const ImU32 black = IM_COL32(0, 0, 0, 255);
                    const ImU32 white = IM_COL32(255, 255, 255, 255);
                    for (int j = 0; j < total; j++)
                        for (int i = 0; i < total; i++) {
                            int mx = i - border, my = j - border;
                            bool dark = (mx >= 0 && mx < qr.getSize() && my >= 0 && my < qr.getSize() && qr.getModule(mx, my));
                            drawList->AddRectFilled(
                                ImVec2(p.x + (float)i * sz, p.y + (float)j * sz),
                                ImVec2(p.x + (float)(i + 1) * sz, p.y + (float)(j + 1) * sz),
                                dark ? black : white);
                        }
                    ImGui::Dummy(ImVec2((float)total * sz, (float)total * sz));
                } catch (...) {}
            }
            ImGui::Dummy(ImVec2(0, 8));
            ImGui::PushItemWidth(120);
            ImGui::InputText("6-Digit Code", ctx.twoFASetupCodeBuf, ctx.twoFASetupCodeBufSize, ImGuiInputTextFlags_CharsDecimal);
            ImGui::PopItemWidth();
            if (ctx.twoFASetupStatusMessage[0]) ImGui::TextDisabled("%s", ctx.twoFASetupStatusMessage);
            ImGui::Dummy(ImVec2(0, 6));
            if (ImGui::Button("Verify & Enable", ImVec2(160, 32))) {
                if (ctx.onVerify2FAClick) ctx.onVerify2FAClick();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(80, 32))) {
                if (ctx.onCancel2FAClick) ctx.onCancel2FAClick();
            }
        }
        // Profile Picture
        ImGui::Dummy(ImVec2(0, 24));
        ImGui::Text("Profile Picture");
        ImGui::TextDisabled("Upload a JPEG or PNG image (max 200KB)");
        ImGui::Dummy(ImVec2(0, 8));

        if (ctx.currentAvatarTexture) {
            ImGui::Image((ImTextureID)ctx.currentAvatarTexture, ImVec2(80, 80));
            ImGui::SameLine();
        }

        if (ImGui::Button("Change Avatar", ImVec2(140, 32))) {
            char filePath[MAX_PATH] = {};
            OPENFILENAMEA ofn = {};
            ofn.lStructSize = sizeof(ofn);
            ofn.lpstrFilter = "Images\0*.png;*.jpg;*.jpeg\0";
            ofn.lpstrFile = filePath;
            ofn.nMaxFile = MAX_PATH;
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
            if (GetOpenFileNameA(&ofn) && strlen(filePath) > 0) {
                FILE* fp = fopen(filePath, "rb");
                if (fp) {
                    fseek(fp, 0, SEEK_END);
                    long sz = ftell(fp);
                    fseek(fp, 0, SEEK_SET);
                    if (sz > 0 && sz < 200 * 1024) {
                        std::vector<uint8_t> data(sz);
                        fread(data.data(), 1, sz, fp);
                        // Base64 encode
                        static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
                        std::string encoded;
                        encoded.reserve((sz + 2) / 3 * 4);
                        for (int i = 0; i < sz; i += 3) {
                            int n = (data[i] << 16) | (i + 1 < sz ? data[i + 1] << 8 : 0) | (i + 2 < sz ? data[i + 2] : 0);
                            encoded += b64[(n >> 18) & 63];
                            encoded += b64[(n >> 12) & 63];
                            encoded += (i + 1 < sz) ? b64[(n >> 6) & 63] : '=';
                            encoded += (i + 2 < sz) ? b64[n & 63] : '=';
                        }
                        if (ctx.onSetAvatar) ctx.onSetAvatar(encoded);
                    }
                    fclose(fp);
                }
            }
        }

        ImGui::Dummy(ImVec2(0, 24));
        ImGui::Text("Sign Out");
        ImGui::TextDisabled("Sign out of your account on this device");
        ImGui::Dummy(ImVec2(0, 12));
        ImGui::PushStyleColor(ImGuiCol_Button, Styles::ButtonDanger());
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Styles::ButtonDangerHover());
        ImGui::PushStyleColor(ImGuiCol_Text, Styles::TextOnAccent());
        if (ImGui::Button("Log Out", ImVec2(200, 38))) {
            if (ctx.onLogout) ctx.onLogout();
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

        const char* tabs[] = { "Appearance", "Voice", "Keybinds", "Overlay", "Notifications", "Account" };
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
        ImGui::TextDisabled("TalkMe v" TALKME_VERSION_STRING);
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
            case 0: RenderAppearanceTab(ctx, contentW); break;
            case 1: RenderVoiceTab(ctx); break;
            case 2: RenderKeybindsTab(ctx.keyMuteMic, ctx.keyDeafen); break;
            case 3: RenderOverlayTab(ctx); break;
            case 4: RenderNotificationsTab(ctx); break;
            case 5: RenderAccountTab(ctx); break;
        }

        ImGui::EndGroup();

        ImGui::EndChild();

        ImGui::EndChild();
        ImGui::PopStyleColor();
    }
}
