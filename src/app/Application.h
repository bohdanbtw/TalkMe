#pragma once
#include <string>
#include <vector>
#include <map>
#include <chrono>
#include <atomic>
#include <mutex>
#include <d3d11.h>
#include "../core/Logger.h"
#include "../network/NetworkClient.h" 
#include "../network/VoiceTransport.h"
#include "../core/ConfigManager.h"
#include "../audio/AudioEngine.h"
#include "../overlay/GameOverlay.h"

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
        TalkMe::VoiceTransport m_VoiceTransport;
        bool m_UseUdpVoice = false;
        bool m_LocalEcho = false; // when true, also locally loopback outgoing voice for testing
        UserSession m_CurrentUser;

        std::vector<Server> m_ServerList;
        std::vector<ChatMessage> m_Messages;

        int m_SelectedServerId = -1;
        int m_SelectedChannelId = -1;
        int m_ActiveVoiceChannelId = -1;
        int m_PrevActiveVoiceChannelId = -2;
        /// Thread-safe: voice callback only pushes when this != -1 (so we never play voice after leaving).
        std::atomic<int> m_ActiveVoiceChannelIdForVoice{-1};

        struct VoiceConfig {
            int keepaliveIntervalMs = 8000;
            int voiceStateRequestIntervalSec = 5;
            int jitterBufferTargetMs = 150;
            int jitterBufferMinMs = 80;
            int jitterBufferMaxMs = 300;
        } m_VoiceConfig;

        std::vector<std::string> m_VoiceMembers;
        std::map<std::string, float> m_SpeakingTimers;
        std::map<std::string, float> m_UserVolumes;  // client-side per-user volume (0 = mute, 1 = normal)
        std::mutex m_RecentSpeakersMutex;
        std::vector<std::string> m_RecentSpeakers;  // drained each frame to update m_SpeakingTimers
        std::chrono::steady_clock::time_point m_LastVoiceStateRequestTime;
        std::chrono::steady_clock::time_point m_LastVoiceStatsLogTime;

        char m_EmailBuf[128] = ""; // NEW
        char m_UsernameBuf[128] = "";
        char m_PasswordBuf[128] = "";
        char m_PasswordRepeatBuf[128] = "";
        char m_ChatInputBuf[1024] = "";
        char m_StatusMessage[256] = "";
        char m_NewServerNameBuf[64] = "";
        char m_NewChannelNameBuf[64] = "";
        bool m_ShowSettings = false;

        bool m_SelfMuted = false;
        bool m_SelfDeafened = false;
        std::vector<int> m_KeyMuteMic;
        std::vector<int> m_KeyDeafen;
        int m_SettingsTab = 0;
        int m_SelectedInputDevice = -1;
        int m_SelectedOutputDevice = -1;

        std::vector<uint8_t> m_JoinSound;
        std::vector<uint8_t> m_LeaveSound;
        void GenerateSounds();
        void PlayJoinSound();
        void PlayLeaveSound();

        GameOverlay m_Overlay;
        bool m_OverlayEnabled = false;
        int m_OverlayCorner = 1;
        float m_OverlayOpacity = 0.85f;
        void UpdateOverlay();
    };
}