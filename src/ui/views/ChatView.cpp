#include "ChatView.h"
#include "../Components.h"
#include "../Theme.h"
#include "../Styles.h"
#include "../../shared/PacketHandler.h"
#include <string>
#include <algorithm>

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
        const std::map<std::string, UserVoiceState>* userMuteStates)
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
            ChannelType cType = ChannelType::Text;
            for (const auto& ch : currentServer.channels)
                if (ch.id == selectedChannelId) { chName = ch.name; cType = ch.type; break; }

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

                float leaveBarH = 80.0f;
                float gridTop = ImGui::GetCursorPosY();
                float gridH = winH - gridTop - leaveBarH;
                if (gridH < 100.0f) gridH = 100.0f;

                ImGui::BeginChild("VoiceGrid", ImVec2(areaW, gridH), false, ImGuiWindowFlags_None);

                float avatarR = Styles::AvatarRadius;
                float itemW   = Styles::VoiceItemWidth;
                float itemH   = Styles::VoiceItemHeight;
                float gap     = Styles::VoiceItemSpacing;
                float cardR   = Styles::VoiceCardRounding;

                float usableW = areaW - 60.0f;
                int cols = std::max(1, (int)(usableW / (itemW + gap)));
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
                    std::string init = member.substr(0, std::min((size_t)2, member.size()));
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

                ImGui::EndChild(); // VoiceGrid

                // Leave Voice Chat button (bottom bar)
                ImGui::Dummy(ImVec2(0, 8));
                float leaveBtnW = 220.0f;
                float leaveBtnH = 42.0f;
                float leaveBtnX = (areaW - leaveBtnW) * 0.5f;
                if (leaveBtnX < 20.0f) leaveBtnX = 20.0f;
                ImGui::SetCursorPosX(leaveBtnX);
                ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 10.0f);
                ImGui::PushStyleColor(ImGuiCol_Button, Styles::ButtonDanger());
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Styles::ButtonDangerHover());
                ImGui::PushStyleColor(ImGuiCol_Text, Styles::TextOnAccent());
                if (ImGui::Button("Leave Voice Chat", ImVec2(leaveBtnW, leaveBtnH))) {
                    activeVoiceChannelId = -1;
                    netClient.Send(PacketType::Join_Voice_Channel, PacketHandler::JoinVoiceChannelPayload(-1));
                }
                ImGui::PopStyleColor(3);
                ImGui::PopStyleVar();
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

                ImGui::PushStyleColor(ImGuiCol_Text, Styles::TextMuted());
                ImGui::Text("Invite: %s", currentServer.inviteCode.c_str());
                ImGui::PopStyleColor();
                ImGui::Unindent(36);

                ImGui::Dummy(ImVec2(0, 6));
                ImGui::PushStyleColor(ImGuiCol_Separator, Styles::Separator());
                ImGui::Separator();
                ImGui::PopStyleColor();
                ImGui::Dummy(ImVec2(0, 4));

                float hdrH = ImGui::GetCursorPosY();
                float inpH = 68.0f;
                float msgH = winH - hdrH - inpH;
                if (msgH < 50.0f) msgH = 50.0f;

                ImGui::SetCursorPosX(32);
                ImGui::BeginChild("Messages", ImVec2(areaW - 64, msgH), false, ImGuiWindowFlags_None);
                for (const auto& msg : messages) {
                    if (msg.channelId != selectedChannelId) continue;
                    bool isMe = (msg.sender == currentUser.username);

                    ImGui::BeginGroup();
                    ImGui::PushStyleColor(ImGuiCol_Text, isMe ? Styles::Accent() : Styles::Error());
                    size_t hp = msg.sender.find('#');
                    if (hp != std::string::npos) {
                        ImGui::Text("%s", msg.sender.substr(0, hp).c_str());
                        ImGui::SameLine(0, 0);
                        ImGui::PushStyleColor(ImGuiCol_Text, Styles::TextMuted());
                        ImGui::Text("%s", msg.sender.substr(hp).c_str());
                        ImGui::PopStyleColor();
                    } else {
                        ImGui::Text("%s", msg.sender.c_str());
                    }
                    ImGui::PopStyleColor();

                    ImGui::SameLine();
                    ImGui::TextDisabled("%s", msg.timestamp.c_str());

                    ImGui::PushStyleColor(ImGuiCol_Text, Styles::TextPrimary());
                    ImGui::TextWrapped("%s", msg.content.c_str());
                    ImGui::PopStyleColor();
                    ImGui::EndGroup();

                    if (isMe && msg.id > 0) {
                        if (ImGui::BeginPopupContextItem(("msg_" + std::to_string(msg.id)).c_str())) {
                            if (ImGui::Selectable("Delete Message"))
                                netClient.Send(PacketType::Delete_Message_Request,
                                    PacketHandler::CreateDeleteMessagePayload(msg.id, selectedChannelId, currentUser.username));
                            ImGui::EndPopup();
                        }
                    }
                    ImGui::Dummy(ImVec2(0, 8));
                }
                if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) ImGui::SetScrollHereY(1.0f);
                ImGui::EndChild();

                // Input bar
                ImGui::Dummy(ImVec2(0, 4));
                ImGui::Indent(32);
                ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f);
                ImGui::PushStyleColor(ImGuiCol_FrameBg, Styles::ButtonSubtle());
                float inputW = areaW - 150;
                ImGui::PushItemWidth(inputW);
                bool enter = ImGui::InputText("##chat_in", chatInputBuf, 1024, ImGuiInputTextFlags_EnterReturnsTrue);
                ImGui::PopItemWidth();
                ImGui::SameLine();
                if (UI::AccentButton("Send", ImVec2(68, 32)) || enter) {
                    if (strlen(chatInputBuf) > 0) {
                        netClient.Send(PacketType::Message_Text,
                            PacketHandler::CreateMessagePayload(selectedChannelId, currentUser.username, chatInputBuf));
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

        ImGui::EndChild();
        ImGui::PopStyleColor();
    }
}
