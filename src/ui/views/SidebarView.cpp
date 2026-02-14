#include "SidebarView.h"
#include "../Components.h"
#include "../../network/PacketHandler.h"
#include "../../core/ConfigManager.h"
#include <string>

namespace TalkMe::UI::Views {

    void RenderSidebar(NetworkClient& netClient, UserSession& currentUser, AppState& currentState, std::vector<Server>& servers, int& selectedServerId, int& selectedChannelId, int& activeVoiceChannelId, std::vector<std::string>& voiceMembers, char* newServerNameBuf, char* newChannelNameBuf) {
        float windowHeight = ImGui::GetWindowHeight();

        // FIX: Increased channel list width so footer buttons fit comfortably
        float serverRailWidth = 72.0f;
        float channelListWidth = 280.0f;

        // --- 1. SERVER RAIL ---
        ImGui::SetCursorPos(ImVec2(0, 0));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.05f, 0.05f, 0.06f, 1.00f));
        ImGui::BeginChild("ServerRail", ImVec2(serverRailWidth, windowHeight), false, ImGuiWindowFlags_NoScrollbar);
        ImGui::Dummy(ImVec2(0, 10)); ImGui::SetCursorPosX((serverRailWidth - 48) * 0.5f);
        if (ImGui::Button("+", ImVec2(48, 48))) ImGui::OpenPopup("CreateServerPopup");

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20, 20)); ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0.13f, 0.13f, 0.14f, 1.00f));
        if (ImGui::BeginPopup("CreateServerPopup")) {
            ImGui::Text("Create a Server"); ImGui::InputText("##sname", newServerNameBuf, 64); ImGui::Dummy(ImVec2(0, 10));
            if (UI::AccentButton("Create", ImVec2(200, 30))) { if (strlen(newServerNameBuf) > 0) { netClient.Send(PacketType::Create_Server_Request, PacketHandler::CreateServerPayload(newServerNameBuf, currentUser.username)); memset(newServerNameBuf, 0, 64); ImGui::CloseCurrentPopup(); } }
            ImGui::Separator(); ImGui::Text("Or Join with Code:"); static char joinCode[10] = ""; ImGui::InputText("##jcode", joinCode, 10);
            if (UI::AccentButton("Join", ImVec2(200, 30))) { netClient.Send(PacketType::Join_Server_Request, PacketHandler::JoinServerPayload(joinCode, currentUser.username)); ImGui::CloseCurrentPopup(); }
            ImGui::EndPopup();
        }
        ImGui::PopStyleColor(); ImGui::PopStyleVar();

        ImGui::Dummy(ImVec2(0, 10)); ImGui::Separator(); ImGui::Dummy(ImVec2(0, 10));

        for (const auto& server : servers) {
            ImGui::SetCursorPosX((serverRailWidth - 48) * 0.5f);
            if (selectedServerId == server.id) { ImDrawList* draw_list = ImGui::GetWindowDrawList(); ImVec2 p = ImGui::GetCursorScreenPos(); draw_list->AddRectFilled(ImVec2(p.x - 8, p.y + 10), ImVec2(p.x - 4, p.y + 38), IM_COL32(255, 255, 255, 255), 4.0f); }
            ImGui::PushID(server.id); std::string initials = server.name.substr(0, 2);
            if (UI::AccentButton(initials.c_str(), ImVec2(48, 48))) { selectedServerId = server.id; netClient.Send(PacketType::Get_Server_Content_Request, PacketHandler::GetServerContentPayload(server.id)); }
            if (ImGui::BeginPopupContextItem()) { if (ImGui::Selectable("Copy Invite Code")) ImGui::SetClipboardText(server.inviteCode.c_str()); ImGui::EndPopup(); }
            ImGui::PopID(); ImGui::Dummy(ImVec2(0, 8));
        }
        ImGui::EndChild(); ImGui::PopStyleColor();

        // --- 2. CHANNEL LIST ---
        ImGui::SetCursorPos(ImVec2(serverRailWidth, 0));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.11f, 0.11f, 0.12f, 1.00f));
        ImGui::BeginChild("ChannelPanel", ImVec2(channelListWidth, windowHeight), false, ImGuiWindowFlags_None);

        ImGui::Dummy(ImVec2(0, 15)); ImGui::Indent(15);
        if (selectedServerId != -1) {
            std::string sName = "Loading..."; for (const auto& s : servers) { if (s.id == selectedServerId) sName = s.name; }
            ImGui::PushFont(ImGui::GetIO().Fonts->Fonts[0]); ImGui::Text("%s", sName.c_str()); ImGui::PopFont();
        }
        else ImGui::Text("Select a Server");
        ImGui::Unindent(15); ImGui::Dummy(ImVec2(0, 15)); ImGui::Separator();

        if (selectedServerId != -1) {
            const Server* currentServer = nullptr; for (const auto& s : servers) { if (s.id == selectedServerId) { currentServer = &s; break; } }
            if (currentServer) {
                ImGui::Dummy(ImVec2(0, 10)); ImGui::Indent(10); ImGui::TextDisabled("TEXT CHANNELS"); ImGui::SameLine(channelListWidth - 30);
                if (ImGui::Button("+##add_text")) ImGui::OpenPopup("CreateChannelPopup"); ImGui::Unindent(10);

                for (const auto& ch : currentServer->channels) {
                    if (ch.type == ChannelType::Text) {
                        bool isSelected = (selectedChannelId == ch.id); std::string label = "# " + ch.name; ImGui::Indent(5);
                        if (ImGui::Selectable(label.c_str(), isSelected)) { if (selectedChannelId != ch.id) { selectedChannelId = ch.id; netClient.Send(PacketType::Select_Text_Channel, PacketHandler::SelectTextChannelPayload(ch.id)); } }
                        ImGui::Unindent(5);
                    }
                }

                ImGui::Dummy(ImVec2(0, 20)); ImGui::Indent(10); ImGui::TextDisabled("VOICE CHANNELS"); ImGui::SameLine(channelListWidth - 30);
                if (ImGui::Button("+##add_voice")) ImGui::OpenPopup("CreateChannelPopup"); ImGui::Unindent(10);

                for (const auto& ch : currentServer->channels) {
                    if (ch.type == ChannelType::Voice) {
                        bool isActive = (activeVoiceChannelId == ch.id); std::string icon = isActive ? "[Connected] " : "[v] "; std::string label = icon + ch.name;
                        if (isActive) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 1.0f, 0.4f, 1.0f)); ImGui::Indent(5);
                        if (ImGui::Selectable(label.c_str(), isActive)) {
                            selectedChannelId = ch.id;
                            if (activeVoiceChannelId != ch.id) { activeVoiceChannelId = ch.id; netClient.Send(PacketType::Join_Voice_Channel, PacketHandler::JoinVoiceChannelPayload(ch.id)); }
                        }
                        ImGui::Unindent(5); if (isActive) ImGui::PopStyleColor();
                    }
                }
            }
        }

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20, 20)); ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0.13f, 0.13f, 0.14f, 1.00f));
        if (ImGui::BeginPopup("CreateChannelPopup")) {
            static int type = 0; ImGui::Text("Create Channel"); ImGui::InputText("##cname", newChannelNameBuf, 64); ImGui::RadioButton("Text", &type, 0); ImGui::SameLine(); ImGui::RadioButton("Voice", &type, 1);
            if (UI::AccentButton("Create", ImVec2(200, 30))) { if (selectedServerId != -1 && strlen(newChannelNameBuf) > 0) { std::string tStr = (type == 1) ? "voice" : "text"; netClient.Send(PacketType::Create_Channel_Request, PacketHandler::CreateChannelPayload(selectedServerId, newChannelNameBuf, tStr)); memset(newChannelNameBuf, 0, 64); ImGui::CloseCurrentPopup(); } }
            ImGui::EndPopup();
        }
        ImGui::PopStyleColor(); ImGui::PopStyleVar();

        // --- 3. FOOTER (Redesigned for spacing) ---
        float footerH = 85.0f; // Made taller to fit 2 rows
        ImGui::SetCursorPos(ImVec2(0, windowHeight - footerH));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.08f, 0.08f, 0.09f, 1.00f));
        ImGui::BeginChild("Footer", ImVec2(channelListWidth, footerH));

        // ROW 1: Username
        ImGui::SetCursorPos(ImVec2(15, 12));
        size_t hashPos = currentUser.username.find('#');
        if (hashPos != std::string::npos) {
            ImGui::Text("%s", currentUser.username.substr(0, hashPos).c_str());
            ImGui::SameLine(0, 0);
            ImGui::TextDisabled("%s", currentUser.username.substr(hashPos).c_str());
        }
        else {
            ImGui::Text("%s", currentUser.username.c_str());
        }

        // ROW 2: Action Buttons
        ImGui::SetCursorPos(ImVec2(15, 42));

        // Log Out
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.2f, 0.2f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.3f, 0.3f, 1.0f));
        if (ImGui::Button("Log Out", ImVec2(75, 30))) {
            ConfigManager::Get().ClearSession();
            netClient.Disconnect();
            currentState = AppState::Login;
            selectedServerId = -1;
            selectedChannelId = -1;
            activeVoiceChannelId = -1;
        }
        ImGui::PopStyleColor(2);

        // Voice Disconnect
        if (activeVoiceChannelId != -1) {
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.2f, 0.2f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.9f, 0.3f, 0.3f, 1.0f));
            if (ImGui::Button("Drop Call", ImVec2(85, 30))) {
                activeVoiceChannelId = -1;
                netClient.Send(PacketType::Join_Voice_Channel, PacketHandler::JoinVoiceChannelPayload(-1));
            }
            ImGui::PopStyleColor(2);
        }

        ImGui::EndChild(); ImGui::PopStyleColor(); ImGui::EndChild(); ImGui::PopStyleColor();
    }
}