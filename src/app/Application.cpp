#include "Application.h"
#include <tchar.h>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <algorithm>

#include "../../vendor/imgui.h"
#include "../../vendor/imgui_impl_win32.h"
#include "../../vendor/imgui_impl_dx11.h"
#include <nlohmann/json.hpp>

#include "../ui/Theme.h"
#include "../ui/Components.h"
#include "../ui/views/LoginView.h"
#include "../ui/views/RegisterView.h"
#include "../ui/views/SidebarView.h"
#include "../ui/views/ChatView.h"
#include "../network/PacketHandler.h"

using json = nlohmann::json;
static TalkMe::Application* g_AppInstance = nullptr;
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace TalkMe {

    void HandleResize(HWND hWnd, UINT width, UINT height) {
        if (g_AppInstance && g_AppInstance->GetDevice() && g_AppInstance->GetSwapChain()) {
            g_AppInstance->CleanupRenderTarget();
            g_AppInstance->GetSwapChain()->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
            g_AppInstance->CreateRenderTarget();
        }
    }

    LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) return true;
        switch (msg) {
        case WM_SIZE: if (wParam != SIZE_MINIMIZED) HandleResize(hWnd, (UINT)LOWORD(lParam), (UINT)HIWORD(lParam)); return 0;
        case WM_SYSCOMMAND: if ((wParam & 0xfff0) == SC_KEYMENU) return 0; break;
        case WM_DESTROY: ::PostQuitMessage(0); return 0;
        }
        return ::DefWindowProc(hWnd, msg, wParam, lParam);
    }

    std::string GetCurrentTimeStr() {
        auto t = std::time(nullptr); struct tm tm; localtime_s(&tm, &t);
        std::ostringstream oss; oss << std::put_time(&tm, "%H:%M"); return oss.str();
    }

    Application::Application(const std::string& title, int width, int height) : m_Title(title), m_Width(width), m_Height(height) { g_AppInstance = this; }
    Application::~Application() { g_AppInstance = nullptr; Cleanup(); }

    bool Application::Initialize() {
        if (!InitWindow() || !InitDirectX() || !InitImGui()) return false;

        m_AudioEngine.InitializeWithSequence([this](const std::vector<uint8_t>& opusData, uint32_t seqNum) {
            if (m_CurrentState == AppState::MainApp && m_NetClient.IsConnected() && m_ActiveVoiceChannelId != -1) {
                std::string payload = PacketHandler::CreateVoicePayloadOpus(m_CurrentUser.username, opusData, seqNum);
                m_NetClient.Send(PacketType::Voice_Data_Opus, payload);
            }
            });

        m_NetClient.SetVoiceCallback([this](const std::string& packetData) {
            auto parsed = PacketHandler::ParseVoicePayloadOpus(packetData);
            if (parsed.valid) {
                m_AudioEngine.PushIncomingAudioWithSequence(parsed.sender, parsed.opusData, parsed.sequenceNumber);
            }
            });

        UserSession session = ConfigManager::Get().LoadSession();
        if (session.isLoggedIn) {
            m_CurrentUser = session; m_CurrentState = AppState::MainApp;
            strcpy_s(m_EmailBuf, sizeof(m_EmailBuf), session.email.c_str());
            strcpy_s(m_PasswordBuf, sizeof(m_PasswordBuf), session.password.c_str());
            m_NetClient.ConnectAsync(m_ServerIP, m_ServerPort, [this, session](bool success) {
                if (success) m_NetClient.Send(PacketType::Login_Request, PacketHandler::CreateLoginPayload(session.email, session.password));
                });
        }
        return true;
    }

    void Application::Run() {
        bool done = false;
        while (!done) {
            MSG msg;
            while (::PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
                ::TranslateMessage(&msg);
                ::DispatchMessage(&msg);
                if (msg.message == WM_QUIT) done = true;
            }
            if (done) break;

            ProcessNetworkMessages();
            m_AudioEngine.Update();

            ImGui_ImplDX11_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();
            RenderUI();
            ImGui::Render();

            const float clear_color[4] = { 0.07f, 0.07f, 0.07f, 1.00f };
            m_pd3dDeviceContext->OMSetRenderTargets(1, &m_mainRenderTargetView, nullptr);
            m_pd3dDeviceContext->ClearRenderTargetView(m_mainRenderTargetView, clear_color);
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
            m_pSwapChain->Present(1, 0);
        }
    }

    void Application::ProcessNetworkMessages() {
        auto msgs = m_NetClient.FetchMessages();
        for (const auto& msg : msgs) {
            try {
                if (msg.type == PacketType::Register_Success) {
                    auto j_res = nlohmann::json::parse(msg.data);
                    std::string assigned = j_res.value("u", "");
                    if (!assigned.empty()) {
                        m_CurrentUser.username = assigned;
                        m_CurrentUser.email = m_EmailBuf;
                        ConfigManager::Get().SaveSession(m_EmailBuf, m_PasswordBuf);
                    }
                    m_CurrentUser.isLoggedIn = true; m_CurrentState = AppState::MainApp; continue;
                }
                if (msg.type == PacketType::Register_Failed) {
                    strcpy_s(m_StatusMessage, "Registration failed (Email already taken)."); continue;
                }
                if (msg.type == PacketType::Login_Success) {
                    auto j_res = nlohmann::json::parse(msg.data);
                    std::string assigned = j_res.value("u", "");
                    if (!assigned.empty()) {
                        m_CurrentUser.username = assigned;
                        m_CurrentUser.email = m_EmailBuf;
                        ConfigManager::Get().SaveSession(m_EmailBuf, m_PasswordBuf);
                    }
                    m_CurrentUser.isLoggedIn = true; m_CurrentState = AppState::MainApp; continue;
                }
                if (msg.type == PacketType::Login_Failed) {
                    strcpy_s(m_StatusMessage, sizeof(m_StatusMessage), "Invalid credentials."); continue;
                }

                auto j = nlohmann::json::parse(msg.data);
                if (msg.type == PacketType::Voice_State_Update) {
                    if (j["cid"] == m_ActiveVoiceChannelId) {
                        m_VoiceMembers.clear();
                        for (const auto& m : j["members"]) m_VoiceMembers.push_back(m);
                    }
                }
                else if (msg.type == PacketType::Server_List_Response) {
                    m_ServerList.clear();
                    for (const auto& item : j) m_ServerList.push_back({ item["id"], item["name"], item["code"] });
                    if (!m_ServerList.empty() && m_SelectedServerId == -1) {
                        m_SelectedServerId = m_ServerList[0].id;
                        m_NetClient.Send(PacketType::Get_Server_Content_Request, PacketHandler::GetServerContentPayload(m_SelectedServerId));
                    }
                }
                else if (msg.type == PacketType::Server_Content_Response) {
                    if (m_SelectedServerId != -1) {
                        auto it = std::find_if(m_ServerList.begin(), m_ServerList.end(), [this](const Server& s) { return s.id == m_SelectedServerId; });
                        if (it != m_ServerList.end()) {
                            it->channels.clear();
                            for (const auto& item : j) {
                                Channel ch; ch.id = item["id"]; ch.name = item["name"];
                                std::string typeStr = item.value("type", "text");
                                ch.type = (typeStr == "voice") ? ChannelType::Voice : ChannelType::Text;
                                it->channels.push_back(ch);
                            }
                        }
                    }
                }
                else if (msg.type == PacketType::Message_History_Response) {
                    if (!j.empty()) {
                        int cid = j[0]["cid"];
                        m_Messages.erase(std::remove_if(m_Messages.begin(), m_Messages.end(), [cid](const ChatMessage& m) { return m.channelId == cid; }), m_Messages.end());
                    }
                    for (const auto& item : j) m_Messages.push_back({ item.value("mid", 0), item["cid"], item["u"], item["msg"], item.value("time", "Old") });
                }
                else if (msg.type == PacketType::Message_Text) {
                    m_Messages.push_back({ j.value("mid", 0), j.value("cid", 0), j.value("u", "??"), j.value("msg", ""), GetCurrentTimeStr() });
                }
            }
            catch (...) {}
        }
    }

    void Application::RenderUI() {
        const ImGuiViewport* viewport = ImGui::GetMainViewport(); ImGui::SetNextWindowPos(viewport->WorkPos); ImGui::SetNextWindowSize(viewport->WorkSize);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f); ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f); ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar;
        ImGui::Begin("MainShell", nullptr, flags);
        if (m_CurrentState == AppState::Login) RenderLogin(); else if (m_CurrentState == AppState::Register) RenderRegister(); else if (m_CurrentState == AppState::MainApp) RenderMainApp();
        ImGui::End(); ImGui::PopStyleVar(3);
    }

    void Application::RenderLogin() { UI::Views::RenderLogin(m_NetClient, m_CurrentState, m_EmailBuf, m_PasswordBuf, m_StatusMessage, m_ServerIP, m_ServerPort); }
    void Application::RenderRegister() { UI::Views::RenderRegister(m_NetClient, m_CurrentState, m_EmailBuf, m_UsernameBuf, m_PasswordBuf, m_PasswordRepeatBuf, m_StatusMessage, m_ServerIP, m_ServerPort); }

    void Application::RenderMainApp() {
        UI::Views::RenderSidebar(m_NetClient, m_CurrentUser, m_CurrentState, m_ServerList, m_SelectedServerId, m_SelectedChannelId, m_ActiveVoiceChannelId, m_VoiceMembers, m_NewServerNameBuf, m_NewChannelNameBuf);
        if (m_SelectedServerId != -1) {
            auto it = std::find_if(m_ServerList.begin(), m_ServerList.end(), [this](const Server& s) { return s.id == m_SelectedServerId; });
            if (it != m_ServerList.end()) {
                UI::Views::RenderChannelView(m_NetClient, m_CurrentUser, *it, m_Messages, m_SelectedChannelId, m_ActiveVoiceChannelId, m_VoiceMembers, m_SpeakingTimers, m_ChatInputBuf);
            }
        }
    }

    bool Application::InitWindow() { WNDCLASSEX wc = { sizeof(WNDCLASSEX), CS_CLASSDC, WndProc, 0L, 0L, GetModuleHandle(nullptr), nullptr, nullptr, nullptr, nullptr, _T("TalkMeClass"), nullptr }; ::RegisterClassEx(&wc); m_Hwnd = ::CreateWindow(wc.lpszClassName, _T("TalkMe"), WS_OVERLAPPEDWINDOW, 0, 0, m_Width, m_Height, nullptr, nullptr, wc.hInstance, nullptr); if (!m_Hwnd) return false; ::ShowWindow(m_Hwnd, SW_MAXIMIZE); ::UpdateWindow(m_Hwnd); return true; }
    bool Application::InitDirectX() {
        DXGI_SWAP_CHAIN_DESC sd = {};
        sd.BufferCount = 2; sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.OutputWindow = m_Hwnd; sd.SampleDesc.Count = 1; sd.Windowed = TRUE; sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        D3D_FEATURE_LEVEL fl;
        HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd, &m_pSwapChain, &m_pd3dDevice, &fl, &m_pd3dDeviceContext);
        if (FAILED(hr)) return false;
        CreateRenderTarget();
        return true;
    }

    void Application::CreateRenderTarget() {
        if (!m_pSwapChain || !m_pd3dDevice) return;
        ID3D11Texture2D* pBackBuffer = nullptr;
        HRESULT hr = m_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
        if (FAILED(hr) || !pBackBuffer) return;
        hr = m_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &m_mainRenderTargetView);
        pBackBuffer->Release();
        if (FAILED(hr)) {
            if (m_mainRenderTargetView) { m_mainRenderTargetView->Release(); m_mainRenderTargetView = nullptr; }
        }
    }
    void Application::CleanupRenderTarget() { if (m_mainRenderTargetView) { m_mainRenderTargetView->Release(); m_mainRenderTargetView = nullptr; } }
    bool Application::InitImGui() { IMGUI_CHECKVERSION(); ImGui::CreateContext(); ImGuiIO& io = ImGui::GetIO(); io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard; ImFont* font = io.Fonts->AddFontFromFileTTF("c:\\Windows\\Fonts\\segoeui.ttf", 20.0f); if (!font) io.Fonts->AddFontDefault(); UI::SetupModernStyle(); ImGui_ImplWin32_Init(m_Hwnd); ImGui_ImplDX11_Init(m_pd3dDevice, m_pd3dDeviceContext); return true; }
    void Application::Cleanup() { m_AudioEngine.Shutdown(); ImGui_ImplDX11_Shutdown(); ImGui_ImplWin32_Shutdown(); ImGui::DestroyContext(); CleanupRenderTarget(); if (m_pSwapChain) m_pSwapChain->Release(); if (m_pd3dDeviceContext) m_pd3dDeviceContext->Release(); if (m_pd3dDevice) m_pd3dDevice->Release(); ::DestroyWindow(m_Hwnd); ::UnregisterClass(_T("TalkMeClass"), GetModuleHandle(nullptr)); }
}