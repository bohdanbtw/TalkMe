#pragma once
#include <string>
#include <vector>
#include <map>
#include <d3d11.h>
#include "../network/NetworkClient.h" 
#include "../core/ConfigManager.h"
#include "../audio/AudioEngine.h"

namespace TalkMe {
    enum class AppState { Login, Register, MainApp };
    enum class ChannelType { Text, Voice };

    struct Channel {
        int id;
        std::string name;
        ChannelType type;
    };

    struct Server {
        int id;
        std::string name;
        std::string inviteCode;
        std::vector<Channel> channels;
    };

    struct ChatMessage {
        int id;
        int channelId;
        std::string sender;
        std::string content;
        std::string timestamp;
    };

    class Application {
    public:
        Application(const std::string& title, int width, int height);
        ~Application();
        bool Initialize();
        void Run();
        ID3D11Device* GetDevice() { return m_pd3dDevice; }
        IDXGISwapChain* GetSwapChain() { return m_pSwapChain; }
        void CreateRenderTarget();
        void CleanupRenderTarget();

    private:
        bool InitWindow();
        bool InitDirectX();
        bool InitImGui();
        void Cleanup();
        void ProcessNetworkMessages();
        void RenderUI();
        void RenderLogin();
        void RenderRegister();
        void RenderMainApp();

    private:
        std::string m_ServerIP = "94.26.90.11";
        int m_ServerPort = 5555;
        std::string m_Title;
        int m_Width, m_Height;
        HWND m_Hwnd = nullptr;
        ID3D11Device* m_pd3dDevice = nullptr;
        ID3D11DeviceContext* m_pd3dDeviceContext = nullptr;
        IDXGISwapChain* m_pSwapChain = nullptr;
        ID3D11RenderTargetView* m_mainRenderTargetView = nullptr;

        AppState m_CurrentState = AppState::Login;
        NetworkClient m_NetClient;
        AudioEngine m_AudioEngine;
        UserSession m_CurrentUser;

        std::vector<Server> m_ServerList;
        std::vector<ChatMessage> m_Messages;

        int m_SelectedServerId = -1;
        int m_SelectedChannelId = -1;
        int m_ActiveVoiceChannelId = -1;

        std::vector<std::string> m_VoiceMembers;
        std::map<std::string, float> m_SpeakingTimers;

        char m_EmailBuf[128] = ""; // NEW
        char m_UsernameBuf[128] = "";
        char m_PasswordBuf[128] = "";
        char m_PasswordRepeatBuf[128] = "";
        char m_ChatInputBuf[1024] = "";
        char m_StatusMessage[256] = "";
        char m_NewServerNameBuf[64] = "";
        char m_NewChannelNameBuf[64] = "";
    };
}