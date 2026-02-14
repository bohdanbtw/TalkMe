#include "RegisterView.h"
#include "../Components.h"
#include "../Theme.h"
#include "../../network/PacketHandler.h"

namespace TalkMe::UI::Views {

    void RenderRegister(NetworkClient& netClient, AppState& currentState, char* emailBuf, char* usernameBuf, char* passwordBuf, char* passwordRepeatBuf, char* statusMessage, const std::string& serverIP, int serverPort) {
        float windowWidth = ImGui::GetWindowWidth();
        float windowHeight = ImGui::GetWindowHeight();
        float contentWidth = 320.0f;
        float contentHeight = 480.0f;

        ImGui::SetCursorPos(ImVec2((windowWidth - contentWidth) * 0.5f, (windowHeight - contentHeight) * 0.5f));
        ImGui::BeginGroup();

        const char* title = "Create Account";
        ImGui::SetWindowFontScale(1.8f);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (contentWidth - ImGui::CalcTextSize(title).x) * 0.5f);
        ImGui::Text("%s", title);
        ImGui::SetWindowFontScale(1.0f);

        ImGui::Dummy(ImVec2(0, 35));

        ImGui::TextDisabled("Email");
        ImGui::PushItemWidth(contentWidth);
        ImGui::InputText("##email", emailBuf, 128);
        ImGui::PopItemWidth();

        ImGui::Dummy(ImVec2(0, 15));

        ImGui::TextDisabled("Username");
        ImGui::PushItemWidth(contentWidth);
        ImGui::InputText("##username", usernameBuf, 128);
        ImGui::PopItemWidth();

        ImGui::Dummy(ImVec2(0, 15));

        ImGui::TextDisabled("Password");
        ImGui::PushItemWidth(contentWidth);
        ImGui::InputText("##password", passwordBuf, 128, ImGuiInputTextFlags_Password);
        ImGui::PopItemWidth();

        ImGui::Dummy(ImVec2(0, 15));

        ImGui::TextDisabled("Repeat Password");
        ImGui::PushItemWidth(contentWidth);
        bool enterPressed = ImGui::InputText("##password_rep", passwordRepeatBuf, 128, ImGuiInputTextFlags_Password | ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::PopItemWidth();

        ImGui::Dummy(ImVec2(0, 30));

        if (UI::AccentButton("Register", ImVec2(contentWidth, 45)) || enterPressed) {
            if (strlen(emailBuf) > 0 && strlen(usernameBuf) > 0 && strlen(passwordBuf) > 0) {
                if (strcmp(passwordBuf, passwordRepeatBuf) == 0) {
                    std::string emailStr(emailBuf);
                    std::string userStr(usernameBuf);
                    std::string passStr(passwordBuf);

                    if (!netClient.IsConnected()) {
                        strcpy_s(statusMessage, 256, "Connecting to server...");
                        netClient.ConnectAsync(serverIP, serverPort, [&netClient, emailStr, userStr, passStr, statusMessage](bool success) {
                            if (success) {
                                strcpy_s(statusMessage, 256, "Authenticating...");
                                netClient.Send(PacketType::Register_Request, PacketHandler::CreateRegisterPayload(emailStr, userStr, passStr));
                            }
                            else {
                                strcpy_s(statusMessage, 256, "Server offline or unreachable.");
                            }
                            });
                    }
                    else {
                        strcpy_s(statusMessage, 256, "Authenticating...");
                        netClient.Send(PacketType::Register_Request, PacketHandler::CreateRegisterPayload(emailStr, userStr, passStr));
                    }
                }
                else strcpy_s(statusMessage, 256, "Passwords do not match.");
            }
            else strcpy_s(statusMessage, 256, "Please fill all fields.");
        }

        ImGui::Dummy(ImVec2(0, 15));

        if (ImGui::Button("Already have an account? Login", ImVec2(contentWidth, 35))) {
            currentState = AppState::Login;
            memset(statusMessage, 0, 256);
        }

        if (strlen(statusMessage) > 0) {
            ImGui::Dummy(ImVec2(0, 15));
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (contentWidth - ImGui::CalcTextSize(statusMessage).x) * 0.5f);
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.9f, 0.4f, 0.4f, 1.0f));
            ImGui::Text("%s", statusMessage);
            ImGui::PopStyleColor();
        }

        ImGui::EndGroup();
    }
}