#pragma once
#include <imgui.h>
#include <string>

namespace TalkMe::UI {

    enum class ThemeId : int { Midnight = 0, Daylight, Ocean, Sakura, Forest, Dracula, Nord, Monokai, Solarized, CyberPunk, Sunset, Arctic, Lavender, Ember, Abyss, Count };

    struct ThemeColors {
        ImVec4 bgMain, bgSidebar, bgChannelList, bgChat, bgFooter, bgPopup, bgAvatar;
        ImVec4 bgCard;
        ImVec4 textPrimary, textSecondary, textMuted;
        ImVec4 accent, accentHover, accentDim;
        ImVec4 speakingGreen, speakingGlow;
        ImVec4 error;
        ImVec4 buttonSubtle, buttonSubtleHover;
        ImVec4 buttonDanger, buttonDangerHover;
        ImVec4 separator;
        ImVec4 textOnAccent;
        ImVec4 frameBg, frameBgHover, frameBgActive;
        ImVec4 scrollGrab, scrollGrabHover, scrollGrabActive;
    };

    struct Styles {
        // ---- Layout: channel sidebar left, server rail right ----
        static constexpr float SidebarWidth          = 240.0f;
        static constexpr float ServerRailWidth       = 64.0f;
        static constexpr float MainContentLeftOffset = 240.0f;
        static constexpr float FooterHeight          = 150.0f;
        static constexpr float SectionPadding        = 14.0f;
        static constexpr float ItemPadding           = 6.0f;
        static constexpr float AddButtonSize         = 28.0f;
        static constexpr float AddButtonMargin       = 12.0f;
        static constexpr float PopupPadding          = 18.0f;
        static constexpr float Rounding              = 12.0f;

        // Voice grid
        static constexpr float AvatarRadius          = 36.0f;
        static constexpr float VoiceItemWidth        = 148.0f;
        static constexpr float VoiceItemHeight       = 176.0f;
        static constexpr float VoiceItemSpacing      = 18.0f;
        static constexpr float VoiceAvatarFontSize   = 22.0f;
        static constexpr float VoiceCardRounding     = 14.0f;
        static constexpr float SpeakingRingRadius    = 4.0f;

        // Login card
        static constexpr float LoginCardWidth        = 380.0f;
        static constexpr float LoginCardRounding     = 18.0f;

        // ---- Active theme ----
        static ThemeColors C;
        static ThemeId     ActiveTheme;

        // Color accessors
        static inline ImVec4 BgMain()           { return C.bgMain; }
        static inline ImVec4 BgSidebar()        { return C.bgSidebar; }
        static inline ImVec4 BgChannelList()    { return C.bgChannelList; }
        static inline ImVec4 BgChat()           { return C.bgChat; }
        static inline ImVec4 BgFooter()         { return C.bgFooter; }
        static inline ImVec4 BgPopup()          { return C.bgPopup; }
        static inline ImVec4 BgAvatar()         { return C.bgAvatar; }
        static inline ImVec4 BgCard()           { return C.bgCard; }
        static inline ImVec4 TextPrimary()      { return C.textPrimary; }
        static inline ImVec4 TextSecondary()    { return C.textSecondary; }
        static inline ImVec4 TextMuted()        { return C.textMuted; }
        static inline ImVec4 Accent()           { return C.accent; }
        static inline ImVec4 AccentHover()      { return C.accentHover; }
        static inline ImVec4 AccentDim()        { return C.accentDim; }
        static inline ImVec4 SpeakingGreen()    { return C.speakingGreen; }
        static inline ImVec4 SpeakingGlow()     { return C.speakingGlow; }
        static inline ImVec4 Error()            { return C.error; }
        static inline ImVec4 ButtonSubtle()     { return C.buttonSubtle; }
        static inline ImVec4 ButtonSubtleHover(){ return C.buttonSubtleHover; }
        static inline ImVec4 ButtonDanger()     { return C.buttonDanger; }
        static inline ImVec4 ButtonDangerHover(){ return C.buttonDangerHover; }
        static inline ImVec4 Separator()        { return C.separator; }
        static inline ImVec4 TextOnAccent()     { return C.textOnAccent; }

        // ImU32 helpers
        static inline ImU32 ColBgMain()             { return ImGui::ColorConvertFloat4ToU32(C.bgMain); }
        static inline ImU32 ColBgSidebar()          { return ImGui::ColorConvertFloat4ToU32(C.bgSidebar); }
        static inline ImU32 ColBgChannelList()      { return ImGui::ColorConvertFloat4ToU32(C.bgChannelList); }
        static inline ImU32 ColBgChat()             { return ImGui::ColorConvertFloat4ToU32(C.bgChat); }
        static inline ImU32 ColBgAvatar()           { return ImGui::ColorConvertFloat4ToU32(C.bgAvatar); }
        static inline ImU32 ColBgCard()             { return ImGui::ColorConvertFloat4ToU32(C.bgCard); }
        static inline ImU32 ColTextOnAvatar()       { return ImGui::ColorConvertFloat4ToU32(C.textPrimary); }
        static inline ImU32 ColTextName()           { return ImGui::ColorConvertFloat4ToU32(C.textPrimary); }
        static inline ImU32 ColSpeakingRing()       { return ImGui::ColorConvertFloat4ToU32(C.speakingGreen); }
        static inline ImU32 ColSpeakingGlow()       { return ImGui::ColorConvertFloat4ToU32(C.speakingGlow); }
        static inline ImU32 ColSelectedIndicator()  { return ImGui::ColorConvertFloat4ToU32(C.accent); }
        static inline ImU32 ColMutedText()          { return ImGui::ColorConvertFloat4ToU32(C.error); }
        static inline ImU32 ColMutedLabel()         { return IM_COL32((int)(C.error.x*255),(int)(C.error.y*255),(int)(C.error.z*255),180); }

        // Theme management
        static ThemeColors  BuildTheme(ThemeId id);
        static const char*  GetThemeName(ThemeId id);
        static void         SetTheme(ThemeId id);
        static void         Apply();
    };
}
