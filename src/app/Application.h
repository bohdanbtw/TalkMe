#pragma once
#include <string>
#include <vector>
#include <map>
#include <set>
#include <deque>
#include <chrono>
#include <atomic>
#include <mutex>
#include <functional>
#include <queue>
#include <thread>
#include "AppSounds.h"
#include "AppWindow.h"
#include "AppGraphics.h"
#include "VoiceSendPacer.h"
#include "../core/Logger.h"
#include "../network/NetworkClient.h"
#include "../network/VoiceTransport.h"
#include "../core/ConfigManager.h"
#include "../audio/AudioEngine.h"
#include "../overlay/GameOverlay.h"

namespace TalkMe {
    enum class AppState { Login, Login2FA, Register, MainApp };
    enum class ChannelType { Text, Voice };

    struct Channel {
        int id;
        std::string name;
        ChannelType type;
        std::string description;
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
        int replyToId = 0;
    };

    class Application {
    public:
        Application(const std::string& title, int width, int height);
        ~Application();
        bool Initialize();
        void Run();
        ID3D11Device* GetDevice() { return m_Graphics.GetDevice(); }
        IDXGISwapChain* GetSwapChain() { return m_Graphics.GetSwapChain(); }
        void OnWindowResize(UINT width, UINT height) { m_Graphics.OnResize(width, height); }

    private:
        void Cleanup();
        void ProcessNetworkMessages();
        void RenderUI();
        void RenderLogin();
        void RenderLogin2FA();   // 2FA challenge screen shown after Login_Requires_2FA
        void RenderRegister();
        void RenderMainApp();

    private:
        std::string m_ServerIP = "94.26.90.11";
        int m_ServerPort = 5555;
        std::string m_Title;
        int m_Width, m_Height;
        AppWindow m_Window;
        AppGraphics m_Graphics;
        void* m_NetworkWakeEvent = nullptr;  // HANDLE for MsgWaitForMultipleObjects / SetEvent
        void* m_QuitEvent = nullptr;         // HANDLE signaled in WndProc on WM_DESTROY so loop exits without relying on WM_QUIT
        bool m_CleanedUp = false;

        AppState m_CurrentState = AppState::Login;
        bool m_ValidatingSession = false;  // true while auto-login from session.dat is in flight
        int m_SplashFrames = 0;  // 0..2: draw splash (window hidden), then show window
        NetworkClient m_NetClient;
        AudioEngine m_AudioEngine;
        TalkMe::VoiceTransport m_VoiceTransport;
        bool m_UseUdpVoice = false;  // TCP-only voice: no UDP reordering, same path as bots
        bool m_LocalEcho = false; // when true, also locally loopback outgoing voice for testing
        UserSession m_CurrentUser;

        std::vector<Server> m_ServerList;
        std::vector<ChatMessage> m_Messages;

        int m_SelectedServerId = -1;
        int m_SelectedChannelId = -1;
        int m_ActiveVoiceChannelId = -1;
        int m_PrevActiveVoiceChannelId = -2;
        /// Thread-safe: voice callback only pushes when this != -1 (so we never play voice after leaving).
        std::atomic<int> m_ActiveVoiceChannelIdForVoice{ -1 };
        std::atomic<bool> m_ShuttingDown{ false };  // set in Cleanup() so background threads exit

        struct VoiceConfig {
            int keepaliveIntervalMs = 8000;
            int voiceStateRequestIntervalSec = 5;
            int jitterBufferTargetMs = 150;
            int jitterBufferMinMs = 80;
            int jitterBufferMaxMs = 300;
            int codecTargetKbps = 32;
            bool preferUdp = true;
        } m_VoiceConfig;
        std::string m_ServerVersion;

        std::vector<std::string> m_VoiceMembers;
        std::set<std::string> m_JoinSoundPlayedFor;  // members we already played join sound for in this channel
        std::map<std::string, float> m_SpeakingTimers;
        std::map<std::string, float> m_UserVolumes;  // client-side per-user volume (0 = mute, 1 = normal)

        struct UserVoiceState { bool muted = false; bool deafened = false; };
        std::map<std::string, UserVoiceState> m_UserMuteStates;  // per-user mute/deafen state from server

        std::map<std::string, float> m_TypingUsers;  // username -> timestamp of last typing indicator
        std::chrono::steady_clock::time_point m_LastTypingSentTime;

        std::set<std::string> m_OnlineUsers;  // set of currently online usernames
        int m_ReplyingToMessageId = 0;  // message ID being replied to (0 = not replying)

        struct ServerMember { std::string username; bool online = false; };
        std::vector<ServerMember> m_ServerMembers;  // members of currently selected server
        bool m_ShowMemberList = false;
        std::mutex m_RecentSpeakersMutex;
        std::vector<std::string> m_RecentSpeakers;  // drained each frame to update m_SpeakingTimers
        std::mutex m_VoiceDedupeMutex;
        std::set<std::pair<std::string, uint32_t>> m_VoiceDedupeSet;
        std::deque<std::pair<std::string, uint32_t>> m_VoiceDedupeQueue;
        static const size_t kMaxVoiceDedupe = 400;
        std::chrono::steady_clock::time_point m_LastVoiceStateRequestTime;
        std::chrono::steady_clock::time_point m_LastVoiceStatsLogTime;
        std::chrono::steady_clock::time_point m_LastVoiceDebugLogTime;
        std::chrono::steady_clock::time_point m_LastUdpHelloTime;
        std::chrono::steady_clock::time_point m_LastVoiceSendTime;
        std::chrono::steady_clock::time_point m_LastPingTime;
        bool m_VoiceRedundancyEnabled = false;  // set when loss > 40% to send each frame twice
        bool m_PendingVoiceRedundant = false;
        std::chrono::steady_clock::time_point m_LastVoiceRedundantTime;
        std::vector<uint8_t> m_LastVoiceRedundantPayload;
        std::vector<uint8_t> m_LastVoiceRedundantOpus;
        uint32_t m_LastVoiceRedundantTimestamp = 0;

        std::string m_DeviceId;
        char m_EmailBuf[128] = "";
        char m_2FACodeBuf[8] = "";
        char m_UsernameBuf[128] = "";
        char m_PasswordBuf[128] = "";
        char m_PasswordRepeatBuf[128] = "";
        char m_ChatInputBuf[1024] = "";
        char m_StatusMessage[256] = "";
        char m_NewServerNameBuf[64] = "";
        char m_NewChannelNameBuf[64] = "";
        bool m_ShowSettings = false;
        bool m_Is2FAEnabled = false;
        bool m_IsSettingUp2FA = false;
        bool m_IsDisabling2FA = false;
        char m_Disable2FACodeBuf[8] = "";
        std::string m_2FASecretStr;
        std::string m_2FAUriStr;
        char m_2FASetupCodeBuf[8] = "";
        char m_2FASetupStatusMessage[256] = "";

        bool m_SelfMuted = false;
        bool m_SelfDeafened = false;
        std::vector<int> m_KeyMuteMic;
        std::vector<int> m_KeyDeafen;
        int m_SettingsTab = 0;
        int m_SelectedInputDevice = -1;
        int m_SelectedOutputDevice = -1;
        int m_NoiseMode = 1;  // Default RNNoise
        bool m_TestMicEnabled = false;

        AppSounds m_Sounds;
        void PlayJoinSound() { m_Sounds.PlayJoin(); }
        void PlayLeaveSound() { m_Sounds.PlayLeave(); }

        static constexpr size_t kEchoLiveHistorySize = 60;
        static constexpr int kEchoLivePacketsPerSecond = 20;
        bool m_EchoLiveEnabled = false;
        std::deque<float> m_EchoLiveHistory;
        std::chrono::steady_clock::time_point m_EchoLiveBucketStartTime;
        uint32_t m_EchoLiveBucketStartSeq = 0;
        uint32_t m_EchoLiveNextSeq = 0;
        int m_EchoLiveRecvThisBucket = 0;
        void ToggleEchoLive();
        void EnsureEchoLiveEnabled();
        void UpdateEchoLive();
        void OnEchoResponse(uint32_t seq);

        void StartLinkProbe();
        void HandleProbeEcho(const std::vector<uint8_t>& pkt);

        std::atomic<bool> m_ProbeRunning{ false };
        std::mutex m_ProbeEchoMutex;
        std::map<uint32_t, int64_t> m_ProbeEchoTimes;

        VoiceSendPacer m_SendPacer;

        GameOverlay m_Overlay;
        bool m_OverlayEnabled = false;
        int m_OverlayCorner = 1;
        float m_OverlayOpacity = 0.85f;
        void UpdateOverlay();

        // FPS overlay member removed — use external profiling tools instead.
    };
}