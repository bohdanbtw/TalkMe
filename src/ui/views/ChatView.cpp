#include "ChatView.h"
#include "../Components.h"
#include "../Theme.h"
#include "../../network/PacketHandler.h"
#include <string>

#ifndef max
#define max(a,b) (((a) > (b)) ? (a) : (b))
#endif

namespace TalkMe::UI::Views {

    void RenderChannelView(NetworkClient& netClient, UserSession& currentUser, const Server& currentServer, std::vector<ChatMessage>& messages, int& selectedChannelId, int& activeVoiceChannelId, std::vector<std::string>& voiceMembers, std::map<std::string, float>& speakingTimers, char* chatInputBuf) {
        float windowHeight = ImGui::GetWindowHeight();
        float windowWidth = ImGui::GetWindowWidth();

        // FIX: Match exact width of new Sidebar (72 + 280)
        float leftOffset = 352.0f;

        ImGui::SetCursorPos(ImVec2(leftOffset, 0));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.14f, 0.14f, 0.15f, 1.00f));
        ImGui::BeginChild("ChatArea", ImVec2(windowWidth - leftOffset, windowHeight), false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

        if (selectedChannelId != -1) {
            std::string channelName = "Unknown"; ChannelType cType = ChannelType::Text;
            for (const auto& ch : currentServer.channels) { if (ch.id == selectedChannelId) { channelName = ch.name; cType = ch.type; break; } }

            // --- VOICE VIEW ---
            if (cType == ChannelType::Voice) {
                ImGui::SetCursorPos(ImVec2(40, 40));
                ImGui::SetWindowFontScale(1.8f);
                ImGui::Text("Connected to %s", channelName.c_str());
                ImGui::SetWindowFontScale(1.0f);
                ImGui::Dummy(ImVec2(0, 40));

                ImGui::SetCursorPos(ImVec2(40, ImGui::GetCursorPosY()));
                ImGui::BeginChild("VoiceGrid", ImVec2(windowWidth - leftOffset - 40, windowHeight - 150), false, ImGuiWindowFlags_None);

                // FIX: ImGui native grid rendering for perfect centering
                float avatarRadius = 60.0f; // Massive circles
                float itemWidth = 160.0f;   // Box size per user
                float itemHeight = 180.0f;
                float spacing = 20.0f;

                float availableWidth = ImGui::GetContentRegionAvail().x;
                int columns = max(1, (int)(availableWidth / (itemWidth + spacing)));
                int col = 0;

                for (const auto& member : voiceMembers) {
                    ImGui::BeginGroup();

                    // Reserve space in layout
                    ImVec2 pos = ImGui::GetCursorScreenPos();
                    ImGui::InvisibleButton(("##u_" + member).c_str(), ImVec2(itemWidth, itemHeight));

                    ImDrawList* draw = ImGui::GetWindowDrawList();
                    ImVec2 center = ImVec2(pos.x + itemWidth * 0.5f, pos.y + avatarRadius + 10.0f);

                    // Draw Base Circle
                    draw->AddCircleFilled(center, avatarRadius, IM_COL32(55, 55, 65, 255));

                    // Draw Speaking Indicator
                    bool isSpeaking = ((float)ImGui::GetTime() - speakingTimers[member] < 0.5f);
                    if (isSpeaking) {
                        draw->AddCircle(center, avatarRadius + 5.0f, IM_COL32(60, 255, 60, 255), 0, 4.0f);
                    }

                    // Draw Initials
                    std::string init = member.substr(0, 2);
                    float fontSize = 38.0f;
                    ImVec2 textSize = ImGui::GetFont()->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, init.c_str());
                    draw->AddText(ImGui::GetFont(), fontSize, ImVec2(center.x - textSize.x * 0.5f, center.y - textSize.y * 0.5f), IM_COL32(255, 255, 255, 255), init.c_str());

                    // Draw Formatted Username
                    std::string disp = member;
                    size_t hashPos = member.find('#');
                    if (hashPos != std::string::npos) disp = member.substr(0, hashPos);

                    ImVec2 nameSize = ImGui::CalcTextSize(disp.c_str());
                    draw->AddText(ImVec2(pos.x + (itemWidth - nameSize.x) * 0.5f, pos.y + avatarRadius * 2.0f + 25.0f), IM_COL32(230, 230, 230, 255), disp.c_str());

                    ImGui::EndGroup();

                    col++;
                    if (col < columns) {
                        ImGui::SameLine(0, spacing);
                    }
                    else {
                        col = 0;
                        ImGui::Dummy(ImVec2(0, spacing));
                    }
                }
                ImGui::EndChild(); // End VoiceGrid
            }

            // --- TEXT VIEW ---
            else {
                ImGui::SetCursorPos(ImVec2(30, 20)); ImGui::BeginGroup(); ImGui::SetWindowFontScale(1.5f); ImGui::TextDisabled("#"); ImGui::SameLine(); ImGui::Text("%s", channelName.c_str()); ImGui::SetWindowFontScale(1.0f);
                ImGui::TextDisabled("Server Invite: %s", currentServer.inviteCode.c_str()); ImGui::EndGroup();

                // FIX: Explicitly forcing cursor down so text never overlaps
                ImGui::Dummy(ImVec2(0, 20));
                ImGui::PushStyleColor(ImGuiCol_Separator, ImVec4(0.20f, 0.20f, 0.22f, 1.00f));
                ImGui::Separator();
                ImGui::PopStyleColor();
                ImGui::Dummy(ImVec2(0, 15));

                float headerHeight = ImGui::GetCursorPosY();
                float inputHeight = 90.0f;
                float messageAreaHeight = windowHeight - headerHeight - inputHeight;
                if (messageAreaHeight < 50.0f) messageAreaHeight = 50.0f;

                ImGui::SetCursorPos(ImVec2(30, headerHeight));
                ImGui::BeginChild("Messages", ImVec2(windowWidth - leftOffset - 60, messageAreaHeight), false, ImGuiWindowFlags_None);

                for (const auto& msg : messages) {
                    if (msg.channelId == selectedChannelId) {
                        bool isMe = (msg.sender == currentUser.username); ImGui::BeginGroup();
                        ImGui::PushStyleColor(ImGuiCol_Text, isMe ? ImVec4(0.35f, 0.71f, 0.95f, 1.00f) : ImVec4(0.9f, 0.4f, 0.4f, 1.00f));
                        size_t hashPos = msg.sender.find('#');
                        if (hashPos != std::string::npos) {
                            ImGui::Text("%s", msg.sender.substr(0, hashPos).c_str()); ImGui::SameLine(0, 0);
                            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.5f, 0.5f, 1.0f)); ImGui::Text("%s", msg.sender.substr(hashPos).c_str()); ImGui::PopStyleColor();
                        }
                        else ImGui::Text("%s", msg.sender.c_str());
                        ImGui::PopStyleColor(); ImGui::SameLine(); ImGui::TextDisabled("%s", msg.timestamp.c_str());
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.9f, 0.9f, 0.9f, 1.00f)); ImGui::TextWrapped("%s", msg.content.c_str()); ImGui::PopStyleColor(); ImGui::EndGroup();

                        if (isMe && msg.id > 0) {
                            if (ImGui::BeginPopupContextItem(("msg_" + std::to_string(msg.id)).c_str())) {
                                if (ImGui::Selectable("Delete Message")) netClient.Send(PacketType::Delete_Message_Request, PacketHandler::CreateDeleteMessagePayload(msg.id, selectedChannelId, currentUser.username));
                                ImGui::EndPopup();
                            }
                        }
                        ImGui::Dummy(ImVec2(0, 15));
                    }
                }
                if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) ImGui::SetScrollHereY(1.0f);
                ImGui::EndChild();

                ImGui::SetCursorPos(ImVec2(30, windowHeight - 70)); ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 5.0f); ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.18f, 0.18f, 0.20f, 1.00f));
                ImGui::PushItemWidth(windowWidth - leftOffset - 150); bool enterPressed = ImGui::InputText("##chat_input", chatInputBuf, 1024, ImGuiInputTextFlags_EnterReturnsTrue); ImGui::PopItemWidth(); ImGui::SameLine();
                if (UI::AccentButton("Send", ImVec2(80, 35)) || enterPressed) {
                    if (strlen(chatInputBuf) > 0) { netClient.Send(PacketType::Message_Text, PacketHandler::CreateMessagePayload(selectedChannelId, currentUser.username, chatInputBuf)); memset(chatInputBuf, 0, 1024); ImGui::SetKeyboardFocusHere(-1); }
                }
                ImGui::PopStyleColor(); ImGui::PopStyleVar();
            }
        }
        else {
            const char* txt = "Select a Text Channel"; auto textSize = ImGui::CalcTextSize(txt);
            ImGui::SetCursorPos(ImVec2((windowWidth - leftOffset - textSize.x) * 0.5f, windowHeight * 0.5f)); ImGui::TextDisabled("%s", txt);
        }
        ImGui::EndChild(); ImGui::PopStyleColor();
    }
}