#include "ChatView.h"
#include "../Components.h"
#include "../Theme.h"
#include "../Styles.h"
#include "../TextureManager.h"
#include "../../shared/PacketHandler.h"
#include "../../network/ImageCache.h"
#include <string>
#include <algorithm>
#include <shellapi.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#undef min
#undef max

namespace TalkMe::UI::Views {

    void RenderChannelView(NetworkClient& netClient, UserSession& currentUser, const Server& currentServer,
        std::vector<ChatMessage>& messages, int& selectedChannelId, int& activeVoiceChannelId,
        std::vector<std::string>& voiceMembers, std::map<std::string, float>& speakingTimers,
        std::map<std::string, float>& userVolumes, std::function<void(const std::string&, float)> setUserVolume,
        char* chatInputBuf, bool selfMuted, bool selfDeafened,
        const std::map<std::string, UserVoiceState>* userMuteStates,
        const std::map<std::string, float>* typingUsers,
        std::function<void()> onUserTyping,
        int* replyingToMessageId,
        const std::vector<std::pair<std::string, bool>>* serverMembers,
        bool* showMemberList,
        char* searchBuf,
        bool* showSearch,
        std::function<void(int fps, int quality)> onStartScreenShare,
        std::function<void()> onStopScreenShare,
        bool isScreenSharing,
        bool someoneIsSharing,
        void* screenShareTexture,
        int screenShareW,
        int screenShareH,
        const std::vector<std::string>* activeStreamers,
        std::string* viewingStream,
        bool* streamMaximized,
        bool* showGifPicker)
    {
        float winH = ImGui::GetWindowHeight();
        float winW = ImGui::GetWindowWidth();
        float left = Styles::MainContentLeftOffset;
        float areaW = winW - left - Styles::ServerRailWidth;

        ImGui::SetCursorPos(ImVec2(left, 0));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, Styles::BgChat());
        ImGui::BeginChild("ChatArea", ImVec2(areaW, winH), false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

        if (selectedChannelId != -1) {
            std::string chName = "Unknown";
            std::string chDesc;
            ChannelType cType = ChannelType::Text;
            for (const auto& ch : currentServer.channels)
                if (ch.id == selectedChannelId) { chName = ch.name; cType = ch.type; chDesc = ch.description; break; }

            // ==================== VOICE VIEW ====================
            if (cType == ChannelType::Voice) {

                ImGui::Dummy(ImVec2(0, 28));
                ImGui::Indent(40);
                ImGui::PushStyleColor(ImGuiCol_Text, Styles::Accent());
                ImGui::SetWindowFontScale(1.4f);
                ImGui::Text("%s", chName.c_str());
                ImGui::SetWindowFontScale(1.0f);
                ImGui::PopStyleColor();

                ImGui::PushStyleColor(ImGuiCol_Text, Styles::TextMuted());
                ImGui::Text("%d connected", (int)voiceMembers.size());
                ImGui::PopStyleColor();
                ImGui::Unindent(40);

                ImGui::Dummy(ImVec2(0, 12));
                ImGui::PushStyleColor(ImGuiCol_Separator, Styles::Separator());
                ImGui::Separator();
                ImGui::PopStyleColor();
                ImGui::Dummy(ImVec2(0, 16));

                float leaveBarH = 120.0f;
                float gridTop = ImGui::GetCursorPosY();
                float gridH = winH - gridTop - leaveBarH;
                if (gridH < 100.0f) gridH = 100.0f;

                // When screen sharing: split into screen viewport (top) + user strip (bottom)
                bool showingScreenShare = (someoneIsSharing || isScreenSharing);
                float screenViewH = showingScreenShare ? (gridH * 0.7f) : 0.0f;
                float userStripH = showingScreenShare ? (gridH - screenViewH) : gridH;

                if (showingScreenShare) {
                    // Stream switcher tabs (when multiple active streams)
                    if (activeStreamers && activeStreamers->size() > 1 && viewingStream) {
                        for (size_t i = 0; i < activeStreamers->size(); i++) {
                            const auto& streamer = (*activeStreamers)[i];
                            std::string disp = streamer;
                            size_t hp = disp.find('#');
                            if (hp != std::string::npos) disp = disp.substr(0, hp);
                            bool selected = (*viewingStream == streamer);
                            if (i > 0) ImGui::SameLine();
                            if (selected) ImGui::PushStyleColor(ImGuiCol_Button, Styles::Accent());
                            if (ImGui::SmallButton((disp + "##stream").c_str()))
                                *viewingStream = streamer;
                            if (selected) ImGui::PopStyleColor();
                        }
                    }

                    // Maximize/minimize button
                    if (streamMaximized) {
                        float maxBtnX = areaW - 90;
                        ImGui::SameLine(maxBtnX);
                        if (ImGui::SmallButton(*streamMaximized ? "Minimize" : "Maximize"))
                            *streamMaximized = !*streamMaximized;
                    }

                    float actualViewH = (streamMaximized && *streamMaximized) ? (gridH + userStripH) : screenViewH;
                    ImGui::BeginChild("ScreenViewport", ImVec2(areaW, actualViewH), false);
                    if (screenShareTexture && screenShareW > 0 && screenShareH > 0) {
                        float viewW = areaW - 20.0f;
                        float viewH = screenViewH - 10.0f;
                        float aspect = (float)screenShareW / (float)screenShareH;
                        float fitW = viewW;
                        float fitH = fitW / aspect;
                        if (fitH > viewH) { fitH = viewH; fitW = fitH * aspect; }
                        float padX = (areaW - fitW) * 0.5f;
                        ImGui::SetCursorPos(ImVec2(padX, 5.0f));
                        ImGui::Image((ImTextureID)screenShareTexture, ImVec2(fitW, fitH));
                    } else {
                        ImGui::Dummy(ImVec2(0, screenViewH * 0.35f));
                        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.08f, 0.08f, 0.1f, 1.0f));
                        float placeholderW = areaW * 0.6f;
                        float placeholderH = screenViewH * 0.4f;
                        ImGui::SetCursorPosX((areaW - placeholderW) * 0.5f);
                        ImGui::BeginChild("##placeholder", ImVec2(placeholderW, placeholderH), true);
                        ImGui::Dummy(ImVec2(0, placeholderH * 0.3f));
                        std::string sharerName = isScreenSharing ? "You" : "Someone";
                        std::string msg = sharerName + " is sharing their screen...";
                        ImVec2 sz = ImGui::CalcTextSize(msg.c_str());
                        ImGui::SetCursorPosX((placeholderW - sz.x) * 0.5f);
                        ImGui::TextColored(ImVec4(0.5f, 0.7f, 1.0f, 1.0f), "%s", msg.c_str());
                        ImGui::SetCursorPosX((placeholderW - 140) * 0.5f);
                        ImGui::TextDisabled("Waiting for video stream...");
                        ImGui::EndChild();
                        ImGui::PopStyleColor();
                    }
                    ImGui::EndChild();
                }

                // User strip (hidden when stream maximized)
                bool hideUsers = (streamMaximized && *streamMaximized && showingScreenShare);
                float actualUserH = hideUsers ? 0.0f : userStripH;
                if (!hideUsers) {
                ImGui::BeginChild("VoiceGrid", ImVec2(areaW, actualUserH), false, ImGuiWindowFlags_None);
                {

                float avatarR = Styles::AvatarRadius;
                float itemW   = Styles::VoiceItemWidth;
                float itemH   = Styles::VoiceItemHeight;
                float gap     = Styles::VoiceItemSpacing;
                float cardR   = Styles::VoiceCardRounding;

                float usableW = areaW - 60.0f;
                int cols = (std::max)(1, (int)(usableW / (itemW + gap)));
                float totalW = cols * itemW + (cols - 1) * gap;
                float padX = (areaW - totalW) * 0.5f;
                if (padX < 20.0f) padX = 20.0f;

                static std::string s_volPopupMember;
                bool wantPopup = false;
                int col = 0;

                for (size_t mi = 0; mi < voiceMembers.size(); mi++) {
                    const auto& member = voiceMembers[mi];

                    if (col == 0) {
                        ImGui::Dummy(ImVec2(padX - 10.0f, 0));
                        ImGui::SameLine();
                    }

                    ImGui::PushID((int)mi);
                    ImGui::BeginGroup();

                    ImVec2 pos = ImGui::GetCursorScreenPos();
                    ImGui::InvisibleButton("##card", ImVec2(itemW, itemH));
                    if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(1) && setUserVolume) {
                        s_volPopupMember = member;
                        wantPopup = true;
                    }

                    ImDrawList* dl = ImGui::GetWindowDrawList();
                    bool speaking = speakingTimers.count(member) && ((float)ImGui::GetTime() - speakingTimers[member] < 0.5f);
                    bool muted = userVolumes.count(member) && userVolumes[member] <= 0.0f;

                    // Card background
                    ImVec2 cardEnd = ImVec2(pos.x + itemW, pos.y + itemH);
                    if (speaking) {
                        // Glow behind card
                        dl->AddRectFilled(
                            ImVec2(pos.x - 3, pos.y - 3),
                            ImVec2(cardEnd.x + 3, cardEnd.y + 3),
                            Styles::ColSpeakingGlow(), cardR + 3);
                    }
                    dl->AddRectFilled(pos, cardEnd, Styles::ColBgCard(), cardR);

                    if (speaking)
                        dl->AddRect(pos, cardEnd, Styles::ColSpeakingRing(), cardR, 0, 2.0f);

                    // Avatar circle centered in card
                    ImVec2 ctr = ImVec2(pos.x + itemW * 0.5f, pos.y + 12.0f + avatarR);
                    dl->AddCircleFilled(ctr, avatarR, Styles::ColBgAvatar());

                    // Initials
                    std::string init = member.substr(0, (std::min)((size_t)2, member.size()));
                    ImVec2 tsz = ImGui::GetFont()->CalcTextSizeA(Styles::VoiceAvatarFontSize, FLT_MAX, 0, init.c_str());
                    dl->AddText(ImGui::GetFont(), Styles::VoiceAvatarFontSize,
                        ImVec2(ctr.x - tsz.x * 0.5f, ctr.y - tsz.y * 0.5f),
                        Styles::ColTextOnAvatar(), init.c_str());

                    // Name
                    std::string disp = member;
                    size_t hp = member.find('#');
                    if (hp != std::string::npos) disp = member.substr(0, hp);
                    if (disp.size() > 12) disp = disp.substr(0, 11) + "..";

                    ImVec2 nsz = ImGui::CalcTextSize(disp.c_str());
                    float nameY = pos.y + avatarR * 2.0f + 22.0f;
                    ImU32 nCol = speaking ? Styles::ColSpeakingRing() :
                                 (muted ? Styles::ColMutedText() : Styles::ColTextName());
                    dl->AddText(ImVec2(pos.x + (itemW - nsz.x) * 0.5f, nameY), nCol, disp.c_str());

                    // Status icons below name
                    bool isMe = (member == currentUser.username);
                    bool micOff = false;
                    bool spkOff = false;
                    if (isMe) {
                        micOff = selfMuted;
                        spkOff = selfDeafened;
                    } else if (userMuteStates) {
                        auto sit = userMuteStates->find(member);
                        if (sit != userMuteStates->end()) {
                            micOff = sit->second.muted;
                            spkOff = sit->second.deafened;
                        }
                    }

                    if (muted || micOff || spkOff) {
                        float iconY = nameY + 18.0f;
                        std::string status;
                        if (muted && !isMe) {
                            status = "MUTED";
                        } else {
                            if (micOff) status += "MIC X";
                            if (spkOff) { if (!status.empty()) status += "  "; status += "SPK X"; }
                        }
                        if (!status.empty()) {
                            ImVec2 ms = ImGui::CalcTextSize(status.c_str());
                            dl->AddText(ImVec2(pos.x + (itemW - ms.x) * 0.5f, iconY), Styles::ColMutedLabel(), status.c_str());
                        }
                    }

                    ImGui::EndGroup();
                    ImGui::PopID();

                    col++;
                    if (col < cols) {
                        ImGui::SameLine(0, gap);
                    } else {
                        col = 0;
                    }
                }

                // Volume popup
                if (wantPopup)
                    ImGui::OpenPopup("UserVolumePopup");

                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Styles::PopupPadding, Styles::PopupPadding));
                ImGui::PushStyleColor(ImGuiCol_PopupBg, Styles::BgPopup());
                if (ImGui::BeginPopup("UserVolumePopup")) {
                    if (!s_volPopupMember.empty() && setUserVolume) {
                        const std::string& m = s_volPopupMember;
                        std::string d = m;
                        size_t hp2 = m.find('#');
                        if (hp2 != std::string::npos) d = m.substr(0, hp2);

                        ImGui::Text("%s", d.c_str());
                        ImGui::Separator();
                        ImGui::Dummy(ImVec2(0, 4));

                        bool isSelf = (m == currentUser.username);
                        if (!isSelf) {
                            float vol = userVolumes.count(m) ? userVolumes[m] : 1.0f;
                            if (vol <= 0.0f) {
                                if (UI::AccentButton("Unmute", ImVec2(220, 30))) {
                                    setUserVolume(m, 1.0f);
                                    ImGui::CloseCurrentPopup();
                                }
                            } else {
                                ImGui::PushStyleColor(ImGuiCol_Button, Styles::ButtonDanger());
                                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Styles::ButtonDangerHover());
                                if (ImGui::Button("Mute", ImVec2(220, 30))) {
                                    setUserVolume(m, 0.0f);
                                    ImGui::CloseCurrentPopup();
                                }
                                ImGui::PopStyleColor(2);
                            }

                            ImGui::Dummy(ImVec2(0, 6));
                            float pct = vol * 100.0f;
                            ImGui::SetNextItemWidth(220);
                            if (ImGui::SliderFloat("##Vol", &pct, 0, 200, "%.0f%%"))
                                setUserVolume(m, pct / 100.0f);
                        } else {
                            ImGui::PushStyleColor(ImGuiCol_Text, Styles::TextMuted());
                            ImGui::TextDisabled("Use MIC / SPK in the sidebar to mute or deafen yourself.");
                            ImGui::PopStyleColor();
                        }
                    }
                    ImGui::EndPopup();
                }
                ImGui::PopStyleColor();
                ImGui::PopStyleVar();

                } // close user grid scope
                ImGui::EndChild(); // VoiceGrid
                } // close !hideUsers

                // ====== Bottom action bar: 3 buttons centered ======
                ImGui::Dummy(ImVec2(0, 8));
                float btnBarW = 380.0f;
                float btnBarX = (areaW - btnBarW) * 0.5f;
                if (btnBarX < 10.0f) btnBarX = 10.0f;
                ImGui::SetCursorPosX(btnBarX);

                float actionBtnH = 38.0f;
                ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f);

                // Button 1: Leave (red)
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.7f, 0.12f, 0.12f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.85f, 0.18f, 0.18f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
                if (ImGui::Button("Leave", ImVec2(100, actionBtnH))) {
                    activeVoiceChannelId = -1;
                    netClient.Send(PacketType::Join_Voice_Channel, PacketHandler::JoinVoiceChannelPayload(-1));
                }
                ImGui::PopStyleColor(3);

                // Button 2: Screen Share (blue) or Stop (if sharing)
                ImGui::SameLine(0, 12);
                if (isScreenSharing) {
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.6f, 0.3f, 0.1f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.7f, 0.35f, 0.12f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
                    if (ImGui::Button("Stop Share", ImVec2(130, actionBtnH))) {
                        if (onStopScreenShare) onStopScreenShare();
                    }
                    ImGui::PopStyleColor(3);
                } else {
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.35f, 0.65f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.22f, 0.42f, 0.75f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
                    if (ImGui::Button("Screen Share", ImVec2(130, actionBtnH))) {
                        ImGui::OpenPopup("ScreenShareSetup");
                    }
                    ImGui::PopStyleColor(3);
                }

                // Button 3: Games (green)
                ImGui::SameLine(0, 12);
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.55f, 0.25f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.2f, 0.65f, 0.3f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
                if (ImGui::Button("Games", ImVec2(100, actionBtnH))) {
                    ImGui::OpenPopup("GamesPicker");
                }
                ImGui::PopStyleColor(3);

                ImGui::PopStyleVar();

                // ====== Screen Share Setup Popup ======
                ImGui::SetNextWindowSize(ImVec2(520, 140), ImGuiCond_Always);
                if (ImGui::BeginPopup("ScreenShareSetup")) {
                    ImGui::Text("Screen Share Settings");
                    ImGui::Separator();
                    ImGui::Dummy(ImVec2(0, 4));

                    ImGui::Columns(3, nullptr, false);

                    static int s_shareMode = 0;
                    ImGui::Text("Source:");
                    ImGui::RadioButton("Full Screen", &s_shareMode, 0);
                    ImGui::RadioButton("App Window", &s_shareMode, 1);

                    ImGui::NextColumn();
                    static int s_shareFps = 1;
                    ImGui::Text("FPS:");
                    ImGui::RadioButton("30", &s_shareFps, 0);
                    ImGui::RadioButton("60", &s_shareFps, 1);
                    ImGui::RadioButton("120", &s_shareFps, 2);

                    ImGui::NextColumn();
                    static int s_shareQuality = 1;
                    ImGui::Text("Quality:");
                    ImGui::RadioButton("Low##q", &s_shareQuality, 0);
                    ImGui::RadioButton("Medium##q", &s_shareQuality, 1);
                    ImGui::RadioButton("High##q", &s_shareQuality, 2);

                    ImGui::Columns(1);
                    ImGui::Dummy(ImVec2(0, 4));

                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.35f, 0.65f, 1.0f));
                    if (ImGui::Button("Start Sharing", ImVec2(150, 32))) {
                        int fps[] = { 30, 60, 120 };
                        int quality[] = { 40, 70, 95 };
                        if (onStartScreenShare) onStartScreenShare(fps[s_shareFps], quality[s_shareQuality]);
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::PopStyleColor();
                    ImGui::EndPopup();
                }

                // ====== Games Picker Popup ======
                ImGui::SetNextWindowSize(ImVec2(260, 200), ImGuiCond_Always);
                if (ImGui::BeginPopup("GamesPicker")) {
                    ImGui::Text("Choose a Game");
                    ImGui::Separator();
                    ImGui::Dummy(ImVec2(0, 6));

                    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);

                    if (ImGui::Button("Chess", ImVec2(230, 34))) {
                        ImGui::OpenPopup("ChessTarget");
                    }
                    ImGui::Dummy(ImVec2(0, 2));
                    if (ImGui::Button("Car Racing", ImVec2(230, 34))) {
                        ImGui::OpenPopup("RacingTarget");
                    }

                    ImGui::PopStyleVar();

                    // Chess opponent picker (nested popup)
                    if (ImGui::BeginPopup("ChessTarget")) {
                        ImGui::Text("Challenge to Chess:");
                        for (const auto& m : voiceMembers) {
                            if (m == currentUser.username) continue;
                            std::string d = m; size_t h = d.find('#'); if (h != std::string::npos) d = d.substr(0, h);
                            if (ImGui::Selectable(d.c_str())) {
                                nlohmann::json cj; cj["to"] = m; cj["game"] = "chess";
                                netClient.Send(PacketType::Game_Challenge, cj.dump());
                            }
                        }
                        if (voiceMembers.size() <= 1) ImGui::TextDisabled("No other users in channel");
                        ImGui::EndPopup();
                    }

                    // Racing opponent picker (nested popup)
                    if (ImGui::BeginPopup("RacingTarget")) {
                        ImGui::Text("Challenge to Race:");
                        for (const auto& m : voiceMembers) {
                            if (m == currentUser.username) continue;
                            std::string d = m; size_t h = d.find('#'); if (h != std::string::npos) d = d.substr(0, h);
                            if (ImGui::Selectable(d.c_str())) {
                                nlohmann::json cj; cj["to"] = m; cj["game"] = "racing";
                                netClient.Send(PacketType::Game_Challenge, cj.dump());
                            }
                        }
                        if (voiceMembers.size() <= 1) ImGui::TextDisabled("No other users in channel");
                        ImGui::EndPopup();
                    }

                    ImGui::EndPopup();
                }
            }
            // ==================== TEXT VIEW ====================
            else {
                ImGui::Dummy(ImVec2(0, 20));
                ImGui::Indent(36);
                ImGui::PushStyleColor(ImGuiCol_Text, Styles::TextSecondary());
                ImGui::SetWindowFontScale(1.25f);
                ImGui::Text("# %s", chName.c_str());
                ImGui::SetWindowFontScale(1.0f);
                ImGui::PopStyleColor();

                if (!chDesc.empty()) {
                    ImGui::PushStyleColor(ImGuiCol_Text, Styles::TextMuted());
                    ImGui::TextWrapped("%s", chDesc.c_str());
                    ImGui::PopStyleColor();
                }

                ImGui::PushStyleColor(ImGuiCol_Text, Styles::TextMuted());
                ImGui::Text("Invite: %s", currentServer.inviteCode.c_str());
                ImGui::PopStyleColor();

                ImGui::Unindent(36);

                ImGui::Indent(36);
                if (showSearch) {
                    if (ImGui::SmallButton(*showSearch ? "[X] Search" : "Search"))
                        *showSearch = !*showSearch;
                    ImGui::SameLine();
                }
                if (showMemberList) {
                    if (ImGui::SmallButton(*showMemberList ? "[X] Members" : "Members"))
                        *showMemberList = !*showMemberList;
                    ImGui::SameLine();
                }
                ImGui::SmallButton("Copy Invite");
                if (ImGui::IsItemClicked()) ImGui::SetClipboardText(currentServer.inviteCode.c_str());
                ImGui::Unindent(36);

                ImGui::Dummy(ImVec2(0, 6));
                ImGui::PushStyleColor(ImGuiCol_Separator, Styles::Separator());
                ImGui::Separator();
                ImGui::PopStyleColor();
                ImGui::Dummy(ImVec2(0, 4));

                float hdrH = ImGui::GetCursorPosY();
                float inpH = 120.0f;
                float msgH = winH - hdrH - inpH;
                if (msgH < 80.0f) msgH = 80.0f;

                if (showSearch && *showSearch && searchBuf) {
                    ImGui::Indent(32);
                    ImGui::PushItemWidth(areaW - 100);
                    ImGui::InputTextWithHint("##search", "Search messages...", searchBuf, 256);
                    ImGui::PopItemWidth();
                    ImGui::Unindent(32);
                }

                std::string searchStr = (showSearch && *showSearch && searchBuf) ? searchBuf : "";
                for (auto& c : searchStr) c = (char)std::tolower((unsigned char)c);

                ImGui::SetCursorPosX(32);
                ImGui::BeginChild("Messages", ImVec2(areaW - 64, msgH), false, ImGuiWindowFlags_None);
                for (const auto& msg : messages) {
                    if (msg.channelId != selectedChannelId) continue;
                    if (!searchStr.empty()) {
                        std::string lower = msg.content;
                        for (auto& c : lower) c = (char)std::tolower((unsigned char)c);
                        std::string lowerSender = msg.sender;
                        for (auto& c : lowerSender) c = (char)std::tolower((unsigned char)c);
                        if (lower.find(searchStr) == std::string::npos && lowerSender.find(searchStr) == std::string::npos)
                            continue;
                    }
                    bool isMe = (msg.sender == currentUser.username);

                    ImGui::BeginGroup();

                    if (msg.replyToId > 0) {
                        for (const auto& orig : messages) {
                            if (orig.id == msg.replyToId) {
                                std::string replySender = orig.sender;
                                size_t rhp = replySender.find('#');
                                if (rhp != std::string::npos) replySender = replySender.substr(0, rhp);
                                std::string preview = orig.content.substr(0, 60);
                                if (orig.content.size() > 60) preview += "...";
                                ImGui::PushStyleColor(ImGuiCol_Text, Styles::TextMuted());
                                ImGui::Text("  | %s: %s", replySender.c_str(), preview.c_str());
                                ImGui::PopStyleColor();
                                break;
                            }
                        }
                    }

                    ImGui::PushStyleColor(ImGuiCol_Text, isMe ? Styles::Accent() : Styles::Error());
                    size_t hp = msg.sender.find('#');
                    std::string dispName = hp != std::string::npos ? msg.sender.substr(0, hp) : msg.sender;
                    std::string tag = hp != std::string::npos ? msg.sender.substr(hp) : "";
                    ImGui::Text("%s", dispName.c_str());
                    if (!tag.empty()) {
                        ImGui::SameLine(0, 0);
                        ImGui::PushStyleColor(ImGuiCol_Text, Styles::TextMuted());
                        ImGui::Text("%s", tag.c_str());
                        ImGui::PopStyleColor();
                    }
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Click to copy username");
                    if (ImGui::IsItemClicked()) ImGui::SetClipboardText(msg.sender.c_str());
                    ImGui::PopStyleColor();

                    ImGui::SameLine();
                    ImGui::TextDisabled("%s", msg.timestamp.c_str());
                    if (msg.pinned) {
                        ImGui::SameLine();
                        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "[pinned]");
                    }

                    {
                        const std::string& text = msg.content;
                        size_t pos = 0;
                        bool hasUrl = false;
                        while (pos < text.size()) {
                            size_t urlStart = std::string::npos;
                            for (const char* prefix : {"https://", "http://"}) {
                                size_t f = text.find(prefix, pos);
                                if (f != std::string::npos && (urlStart == std::string::npos || f < urlStart))
                                    urlStart = f;
                            }
                            if (urlStart == std::string::npos) {
                                std::string rest = text.substr(pos);
                                if (!rest.empty()) {
                                    ImGui::PushStyleColor(ImGuiCol_Text, Styles::TextPrimary());
                                    ImGui::TextWrapped("%s", rest.c_str());
                                    ImGui::PopStyleColor();
                                }
                                break;
                            }
                            if (urlStart > pos) {
                                std::string before = text.substr(pos, urlStart - pos);
                                ImGui::PushStyleColor(ImGuiCol_Text, Styles::TextPrimary());
                                ImGui::TextWrapped("%s", before.c_str());
                                ImGui::PopStyleColor();
                            }
                            size_t urlEnd = text.find_first_of(" \t\n\r", urlStart);
                            if (urlEnd == std::string::npos) urlEnd = text.size();
                            std::string url = text.substr(urlStart, urlEnd - urlStart);

                            bool isImage = false;
                            std::string lower = url;
                            for (auto& c : lower) c = (char)std::tolower((unsigned char)c);
                            for (const char* ext : {".png", ".jpg", ".jpeg", ".gif", ".webp", ".bmp"})
                                if (lower.size() > strlen(ext) && lower.compare(lower.size() - strlen(ext), strlen(ext), ext) == 0)
                                    isImage = true;

                            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.3f, 0.6f, 1.0f, 1.0f));
                            ImGui::TextWrapped("%s", url.c_str());
                            ImGui::PopStyleColor();
                            if (ImGui::IsItemHovered()) {
                                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                                ImGui::SetTooltip("Click to open in browser");
                            }
                            if (ImGui::IsItemClicked())
                                ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);

                            // Inline image rendering for image URLs
                            if (isImage) {
                                auto& imgCache = TalkMe::ImageCache::Get();
                                auto* cached = imgCache.GetImage(url);
                                if (!cached && !imgCache.IsLoading(url))
                                    imgCache.RequestImage(url);

                                if (cached && cached->ready && cached->width > 0) {
                                    auto& tm = TalkMe::TextureManager::Get();
                                    std::string texId = "img_" + url;
                                    auto* srv = tm.GetTexture(texId);
                                    if (!srv)
                                        srv = tm.LoadFromRGBA(texId, cached->data.data(), cached->width, cached->height);
                                    if (srv) {
                                        float maxW = areaW * 0.5f;
                                        float maxH = 300.0f;
                                        float imgW = (float)cached->width;
                                        float imgH = (float)cached->height;
                                        if (imgW > maxW) { imgH *= maxW / imgW; imgW = maxW; }
                                        if (imgH > maxH) { imgW *= maxH / imgH; imgH = maxH; }
                                        ImGui::Image((ImTextureID)srv, ImVec2(imgW, imgH));
                                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Click to open full size");
                                        if (ImGui::IsItemClicked())
                                            ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                                    }
                                } else if (cached && cached->failed) {
                                    ImGui::PushStyleColor(ImGuiCol_Text, Styles::TextMuted());
                                    ImGui::Text("[Image failed to load]");
                                    ImGui::PopStyleColor();
                                } else {
                                    ImGui::PushStyleColor(ImGuiCol_Text, Styles::TextMuted());
                                    ImGui::Text("[Loading image...]");
                                    ImGui::PopStyleColor();
                                }
                            }

                            // YouTube link preview
                            bool isYouTube = (lower.find("youtube.com/watch") != std::string::npos ||
                                              lower.find("youtu.be/") != std::string::npos);
                            if (isYouTube && !isImage) {
                                // Extract video ID and show thumbnail
                                std::string videoId;
                                size_t vPos = url.find("v=");
                                if (vPos != std::string::npos) {
                                    videoId = url.substr(vPos + 2);
                                    size_t ampPos = videoId.find('&');
                                    if (ampPos != std::string::npos) videoId = videoId.substr(0, ampPos);
                                } else {
                                    size_t slashPos = url.find("youtu.be/");
                                    if (slashPos != std::string::npos) {
                                        videoId = url.substr(slashPos + 9);
                                        size_t qPos = videoId.find('?');
                                        if (qPos != std::string::npos) videoId = videoId.substr(0, qPos);
                                    }
                                }
                                if (!videoId.empty()) {
                                    std::string thumbUrl = "https://img.youtube.com/vi/" + videoId + "/mqdefault.jpg";
                                    auto& imgCache = TalkMe::ImageCache::Get();
                                    auto* cached = imgCache.GetImage(thumbUrl);
                                    if (!cached && !imgCache.IsLoading(thumbUrl))
                                        imgCache.RequestImage(thumbUrl);

                                    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.12f, 0.12f, 0.15f, 1.0f));
                                    ImGui::BeginChild(("yt_" + videoId).c_str(), ImVec2(340, 200), true, ImGuiWindowFlags_NoScrollbar);

                                    if (cached && cached->ready && cached->width > 0) {
                                        auto& tm = TalkMe::TextureManager::Get();
                                        std::string texId = "yt_" + videoId;
                                        auto* srv = tm.GetTexture(texId);
                                        if (!srv) srv = tm.LoadFromRGBA(texId, cached->data.data(), cached->width, cached->height);
                                        if (srv) ImGui::Image((ImTextureID)srv, ImVec2(320, 180));
                                    } else {
                                        ImGui::Dummy(ImVec2(320, 160));
                                        ImGui::TextDisabled("Loading YouTube preview...");
                                    }

                                    ImGui::EndChild();
                                    ImGui::PopStyleColor();
                                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Click to watch on YouTube");
                                    if (ImGui::IsItemClicked())
                                        ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                                }
                            }
                            hasUrl = true;
                            pos = urlEnd;
                        }
                    }

                    if (!msg.reactions.empty()) {
                        for (const auto& [emoji, users] : msg.reactions) {
                            bool iReacted = std::find(users.begin(), users.end(), currentUser.username) != users.end();
                            if (iReacted) ImGui::PushStyleColor(ImGuiCol_Button, Styles::Accent());
                            char label[64];
                            snprintf(label, sizeof(label), "%s %d##r_%s_%d", emoji.c_str(), (int)users.size(), emoji.c_str(), msg.id);
                            if (ImGui::SmallButton(label)) {
                                nlohmann::json rj;
                                rj["mid"] = msg.id; rj["emoji"] = emoji; rj["cid"] = selectedChannelId;
                                netClient.Send(iReacted ? PacketType::Remove_Reaction : PacketType::Add_Reaction, rj.dump());
                            }
                            if (ImGui::IsItemHovered()) {
                                std::string tip;
                                for (size_t ui = 0; ui < users.size() && ui < 5; ui++) {
                                    if (ui > 0) tip += ", ";
                                    std::string d = users[ui];
                                    size_t hp2 = d.find('#');
                                    if (hp2 != std::string::npos) d = d.substr(0, hp2);
                                    tip += d;
                                }
                                if (users.size() > 5) tip += " + " + std::to_string(users.size() - 5) + " more";
                                ImGui::SetTooltip("%s", tip.c_str());
                            }
                            if (iReacted) ImGui::PopStyleColor();
                            ImGui::SameLine();
                        }
                        ImGui::NewLine();
                    }

                    ImGui::EndGroup();

                    if (msg.id > 0) {
                        static int s_editingMsgId = 0;
                        static char s_editBuf[1024] = "";

                        if (s_editingMsgId == msg.id) {
                            ImGui::PushItemWidth(areaW - 180);
                            bool submitted = ImGui::InputText(("##edit_" + std::to_string(msg.id)).c_str(),
                                s_editBuf, sizeof(s_editBuf), ImGuiInputTextFlags_EnterReturnsTrue);
                            ImGui::PopItemWidth();
                            ImGui::SameLine();
                            if (ImGui::SmallButton("Save") || submitted) {
                                if (strlen(s_editBuf) > 0) {
                                    nlohmann::json ej;
                                    ej["mid"] = msg.id; ej["cid"] = selectedChannelId; ej["msg"] = std::string(s_editBuf);
                                    netClient.Send(PacketType::Edit_Message_Request, ej.dump());
                                }
                                s_editingMsgId = 0;
                            }
                            ImGui::SameLine();
                            if (ImGui::SmallButton("Cancel")) s_editingMsgId = 0;
                        }

                        if (ImGui::BeginPopupContextItem(("msg_" + std::to_string(msg.id)).c_str())) {
                            if (replyingToMessageId && ImGui::Selectable("Reply"))
                                *replyingToMessageId = msg.id;

                            ImGui::Separator();
                            ImGui::Text("React:");
                            ImGui::SameLine();
                            const char* quickEmojis[] = { "+1", "<3", ":)", "eyes", "fire", "GG" };
                            for (int ei = 0; ei < 6; ei++) {
                                if (ei > 0) ImGui::SameLine();
                                char eid[32];
                                snprintf(eid, sizeof(eid), "%s##qr%d", quickEmojis[ei], ei);
                                if (ImGui::SmallButton(eid)) {
                                    nlohmann::json rj;
                                    rj["mid"] = msg.id; rj["emoji"] = quickEmojis[ei]; rj["cid"] = selectedChannelId;
                                    netClient.Send(PacketType::Add_Reaction, rj.dump());
                                    ImGui::CloseCurrentPopup();
                                }
                            }
                            ImGui::Separator();

                            if (ImGui::Selectable(msg.pinned ? "Unpin Message" : "Pin Message")) {
                                nlohmann::json pj;
                                pj["mid"] = msg.id; pj["cid"] = selectedChannelId; pj["pin"] = !msg.pinned;
                                netClient.Send(PacketType::Pin_Message_Request, pj.dump());
                            }
                            if (isMe && ImGui::Selectable("Edit Message")) {
                                s_editingMsgId = msg.id;
                                strncpy_s(s_editBuf, msg.content.c_str(), sizeof(s_editBuf) - 1);
                            }
                            if (isMe && ImGui::Selectable("Delete Message"))
                                netClient.Send(PacketType::Delete_Message_Request,
                                    PacketHandler::CreateDeleteMessagePayload(msg.id, selectedChannelId, currentUser.username));
                            ImGui::EndPopup();
                        }
                    }
                    ImGui::Dummy(ImVec2(0, 8));
                }
                if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) ImGui::SetScrollHereY(1.0f);
                ImGui::EndChild();

                // Typing indicator
                {
                    std::string typingText;
                    if (typingUsers) {
                        float now = (float)ImGui::GetTime();
                        std::vector<std::string> active;
                        for (const auto& [user, ts] : *typingUsers) {
                            if (now - ts < 4.0f) {
                                std::string disp = user;
                                size_t hp = user.find('#');
                                if (hp != std::string::npos) disp = user.substr(0, hp);
                                active.push_back(disp);
                            }
                        }
                        if (active.size() == 1) typingText = active[0] + " is typing...";
                        else if (active.size() == 2) typingText = active[0] + " and " + active[1] + " are typing...";
                        else if (active.size() > 2) typingText = std::to_string(active.size()) + " people are typing...";
                    }
                    ImGui::Indent(32);
                    if (!typingText.empty()) {
                        ImGui::PushStyleColor(ImGuiCol_Text, Styles::TextMuted());
                        ImGui::Text("%s", typingText.c_str());
                        ImGui::PopStyleColor();
                    } else {
                        ImGui::Dummy(ImVec2(0, ImGui::GetTextLineHeight()));
                    }
                    ImGui::Unindent(32);
                }

                // Reply bar (if replying)
                if (replyingToMessageId && *replyingToMessageId > 0) {
                    ImGui::Indent(32);
                    ImGui::PushStyleColor(ImGuiCol_Text, Styles::Accent());
                    for (const auto& orig : messages) {
                        if (orig.id == *replyingToMessageId) {
                            std::string rn = orig.sender;
                            size_t rhp = rn.find('#');
                            if (rhp != std::string::npos) rn = rn.substr(0, rhp);
                            ImGui::Text("Replying to %s", rn.c_str());
                            break;
                        }
                    }
                    ImGui::PopStyleColor();
                    ImGui::SameLine();
                    ImGui::PushStyleColor(ImGuiCol_Text, Styles::TextMuted());
                    if (ImGui::SmallButton("X")) *replyingToMessageId = 0;
                    ImGui::PopStyleColor();
                    ImGui::Unindent(32);
                }

                // Input bar
                ImGui::Indent(32);
                ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f);
                ImGui::PushStyleColor(ImGuiCol_FrameBg, Styles::ButtonSubtle());
                float inputW = areaW - 220;
                ImGui::PushItemWidth(inputW);
                bool enter = ImGui::InputText("##chat_in", chatInputBuf, 1024, ImGuiInputTextFlags_EnterReturnsTrue);
                ImGui::PopItemWidth();

                if (ImGui::IsItemActive() && strlen(chatInputBuf) > 0 && onUserTyping)
                    onUserTyping();

                ImGui::SameLine();
                if (ImGui::Button("+", ImVec2(28, 32))) {
                    ImGui::OpenPopup("AttachPopup");
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Attach file or image");
                if (ImGui::BeginPopup("AttachPopup")) {
                    if (ImGui::Selectable("Upload Image/Video...")) {
                        // Open Windows file dialog
                        char filePath[MAX_PATH] = {};
                        OPENFILENAMEA ofn = {};
                        ofn.lStructSize = sizeof(ofn);
                        ofn.lpstrFilter = "Images & Videos\0*.png;*.jpg;*.jpeg;*.gif;*.bmp;*.webp;*.mp4;*.webm\0All Files\0*.*\0";
                        ofn.lpstrFile = filePath;
                        ofn.nMaxFile = MAX_PATH;
                        ofn.Flags = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
                        if (GetOpenFileNameA(&ofn) && strlen(filePath) > 0) {
                            // Read file and send via File_Transfer_Request
                            FILE* fp = fopen(filePath, "rb");
                            if (fp) {
                                fseek(fp, 0, SEEK_END);
                                long sz = ftell(fp);
                                fseek(fp, 0, SEEK_SET);
                                if (sz > 0 && sz < 10 * 1024 * 1024) {
                                    std::string filename = filePath;
                                    size_t lastSlash = filename.find_last_of("\\/");
                                    if (lastSlash != std::string::npos) filename = filename.substr(lastSlash + 1);

                                    nlohmann::json req;
                                    req["filename"] = filename;
                                    req["size"] = sz;
                                    netClient.Send(PacketType::File_Transfer_Request, req.dump());

                                    // Read and send in chunks
                                    std::vector<uint8_t> buf(65536);
                                    while (!feof(fp)) {
                                        size_t read = fread(buf.data(), 1, buf.size(), fp);
                                        if (read > 0) {
                                            std::vector<uint8_t> chunk(buf.begin(), buf.begin() + read);
                                            netClient.SendRaw(PacketType::File_Transfer_Chunk, chunk);
                                        }
                                    }
                                    netClient.Send(PacketType::File_Transfer_Complete, "{}");
                                }
                                fclose(fp);
                            }
                        }
                    }
                    if (ImGui::Selectable("GIF") && showGifPicker)
                        *showGifPicker = true;
                    ImGui::EndPopup();
                }

                ImGui::SameLine();
                if (UI::AccentButton("Send", ImVec2(60, 32)) || enter) {
                    if (strlen(chatInputBuf) > 0) {
                        std::string input(chatInputBuf);
                        if (input.size() > 1 && input[0] == '/') {
                            size_t spacePos = input.find(' ');
                            std::string cmd = (spacePos != std::string::npos) ? input.substr(1, spacePos - 1) : input.substr(1);
                            std::string args = (spacePos != std::string::npos) ? input.substr(spacePos + 1) : "";
                            nlohmann::json cmdJ;
                            cmdJ["cid"] = selectedChannelId;
                            cmdJ["cmd"] = cmd;
                            cmdJ["args"] = args;
                            netClient.Send(PacketType::Bot_Command, cmdJ.dump());
                        } else {
                            nlohmann::json msgJ;
                            msgJ["cid"] = selectedChannelId;
                            msgJ["u"] = currentUser.username;
                            msgJ["msg"] = input;
                            if (replyingToMessageId && *replyingToMessageId > 0) {
                                msgJ["reply_to"] = *replyingToMessageId;
                                *replyingToMessageId = 0;
                            }
                            netClient.Send(PacketType::Message_Text, msgJ.dump());
                        }
                        memset(chatInputBuf, 0, 1024);
                        ImGui::SetKeyboardFocusHere(-1);
                    }
                }
                ImGui::PopStyleColor();
                ImGui::PopStyleVar();
                ImGui::Unindent(32);
            }
        } else {
            const char* t = "Select a channel to get started";
            ImVec2 sz = ImGui::CalcTextSize(t);
            ImGui::Dummy(ImVec2(0, winH * 0.5f - sz.y));
            float cx = (areaW - sz.x) * 0.5f;
            if (cx > 0) { ImGui::Dummy(ImVec2(cx, 0)); ImGui::SameLine(); }
            ImGui::TextDisabled("%s", t);
        }

        // Member list panel (overlaid on right side of chat area)
        if (showMemberList && *showMemberList && serverMembers && !serverMembers->empty()) {
            float panelW = 180.0f;
            float panelX = areaW - panelW;
            ImGui::SetCursorPos(ImVec2(panelX, 0));
            ImGui::PushStyleColor(ImGuiCol_ChildBg, Styles::BgSidebar());
            ImGui::BeginChild("MemberList", ImVec2(panelW, winH), true);

            ImGui::PushStyleColor(ImGuiCol_Text, Styles::TextMuted());
            int onlineCount = 0;
            for (const auto& [_, on] : *serverMembers) if (on) onlineCount++;
            ImGui::Text("MEMBERS - %d/%d", onlineCount, (int)serverMembers->size());
            ImGui::PopStyleColor();
            ImGui::Separator();
            ImGui::Dummy(ImVec2(0, 4));

            for (const auto& [name, online] : *serverMembers) {
                std::string disp = name;
                size_t hp = name.find('#');
                if (hp != std::string::npos) disp = name.substr(0, hp);

                ImU32 dotCol = online ? IM_COL32(80, 220, 100, 255) : IM_COL32(120, 120, 125, 255);
                ImVec2 pos = ImGui::GetCursorScreenPos();
                ImGui::GetWindowDrawList()->AddCircleFilled(
                    ImVec2(pos.x + 6, pos.y + 8), 4.0f, dotCol);
                ImGui::Dummy(ImVec2(16, 0));
                ImGui::SameLine();

                if (!online) ImGui::PushStyleColor(ImGuiCol_Text, Styles::TextMuted());
                ImGui::Text("%s", disp.c_str());
                if (!online) ImGui::PopStyleColor();
            }

            ImGui::EndChild();
            ImGui::PopStyleColor();
        }

        ImGui::EndChild();
        ImGui::PopStyleColor();
    }
}
