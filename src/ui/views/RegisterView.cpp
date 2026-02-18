#include "RegisterView.h"
#include "../Components.h"
#include "../Styles.h"
#include "../../network/PacketHandler.h"

namespace TalkMe::UI::Views {

    void RenderRegister(NetworkClient& netClient, AppState& currentState,
        char* emailBuf, char* usernameBuf, char* passwordBuf, char* passwordRepeatBuf,
        char* statusMessage, const std::string& serverIP, int serverPort)
    {
        float winW = ImGui::GetWindowWidth();
        float winH = ImGui::GetWindowHeight();
        float cardW = Styles::LoginCardWidth;
        float cardH = 620.0f;
        float cardR = Styles::LoginCardRounding;

        float cx = (winW - cardW) * 0.5f;
        float cy = (winH - cardH) * 0.5f;
        if (cy < 30) cy = 30;

        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 wp = ImGui::GetCursorScreenPos();
        ImVec2 cardTL = ImVec2(wp.x + cx, wp.y + cy);
        ImVec2 cardBR = ImVec2(cardTL.x + cardW, cardTL.y + cardH);
        dl->AddRectFilled(cardTL, cardBR, Styles::ColBgCard(), cardR);

        float pad = 40.0f;
        float fieldW = cardW - pad * 2;
        ImGui::SetCursorPos(ImVec2(cx + pad, cy + 32));
        ImGui::BeginGroup();

        ImGui::PushStyleColor(ImGuiCol_Text, Styles::Accent());
        ImGui::SetWindowFontScale(1.7f);
        const char* title = "Create Account";
        ImVec2 tsz = ImGui::CalcTextSize(title);
        ImGui::SetCursorPosX(cx + (cardW - tsz.x) * 0.5f);
        ImGui::Text("%s", title);
        ImGui::SetWindowFontScale(1.0f);
        ImGui::PopStyleColor();

        ImGui::Dummy(ImVec2(0, 4));
        const char* sub = "Join TalkMe today";
        ImVec2 ssz = ImGui::CalcTextSize(sub);
        ImGui::SetCursorPosX(cx + (cardW - ssz.x) * 0.5f);
        ImGui::TextDisabled("%s", sub);

        ImGui::Dummy(ImVec2(0, 24));

        ImGui::SetCursorPosX(cx + pad);
        ImGui::PushStyleColor(ImGuiCol_Text, Styles::TextSecondary());
        ImGui::Text("Email");
        ImGui::PopStyleColor();
        ImGui::SetCursorPosX(cx + pad);
        ImGui::PushItemWidth(fieldW);
        ImGui::InputText("##email", emailBuf, 128);
        ImGui::PopItemWidth();

        ImGui::Dummy(ImVec2(0, 10));

        ImGui::SetCursorPosX(cx + pad);
        ImGui::PushStyleColor(ImGuiCol_Text, Styles::TextSecondary());
        ImGui::Text("Username");
        ImGui::PopStyleColor();
        ImGui::SetCursorPosX(cx + pad);
        ImGui::PushItemWidth(fieldW);
        ImGui::InputText("##username", usernameBuf, 128);
        ImGui::PopItemWidth();

        ImGui::Dummy(ImVec2(0, 10));

        ImGui::SetCursorPosX(cx + pad);
        ImGui::PushStyleColor(ImGuiCol_Text, Styles::TextSecondary());
        ImGui::Text("Password");
        ImGui::PopStyleColor();
        ImGui::SetCursorPosX(cx + pad);
        ImGui::PushItemWidth(fieldW);
        ImGui::InputText("##password", passwordBuf, 128, ImGuiInputTextFlags_Password);
        ImGui::PopItemWidth();

        ImGui::Dummy(ImVec2(0, 10));

        ImGui::SetCursorPosX(cx + pad);
        ImGui::PushStyleColor(ImGuiCol_Text, Styles::TextSecondary());
        ImGui::Text("Repeat Password");
        ImGui::PopStyleColor();
        ImGui::SetCursorPosX(cx + pad);
        ImGui::PushItemWidth(fieldW);
        bool enterPressed = ImGui::InputText("##password_rep", passwordRepeatBuf, 128,
            ImGuiInputTextFlags_Password | ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::PopItemWidth();

        ImGui::Dummy(ImVec2(0, 22));

        ImGui::SetCursorPosX(cx + pad);
        if (UI::AccentButton("Register", ImVec2(fieldW, 42)) || enterPressed) {
            if (strlen(emailBuf) > 0 && strlen(usernameBuf) > 0 && strlen(passwordBuf) > 0) {
                if (strcmp(passwordBuf, passwordRepeatBuf) == 0) {
                    std::string emailStr(emailBuf);
                    std::string userStr(usernameBuf);
                    std::string passStr(passwordBuf);
                    if (!netClient.IsConnected()) {
                        strcpy_s(statusMessage, 256, "Connecting...");
                        netClient.ConnectAsync(serverIP, serverPort,
                            [&netClient, statusMessage, emailStr, userStr, passStr](bool success) {
                                if (success) {
                                    strcpy_s(statusMessage, 256, "Registering...");
                                    netClient.Send(PacketType::Register_Request,
                                        PacketHandler::CreateRegisterPayload(emailStr, userStr, passStr));
                                } else {
                                    strcpy_s(statusMessage, 256, "Server offline.");
                                }
                            });
                    } else {
                        strcpy_s(statusMessage, 256, "Registering...");
                        netClient.Send(PacketType::Register_Request,
                            PacketHandler::CreateRegisterPayload(emailStr, userStr, passStr));
                    }
                } else {
                    strcpy_s(statusMessage, 256, "Passwords do not match.");
                }
            } else {
                strcpy_s(statusMessage, 256, "Please fill all fields.");
            }
        }

        ImGui::Dummy(ImVec2(0, 10));

        ImGui::SetCursorPosX(cx + pad);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0,0,0,0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0,0,0,0));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0,0,0,0));
        ImGui::PushStyleColor(ImGuiCol_Text, Styles::TextMuted());
        if (ImGui::Button("Already have an account? Login", ImVec2(fieldW, 28))) {
            currentState = AppState::Login;
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
