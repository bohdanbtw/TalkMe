#include "Styles.h"

namespace TalkMe::UI {

    ThemeColors Styles::C = Styles::BuildTheme(ThemeId::Midnight);
    ThemeId     Styles::ActiveTheme = ThemeId::Midnight;

    static ThemeColors MakeMidnight() {
        ThemeColors t{};
        t.bgMain        = {0.07f, 0.07f, 0.08f, 1.f};
        t.bgSidebar     = {0.09f, 0.09f, 0.10f, 1.f};
        t.bgChannelList = {0.09f, 0.09f, 0.10f, 1.f};
        t.bgChat        = {0.11f, 0.11f, 0.12f, 1.f};
        t.bgFooter      = {0.08f, 0.08f, 0.09f, 1.f};
        t.bgPopup       = {0.13f, 0.13f, 0.15f, 1.f};
        t.bgAvatar      = {0.22f, 0.22f, 0.26f, 1.f};
        t.bgCard        = {0.14f, 0.14f, 0.16f, 1.f};

        t.textPrimary   = {0.94f, 0.94f, 0.96f, 1.f};
        t.textSecondary = {0.58f, 0.58f, 0.62f, 1.f};
        t.textMuted     = {0.42f, 0.42f, 0.46f, 1.f};

        t.accent        = {0.28f, 0.72f, 0.55f, 1.f};
        t.accentHover   = {0.34f, 0.82f, 0.62f, 1.f};
        t.accentDim     = {0.22f, 0.58f, 0.44f, 1.f};

        t.speakingGreen = {0.30f, 0.90f, 0.42f, 1.f};
        t.speakingGlow  = {0.25f, 0.72f, 0.35f, 0.40f};
        t.error         = {0.90f, 0.35f, 0.35f, 1.f};

        t.buttonSubtle      = {0.16f, 0.16f, 0.18f, 1.f};
        t.buttonSubtleHover = {0.22f, 0.22f, 0.25f, 1.f};
        t.buttonDanger      = {0.72f, 0.22f, 0.22f, 1.f};
        t.buttonDangerHover = {0.85f, 0.28f, 0.28f, 1.f};
        t.separator         = {0.18f, 0.18f, 0.20f, 1.f};
        t.textOnAccent      = {0.05f, 0.05f, 0.05f, 1.f};

        t.frameBg       = {0.13f, 0.13f, 0.15f, 1.f};
        t.frameBgHover  = {0.17f, 0.17f, 0.19f, 1.f};
        t.frameBgActive = {0.19f, 0.19f, 0.22f, 1.f};
        t.scrollGrab       = {0.30f, 0.30f, 0.34f, 0.55f};
        t.scrollGrabHover  = {0.40f, 0.40f, 0.44f, 0.75f};
        t.scrollGrabActive = {0.50f, 0.50f, 0.54f, 1.f};
        return t;
    }

    static ThemeColors MakeDaylight() {
        ThemeColors t{};
        t.bgMain        = {0.96f, 0.96f, 0.97f, 1.f};
        t.bgSidebar     = {0.92f, 0.92f, 0.94f, 1.f};
        t.bgChannelList = {0.92f, 0.92f, 0.94f, 1.f};
        t.bgChat        = {0.98f, 0.98f, 0.99f, 1.f};
        t.bgFooter      = {0.90f, 0.90f, 0.92f, 1.f};
        t.bgPopup       = {1.00f, 1.00f, 1.00f, 1.f};
        t.bgAvatar      = {0.82f, 0.83f, 0.86f, 1.f};
        t.bgCard        = {0.93f, 0.93f, 0.95f, 1.f};

        t.textPrimary   = {0.10f, 0.10f, 0.12f, 1.f};
        t.textSecondary = {0.40f, 0.40f, 0.44f, 1.f};
        t.textMuted     = {0.56f, 0.56f, 0.60f, 1.f};

        t.accent        = {0.20f, 0.55f, 0.88f, 1.f};
        t.accentHover   = {0.28f, 0.62f, 0.94f, 1.f};
        t.accentDim     = {0.16f, 0.45f, 0.72f, 1.f};

        t.speakingGreen = {0.18f, 0.72f, 0.32f, 1.f};
        t.speakingGlow  = {0.15f, 0.60f, 0.28f, 0.35f};
        t.error         = {0.85f, 0.25f, 0.25f, 1.f};

        t.buttonSubtle      = {0.86f, 0.86f, 0.88f, 1.f};
        t.buttonSubtleHover = {0.80f, 0.80f, 0.83f, 1.f};
        t.buttonDanger      = {0.82f, 0.22f, 0.22f, 1.f};
        t.buttonDangerHover = {0.90f, 0.30f, 0.30f, 1.f};
        t.separator         = {0.84f, 0.84f, 0.86f, 1.f};
        t.textOnAccent      = {1.00f, 1.00f, 1.00f, 1.f};

        t.frameBg       = {0.90f, 0.90f, 0.92f, 1.f};
        t.frameBgHover  = {0.86f, 0.86f, 0.88f, 1.f};
        t.frameBgActive = {0.82f, 0.82f, 0.85f, 1.f};
        t.scrollGrab       = {0.72f, 0.72f, 0.76f, 0.60f};
        t.scrollGrabHover  = {0.62f, 0.62f, 0.66f, 0.80f};
        t.scrollGrabActive = {0.52f, 0.52f, 0.56f, 1.f};
        return t;
    }

    static ThemeColors MakeOcean() {
        ThemeColors t{};
        t.bgMain        = {0.06f, 0.08f, 0.14f, 1.f};
        t.bgSidebar     = {0.07f, 0.10f, 0.17f, 1.f};
        t.bgChannelList = {0.07f, 0.10f, 0.17f, 1.f};
        t.bgChat        = {0.09f, 0.12f, 0.20f, 1.f};
        t.bgFooter      = {0.06f, 0.09f, 0.15f, 1.f};
        t.bgPopup       = {0.10f, 0.13f, 0.22f, 1.f};
        t.bgAvatar      = {0.16f, 0.22f, 0.36f, 1.f};
        t.bgCard        = {0.11f, 0.15f, 0.25f, 1.f};

        t.textPrimary   = {0.88f, 0.92f, 0.98f, 1.f};
        t.textSecondary = {0.52f, 0.58f, 0.70f, 1.f};
        t.textMuted     = {0.38f, 0.44f, 0.56f, 1.f};

        t.accent        = {0.28f, 0.60f, 0.92f, 1.f};
        t.accentHover   = {0.36f, 0.68f, 0.98f, 1.f};
        t.accentDim     = {0.22f, 0.48f, 0.76f, 1.f};

        t.speakingGreen = {0.28f, 0.88f, 0.50f, 1.f};
        t.speakingGlow  = {0.22f, 0.70f, 0.40f, 0.38f};
        t.error         = {0.92f, 0.38f, 0.38f, 1.f};

        t.buttonSubtle      = {0.12f, 0.16f, 0.26f, 1.f};
        t.buttonSubtleHover = {0.16f, 0.22f, 0.34f, 1.f};
        t.buttonDanger      = {0.72f, 0.22f, 0.22f, 1.f};
        t.buttonDangerHover = {0.85f, 0.28f, 0.28f, 1.f};
        t.separator         = {0.14f, 0.18f, 0.28f, 1.f};
        t.textOnAccent      = {1.00f, 1.00f, 1.00f, 1.f};

        t.frameBg       = {0.10f, 0.14f, 0.24f, 1.f};
        t.frameBgHover  = {0.14f, 0.18f, 0.30f, 1.f};
        t.frameBgActive = {0.16f, 0.22f, 0.34f, 1.f};
        t.scrollGrab       = {0.24f, 0.30f, 0.44f, 0.55f};
        t.scrollGrabHover  = {0.32f, 0.38f, 0.52f, 0.75f};
        t.scrollGrabActive = {0.40f, 0.46f, 0.60f, 1.f};
        return t;
    }

    static ThemeColors MakeSakura() {
        ThemeColors t{};
        t.bgMain        = {0.14f, 0.08f, 0.10f, 1.f};
        t.bgSidebar     = {0.16f, 0.10f, 0.12f, 1.f};
        t.bgChannelList = {0.16f, 0.10f, 0.12f, 1.f};
        t.bgChat        = {0.19f, 0.12f, 0.14f, 1.f};
        t.bgFooter      = {0.14f, 0.09f, 0.11f, 1.f};
        t.bgPopup       = {0.20f, 0.13f, 0.16f, 1.f};
        t.bgAvatar      = {0.32f, 0.20f, 0.26f, 1.f};
        t.bgCard        = {0.22f, 0.14f, 0.18f, 1.f};

        t.textPrimary   = {0.96f, 0.90f, 0.92f, 1.f};
        t.textSecondary = {0.66f, 0.52f, 0.56f, 1.f};
        t.textMuted     = {0.50f, 0.40f, 0.44f, 1.f};

        t.accent        = {0.88f, 0.42f, 0.58f, 1.f};
        t.accentHover   = {0.94f, 0.52f, 0.66f, 1.f};
        t.accentDim     = {0.72f, 0.34f, 0.48f, 1.f};

        t.speakingGreen = {0.38f, 0.88f, 0.55f, 1.f};
        t.speakingGlow  = {0.30f, 0.70f, 0.44f, 0.38f};
        t.error         = {0.90f, 0.35f, 0.40f, 1.f};

        t.buttonSubtle      = {0.22f, 0.14f, 0.17f, 1.f};
        t.buttonSubtleHover = {0.30f, 0.19f, 0.23f, 1.f};
        t.buttonDanger      = {0.78f, 0.24f, 0.30f, 1.f};
        t.buttonDangerHover = {0.88f, 0.32f, 0.38f, 1.f};
        t.separator         = {0.26f, 0.16f, 0.20f, 1.f};
        t.textOnAccent      = {1.00f, 1.00f, 1.00f, 1.f};

        t.frameBg       = {0.20f, 0.12f, 0.15f, 1.f};
        t.frameBgHover  = {0.26f, 0.16f, 0.20f, 1.f};
        t.frameBgActive = {0.30f, 0.19f, 0.24f, 1.f};
        t.scrollGrab       = {0.38f, 0.24f, 0.30f, 0.55f};
        t.scrollGrabHover  = {0.48f, 0.32f, 0.38f, 0.75f};
        t.scrollGrabActive = {0.58f, 0.40f, 0.46f, 1.f};
        return t;
    }

    static ThemeColors MakeForest() {
        ThemeColors t{};
        t.bgMain        = {0.06f, 0.10f, 0.07f, 1.f};
        t.bgSidebar     = {0.08f, 0.12f, 0.08f, 1.f};
        t.bgChannelList = {0.08f, 0.12f, 0.08f, 1.f};
        t.bgChat        = {0.10f, 0.15f, 0.11f, 1.f};
        t.bgFooter      = {0.07f, 0.11f, 0.07f, 1.f};
        t.bgPopup       = {0.11f, 0.16f, 0.12f, 1.f};
        t.bgAvatar      = {0.18f, 0.28f, 0.20f, 1.f};
        t.bgCard        = {0.12f, 0.18f, 0.13f, 1.f};

        t.textPrimary   = {0.90f, 0.96f, 0.90f, 1.f};
        t.textSecondary = {0.52f, 0.64f, 0.54f, 1.f};
        t.textMuted     = {0.40f, 0.50f, 0.42f, 1.f};

        t.accent        = {0.35f, 0.78f, 0.45f, 1.f};
        t.accentHover   = {0.42f, 0.86f, 0.52f, 1.f};
        t.accentDim     = {0.28f, 0.62f, 0.36f, 1.f};

        t.speakingGreen = {0.40f, 0.92f, 0.48f, 1.f};
        t.speakingGlow  = {0.32f, 0.74f, 0.38f, 0.40f};
        t.error         = {0.88f, 0.38f, 0.32f, 1.f};

        t.buttonSubtle      = {0.14f, 0.20f, 0.14f, 1.f};
        t.buttonSubtleHover = {0.18f, 0.26f, 0.18f, 1.f};
        t.buttonDanger      = {0.72f, 0.25f, 0.20f, 1.f};
        t.buttonDangerHover = {0.84f, 0.32f, 0.26f, 1.f};
        t.separator         = {0.16f, 0.22f, 0.16f, 1.f};
        t.textOnAccent      = {0.05f, 0.05f, 0.05f, 1.f};

        t.frameBg       = {0.10f, 0.16f, 0.11f, 1.f};
        t.frameBgHover  = {0.14f, 0.20f, 0.14f, 1.f};
        t.frameBgActive = {0.16f, 0.24f, 0.17f, 1.f};
        t.scrollGrab       = {0.26f, 0.36f, 0.28f, 0.55f};
        t.scrollGrabHover  = {0.34f, 0.46f, 0.36f, 0.75f};
        t.scrollGrabActive = {0.42f, 0.54f, 0.44f, 1.f};
        return t;
    }

    ThemeColors Styles::BuildTheme(ThemeId id) {
        switch (id) {
            case ThemeId::Daylight: return MakeDaylight();
            case ThemeId::Ocean:    return MakeOcean();
            case ThemeId::Sakura:   return MakeSakura();
            case ThemeId::Forest:   return MakeForest();
            default:                return MakeMidnight();
        }
    }

    const char* Styles::GetThemeName(ThemeId id) {
        switch (id) {
            case ThemeId::Midnight: return "Midnight";
            case ThemeId::Daylight: return "Daylight";
            case ThemeId::Ocean:    return "Ocean";
            case ThemeId::Sakura:   return "Sakura";
            case ThemeId::Forest:   return "Forest";
            default:                return "Unknown";
        }
    }

    void Styles::SetTheme(ThemeId id) {
        ActiveTheme = id;
        C = BuildTheme(id);
        Apply();
    }

    void Styles::Apply() {
        ImGuiStyle& s = ImGui::GetStyle();

        s.WindowRounding    = Rounding;
        s.ChildRounding     = Rounding * 0.6f;
        s.FrameRounding     = Rounding * 0.6f;
        s.PopupRounding     = Rounding;
        s.ScrollbarRounding = Rounding * 0.4f;
        s.GrabRounding      = Rounding * 0.4f;
        s.TabRounding       = Rounding * 0.4f;

        s.WindowPadding    = ImVec2(14, 14);
        s.FramePadding     = ImVec2(12, 8);
        s.ItemSpacing      = ImVec2(10, 8);
        s.ItemInnerSpacing = ImVec2(8, 6);
        s.IndentSpacing    = 20.0f;
        s.ScrollbarSize    = 5.0f;

        s.WindowBorderSize = 0.0f;
        s.ChildBorderSize  = 0.0f;
        s.PopupBorderSize  = 1.0f;
        s.FrameBorderSize  = 0.0f;

        ImVec4* c = s.Colors;
        c[ImGuiCol_WindowBg]             = C.bgMain;
        c[ImGuiCol_ChildBg]              = C.bgSidebar;
        c[ImGuiCol_PopupBg]              = C.bgPopup;
        c[ImGuiCol_Text]                 = C.textPrimary;
        c[ImGuiCol_TextDisabled]         = C.textMuted;
        c[ImGuiCol_Border]               = C.separator;
        c[ImGuiCol_Header]               = C.buttonSubtle;
        c[ImGuiCol_HeaderHovered]        = C.buttonSubtleHover;
        c[ImGuiCol_HeaderActive]         = C.frameBgActive;
        c[ImGuiCol_FrameBg]              = C.frameBg;
        c[ImGuiCol_FrameBgHovered]       = C.frameBgHover;
        c[ImGuiCol_FrameBgActive]        = C.frameBgActive;
        c[ImGuiCol_Button]               = C.buttonSubtle;
        c[ImGuiCol_ButtonHovered]        = C.buttonSubtleHover;
        c[ImGuiCol_ButtonActive]         = C.frameBgActive;
        c[ImGuiCol_Separator]            = C.separator;
        c[ImGuiCol_ScrollbarBg]          = ImVec4(0,0,0,0);
        c[ImGuiCol_ScrollbarGrab]        = C.scrollGrab;
        c[ImGuiCol_ScrollbarGrabHovered] = C.scrollGrabHover;
        c[ImGuiCol_ScrollbarGrabActive]  = C.scrollGrabActive;
        c[ImGuiCol_SliderGrab]           = C.accent;
        c[ImGuiCol_SliderGrabActive]     = C.accentHover;
        c[ImGuiCol_CheckMark]            = C.accent;
    }
}
