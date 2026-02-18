#include "SidebarView.h"
#include "../Components.h"
#include "../Styles.h"
#include "../../network/PacketHandler.h"
#include "../../core/ConfigManager.h"
#include <string>

namespace TalkMe::UI::Views {

    void RenderSidebar(NetworkClient& netClient, UserSession& currentUser, AppState& currentState,
        std::vector<Server>& servers, int& selectedServerId, int& selectedChannelId,
        int& activeVoiceChannelId, std::vector<std::string>& voiceMembers,
        char* newServerNameBuf, char* newChannelNameBuf, bool& showSettings,
        bool& selfMuted, bool& selfDeafened, const VoiceInfoData& voiceInfo)
    {
        float parentW = ImGui::GetWindowWidth();
        float windowHeight = ImGui::GetWindowHeight();
        float sW = Styles::SidebarWidth;
        float railW = Styles::ServerRailWidth;

        // ================================================================
        //  LEFT PANEL: Channel sidebar
        // ================================================================
        ImGui::SetCursorPos(ImVec2(0, 0));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, Styles::BgSidebar());
        ImGui::BeginChild("ChannelSidebar", ImVec2(sW, windowHeight), false, ImGuiWindowFlags_NoScrollbar);

        // Header
        ImGui::Dummy(ImVec2(0, 14));
        ImGui::Indent(18);
        if (selectedServerId != -1) {
            std::string sName = "Loading...";
            for (const auto& s : servers)
                if (s.id == selectedServerId) sName = s.name;
            ImGui::PushStyleColor(ImGuiCol_Text, Styles::Accent());
            ImGui::SetWindowFontScale(1.2f);
            ImGui::Text("%s", sName.c_str());
            ImGui::SetWindowFontScale(1.0f);
            ImGui::PopStyleColor();
        } else {
            ImGui::TextDisabled("Select a Server");
        }
        ImGui::Unindent(18);
        ImGui::Dummy(ImVec2(0, 8));
        ImGui::PushStyleColor(ImGuiCol_Separator, Styles::Separator());
        ImGui::Separator();
        ImGui::PopStyleColor();

        // Channel list
        if (selectedServerId != -1) {
            const Server* cur = nullptr;
            for (const auto& s : servers)
                if (s.id == selectedServerId) { cur = &s; break; }

            if (cur) {
                float addBtnX = sW - Styles::AddButtonMargin - Styles::AddButtonSize - 6;

                // Text channels
                ImGui::Dummy(ImVec2(0, Styles::SectionPadding));
                ImGui::Indent(16);
                ImGui::PushStyleColor(ImGuiCol_Text, Styles::TextMuted());
                ImGui::Text("TEXT CHANNELS");
                ImGui::PopStyleColor();
                ImGui::SameLine(addBtnX);
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
                if (ImGui::Button("+##add_text", ImVec2(Styles::AddButtonSize, Styles::AddButtonSize)))
                    ImGui::OpenPopup("CreateChannelPopup");
                ImGui::PopStyleVar();
                ImGui::Unindent(16);
                ImGui::Dummy(ImVec2(0, 4));

                for (const auto& ch : cur->channels) {
                    if (ch.type != ChannelType::Text) continue;
                    bool sel = (selectedChannelId == ch.id);
                    std::string label = "  # " + ch.name;

                    if (sel) {
                        ImGui::PushStyleColor(ImGuiCol_Header, Styles::ButtonSubtle());
                        ImGui::PushStyleColor(ImGuiCol_Text, Styles::Accent());
                    }
                    ImGui::Indent(10);
                    if (ImGui::Selectable(label.c_str(), sel, 0, ImVec2(sW - 30, 26))) {
                        if (selectedChannelId != ch.id) {
                            selectedChannelId = ch.id;
                            showSettings = false;
                            netClient.Send(PacketType::Select_Text_Channel, PacketHandler::SelectTextChannelPayload(ch.id));
                        }
                    }
                    ImGui::Unindent(10);
                    if (sel) ImGui::PopStyleColor(2);
                }

                // Voice channels
                ImGui::Dummy(ImVec2(0, Styles::SectionPadding));
                ImGui::Indent(16);
                ImGui::PushStyleColor(ImGuiCol_Text, Styles::TextMuted());
                ImGui::Text("VOICE CHANNELS");
                ImGui::PopStyleColor();
                ImGui::SameLine(addBtnX);
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
                if (ImGui::Button("+##add_voice", ImVec2(Styles::AddButtonSize, Styles::AddButtonSize)))
                    ImGui::OpenPopup("CreateChannelPopup");
                ImGui::PopStyleVar();
                ImGui::Unindent(16);
                ImGui::Dummy(ImVec2(0, 4));

                for (const auto& ch : cur->channels) {
                    if (ch.type != ChannelType::Voice) continue;
                    bool active = (activeVoiceChannelId == ch.id);
                    std::string label = (active ? "  > " : "  ~  ") + ch.name;

                    if (active) ImGui::PushStyleColor(ImGuiCol_Text, Styles::Accent());
                    ImGui::Indent(10);
                    if (ImGui::Selectable(label.c_str(), active, 0, ImVec2(sW - 30, 26))) {
                        selectedChannelId = ch.id;
                        showSettings = false;
                        if (activeVoiceChannelId != ch.id) {
                            activeVoiceChannelId = ch.id;
                            netClient.Send(PacketType::Join_Voice_Channel, PacketHandler::JoinVoiceChannelPayload(ch.id));
                        }
                    }
                    ImGui::Unindent(10);
                    if (active) ImGui::PopStyleColor();
                }

                if (activeVoiceChannelId != -1 && !voiceMembers.empty()) {
                    ImGui::Indent(26);
                    ImGui::PushStyleColor(ImGuiCol_Text, Styles::TextMuted());
                    ImGui::Text("%d in call", (int)voiceMembers.size());
                    ImGui::PopStyleColor();
                    ImGui::Unindent(26);
                }
            }
        }

        // Create channel popup
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Styles::PopupPadding, Styles::PopupPadding));
        ImGui::PushStyleColor(ImGuiCol_PopupBg, Styles::BgPopup());
        ImGui::PushStyleColor(ImGuiCol_FrameBg, Styles::ButtonSubtle());
        if (ImGui::BeginPopup("CreateChannelPopup")) {
            static int type = 0;
            ImGui::Text("Create Channel");
            ImGui::Dummy(ImVec2(0, 4));
            ImGui::SetNextItemWidth(220);
            ImGui::InputText("##cname", newChannelNameBuf, 64);
            ImGui::Dummy(ImVec2(0, 4));
            ImGui::RadioButton("Text", &type, 0);
            ImGui::SameLine();
            ImGui::RadioButton("Voice", &type, 1);
            ImGui::Dummy(ImVec2(0, 6));
            if (UI::AccentButton("Create", ImVec2(220, 30))) {
                if (selectedServerId != -1 && strlen(newChannelNameBuf) > 0) {
                    std::string tStr = (type == 1) ? "voice" : "text";
                    netClient.Send(PacketType::Create_Channel_Request, PacketHandler::CreateChannelPayload(selectedServerId, newChannelNameBuf, tStr));
                    memset(newChannelNameBuf, 0, 64);
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::EndPopup();
        }
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar();

        // Footer (static: username + MIC/SPK/INF row + SETTINGS row)
        float footerH = Styles::FooterHeight;
        ImGui::SetCursorPos(ImVec2(0, windowHeight - footerH));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, Styles::BgFooter());
        ImGui::BeginChild("Footer", ImVec2(sW, footerH), false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

        float pad = 12.0f;
        float innerW = sW - pad * 2;

        ImGui::Dummy(ImVec2(0, 16));
        ImGui::Indent(pad);
        size_t hashPos = currentUser.username.find('#');
        if (hashPos != std::string::npos) {
            ImGui::Text("%s", currentUser.username.substr(0, hashPos).c_str());
            ImGui::SameLine(0, 0);
            ImGui::TextDisabled("%s", currentUser.username.substr(hashPos).c_str());
        } else {
            ImGui::Text("%s", currentUser.username.c_str());
        }

        ImGui::Dummy(ImVec2(0, 8));

        float btnH = 30.0f;
        float gap = 4.0f;
        float btnW3 = (innerW - gap * 2) / 3.0f;

        bool wasMuted = selfMuted;
        bool wasDeafened = selfDeafened;

        // Row 1: MIC | SPK | INF
        if (wasMuted) {
            ImGui::PushStyleColor(ImGuiCol_Button, Styles::ButtonDanger());
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Styles::ButtonDangerHover());
        }
        if (ImGui::Button(wasMuted ? "MIC X" : "MIC", ImVec2(btnW3, btnH))) {
            selfMuted = !selfMuted;
            if (!selfMuted && selfDeafened) selfDeafened = false;
        }
        if (wasMuted) ImGui::PopStyleColor(2);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip(selfMuted ? "Unmute Microphone" : "Mute Microphone");

        ImGui::SameLine(0, gap);

        if (wasDeafened) {
            ImGui::PushStyleColor(ImGuiCol_Button, Styles::ButtonDanger());
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Styles::ButtonDangerHover());
        }
        if (ImGui::Button(wasDeafened ? "SPK X" : "SPK", ImVec2(btnW3, btnH))) {
            selfDeafened = !selfDeafened;
            if (selfDeafened) selfMuted = true;
        }
        if (wasDeafened) ImGui::PopStyleColor(2);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip(selfDeafened ? "Undeafen" : "Deafen");

        ImGui::SameLine(0, gap);

        bool inVoice = (activeVoiceChannelId != -1);
        if (!inVoice) {
            ImGui::PushStyleColor(ImGuiCol_Button, Styles::ButtonSubtle());
            ImGui::PushStyleColor(ImGuiCol_Text, Styles::TextMuted());
        }
        if (ImGui::Button("INF", ImVec2(btnW3, btnH)) && inVoice)
            ImGui::OpenPopup("VoiceInfoPopup");
        if (!inVoice) ImGui::PopStyleColor(2);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip(inVoice ? "Voice Call Info" : "Not in a voice channel");

        // Voice info popup
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16, 14));
        ImGui::PushStyleColor(ImGuiCol_PopupBg, Styles::BgPopup());
        if (ImGui::BeginPopup("VoiceInfoPopup")) {
            ImGui::PushStyleColor(ImGuiCol_Text, Styles::Accent());
            ImGui::Text("Voice Call Info");
            ImGui::PopStyleColor();
            ImGui::Dummy(ImVec2(0, 6));
            ImGui::PushStyleColor(ImGuiCol_Separator, Styles::Separator());
            ImGui::Separator();
            ImGui::PopStyleColor();
            ImGui::Dummy(ImVec2(0, 8));

            ImGui::Text("Avg Ping:");           ImGui::SameLine(160); ImGui::Text("%.0f ms", voiceInfo.avgPingMs);
            ImGui::Text("Packet Loss:");        ImGui::SameLine(160); ImGui::Text("%.1f %%", voiceInfo.packetLossPercent);
            ImGui::Dummy(ImVec2(0, 4));
            ImGui::Text("Packets Received:");   ImGui::SameLine(160); ImGui::Text("%d", voiceInfo.packetsReceived);
            ImGui::Text("Packets Lost:");       ImGui::SameLine(160); ImGui::Text("%d", voiceInfo.packetsLost);
            ImGui::Dummy(ImVec2(0, 4));
            ImGui::Text("Buffer:");             ImGui::SameLine(160); ImGui::Text("%d ms", voiceInfo.currentBufferMs);
            ImGui::Text("Encoder Bitrate:");    ImGui::SameLine(160); ImGui::Text("%d kbps", voiceInfo.encoderBitrateKbps);

            if (voiceInfo.packetLossPercent > 10.0f) {
                ImGui::Dummy(ImVec2(0, 8));
                ImGui::PushStyleColor(ImGuiCol_Text, Styles::Error());
                ImGui::TextWrapped("High packet loss detected. Voice quality may be degraded.");
                ImGui::PopStyleColor();
            }

            ImGui::EndPopup();
        }
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();

        // Row 2: SETTINGS (full width)
        ImGui::Dummy(ImVec2(0, 4));
        if (ImGui::Button("Settings", ImVec2(innerW, btnH)))
            showSettings = !showSettings;

        ImGui::Unindent(pad);
        ImGui::EndChild();
        ImGui::PopStyleColor();

        ImGui::EndChild();
        ImGui::PopStyleColor();

        // ================================================================
        //  RIGHT PANEL: Server rail
        // ================================================================
        ImGui::SetCursorPos(ImVec2(parentW - railW, 0));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, Styles::BgSidebar());
        ImGui::BeginChild("ServerRail", ImVec2(railW, windowHeight), false, ImGuiWindowFlags_NoScrollbar);

        float btnSz = 42.0f;
        float btnX  = (railW - btnSz) * 0.5f;

        ImGui::Dummy(ImVec2(0, 12));

        // Create / Join server button
        ImGui::SetCursorPosX(btnX);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
        if (ImGui::Button("+##new_srv", ImVec2(btnSz, btnSz)))
            ImGui::OpenPopup("CreateServerPopup");
        ImGui::PopStyleVar();

        // Create / Join popup
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Styles::PopupPadding, Styles::PopupPadding));
        ImGui::PushStyleColor(ImGuiCol_PopupBg, Styles::BgPopup());
        ImGui::PushStyleColor(ImGuiCol_FrameBg, Styles::ButtonSubtle());
        if (ImGui::BeginPopup("CreateServerPopup")) {
            ImGui::Text("Create a Server");
            ImGui::Dummy(ImVec2(0, 4));
            ImGui::SetNextItemWidth(220);
            ImGui::InputText("##sname", newServerNameBuf, 64);
            ImGui::Dummy(ImVec2(0, 4));
            if (UI::AccentButton("Create", ImVec2(220, 30))) {
                if (strlen(newServerNameBuf) > 0) {
                    netClient.Send(PacketType::Create_Server_Request, PacketHandler::CreateServerPayload(newServerNameBuf, currentUser.username));
                    memset(newServerNameBuf, 0, 64);
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::Dummy(ImVec2(0, 6));
            ImGui::PushStyleColor(ImGuiCol_Separator, Styles::Separator());
            ImGui::Separator();
            ImGui::PopStyleColor();
            ImGui::Dummy(ImVec2(0, 6));
            ImGui::Text("Join with Code");
            ImGui::Dummy(ImVec2(0, 4));
            static char joinCode[10] = "";
            ImGui::SetNextItemWidth(220);
            ImGui::InputText("##jcode", joinCode, 10);
            ImGui::Dummy(ImVec2(0, 4));
            if (UI::AccentButton("Join", ImVec2(220, 30))) {
                netClient.Send(PacketType::Join_Server_Request, PacketHandler::JoinServerPayload(joinCode, currentUser.username));
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar();

        ImGui::Dummy(ImVec2(0, 8));
        ImGui::PushStyleColor(ImGuiCol_Separator, Styles::Separator());
        ImGui::Separator();
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0, 8));

        // Server buttons
        for (const auto& server : servers) {
            ImGui::SetCursorPosX(btnX);

            // Selection indicator (right edge bar)
            if (selectedServerId == server.id) {
                ImDrawList* dl = ImGui::GetWindowDrawList();
                ImVec2 p = ImGui::GetCursorScreenPos();
                dl->AddRectFilled(
                    ImVec2(p.x + btnSz + btnX - 2, p.y + 8),
                    ImVec2(p.x + btnSz + btnX + 2, p.y + btnSz - 8),
                    Styles::ColSelectedIndicator(), 2.0f);
            }

            ImGui::PushID(server.id);
            std::string initials = server.name.substr(0, 2);
            if (UI::AccentButton(initials.c_str(), ImVec2(btnSz, btnSz))) {
                selectedServerId = server.id;
                showSettings = false;
                netClient.Send(PacketType::Get_Server_Content_Request, PacketHandler::GetServerContentPayload(server.id));
            }
            if (ImGui::IsItemHovered()) {
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 6));
                ImGui::SetTooltip("%s", server.name.c_str());
                ImGui::PopStyleVar();
            }
            if (ImGui::BeginPopupContextItem()) {
                if (ImGui::Selectable("Copy Invite Code"))
                    ImGui::SetClipboardText(server.inviteCode.c_str());
                ImGui::EndPopup();
            }
            ImGui::PopID();
            ImGui::Dummy(ImVec2(0, 4));
        }

        ImGui::EndChild();
        ImGui::PopStyleColor();
    }
}
