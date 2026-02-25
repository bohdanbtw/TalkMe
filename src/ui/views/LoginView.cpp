#include "LoginView.h"
#include "../Components.h"
#include "../Styles.h"
#include "../../shared/PacketHandler.h"

namespace TalkMe::UI::Views {

    void RenderLogin(NetworkClient& netClient, AppState& currentState,
        char* emailBuf, char* passwordBuf, char* statusMessage,
        const std::string& serverIP, int serverPort, const std::string& deviceId, bool validatingSession)
    {
        float winW = ImGui::GetWindowWidth();
        float winH = ImGui::GetWindowHeight();
        float cardW = Styles::LoginCardWidth;
        float cardH = 420.0f;
        float cardR = Styles::LoginCardRounding;

        // Card position
        float cx = (winW - cardW) * 0.5f;
        float cy = (winH - cardH) * 0.5f;
        if (cy < 40) cy = 40;

        // Draw card background
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 wp = ImGui::GetCursorScreenPos();
        ImVec2 cardTL = ImVec2(wp.x + cx, wp.y + cy);
        ImVec2 cardBR = ImVec2(cardTL.x + cardW, cardTL.y + cardH);
        dl->AddRectFilled(cardTL, cardBR, Styles::ColBgCard(), cardR);

        // Content inside card
        float pad = 40.0f;
        float fieldW = cardW - pad * 2;
        ImGui::SetCursorPos(ImVec2(cx + pad, cy + 36));
        ImGui::BeginGroup();

        // Title
        ImGui::PushStyleColor(ImGuiCol_Text, Styles::Accent());
        ImGui::SetWindowFontScale(1.7f);
        const char* title = "TalkMe";
        ImVec2 tsz = ImGui::CalcTextSize(title);
        ImGui::SetCursorPosX(cx + (cardW - tsz.x) * 0.5f);
        ImGui::Text("%s", title);
        ImGui::SetWindowFontScale(1.0f);
        ImGui::PopStyleColor();

        ImGui::Dummy(ImVec2(0, 4));
        const char* sub = "Sign in to continue";
        ImVec2 ssz = ImGui::CalcTextSize(sub);
        ImGui::SetCursorPosX(cx + (cardW - ssz.x) * 0.5f);
        ImGui::TextDisabled("%s", sub);

        ImGui::Dummy(ImVec2(0, 28));

        ImGui::SetCursorPosX(cx + pad);
        ImGui::PushStyleColor(ImGuiCol_Text, Styles::TextSecondary());
        ImGui::Text("Email");
        ImGui::PopStyleColor();
        ImGui::SetCursorPosX(cx + pad);
        ImGui::PushItemWidth(fieldW);
        ImGui::InputText("##email", emailBuf, 128, validatingSession ? ImGuiInputTextFlags_ReadOnly : 0);
        ImGui::PopItemWidth();

        ImGui::Dummy(ImVec2(0, 14));

        ImGui::SetCursorPosX(cx + pad);
        ImGui::PushStyleColor(ImGuiCol_Text, Styles::TextSecondary());
        ImGui::Text("Password");
        ImGui::PopStyleColor();
        ImGui::SetCursorPosX(cx + pad);
        ImGui::PushItemWidth(fieldW);
        bool enterPressed = ImGui::InputText("##password", passwordBuf, 128,
            ImGuiInputTextFlags_Password | ImGuiInputTextFlags_EnterReturnsTrue | (validatingSession ? ImGuiInputTextFlags_ReadOnly : 0));
        ImGui::PopItemWidth();

        ImGui::Dummy(ImVec2(0, 24));

        ImGui::SetCursorPosX(cx + pad);
        bool signInClicked = (UI::AccentButton(validatingSession ? "Signing in..." : "Sign In", ImVec2(fieldW, 42)) || enterPressed) && !validatingSession;
        if (signInClicked) {
            if (strlen(emailBuf) > 0 && strlen(passwordBuf) > 0) {
                std::string emailStr(emailBuf);
                std::string passStr(passwordBuf);
                if (!netClient.IsConnected()) {
                    strcpy_s(statusMessage, 256, "Connecting...");
                    netClient.ConnectAsync(serverIP, serverPort,
                        [&netClient, statusMessage, emailStr, passStr, deviceId](bool success) {
                            if (success) {
                                strcpy_s(statusMessage, 256, "Authenticating...");
                                netClient.Send(PacketType::Login_Request,
                                    PacketHandler::CreateLoginPayload(emailStr, passStr, deviceId));
                            } else {
                                strcpy_s(statusMessage, 256, "Server offline.");
                            }
                        });
                } else {
                    strcpy_s(statusMessage, 256, "Authenticating...");
                    netClient.Send(PacketType::Login_Request,
                        PacketHandler::CreateLoginPayload(emailStr, passStr, deviceId));
                }
            } else {
                strcpy_s(statusMessage, 256, "Please fill all fields.");
            }
        }

        ImGui::Dummy(ImVec2(0, 12));

        ImGui::SetCursorPosX(cx + pad);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0,0,0,0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0,0,0,0));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0,0,0,0));
        ImGui::PushStyleColor(ImGuiCol_Text, Styles::TextMuted());
        if (ImGui::Button("Need an account? Register", ImVec2(fieldW, 28)) && !validatingSession) {
            currentState = AppState::Register;
            memset(statusMessage, 0, 256);
        }
        ImGui::PopStyleColor(4);

        if (strlen(statusMessage) > 0) {
            ImGui::Dummy(ImVec2(0, 8));
            ImVec2 sts = ImGui::CalcTextSize(statusMessage);
            ImGui::SetCursorPosX(cx + (cardW - sts.x) * 0.5f);
            ImGui::PushStyleColor(ImGuiCol_Text, Styles::Error());
            ImGui::Text("%s", statusMessage);
            ImGui::PopStyleColor();
        }

        ImGui::EndGroup();
    }
}
