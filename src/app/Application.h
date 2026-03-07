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
#include <unordered_set>
#include <unordered_map>
#include <condition_variable>
#include "AppSounds.h"
#include "AppWindow.h"
#include "AppGraphics.h"
#include "VoiceSendPacer.h"
#include "../core/Logger.h"
#include "../network/NetworkClient.h"
#include "../network/VoiceTransport.h"
#include "../core/ConfigManager.h"
#include "../audio/AudioEngine.h"
#include "../audio/Soundboard.h"
#include "../overlay/GameOverlay.h"
#include "../game/Chess.h"
#include "../screen/ScreenCapture.h"
#include "../screen/DXGICapture.h"
#include "../screen/H264Encoder.h"
#include "../screen/AudioLoopback.h"
#include "../screen/WebcamCapture.h"
#include "../network/KlipyGifProvider.h"
#include "../ui/GifPickerPanel.h"
#include "../core/Secrets.h"
#include <memory>
#include "../game/Racing.h"
#include "../game/FlappyBird.h"
#include "../game/TicTacToe.h"
#include "../storage/MessageCacheDb.h"

namespace TalkMe {
    enum class AppState { Login, Login2FA, Register, MainApp };
    enum class ChannelType { Text, Voice, Cinema, Announcement };

    struct Channel {
        int id;
        std::string name;
        ChannelType type;
        std::string description;
        int userLimit = 0;
        int memberCount = -1;  // -1 = unknown; server may send in Server_Content_Response
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
        bool pinned = false;
        std::map<std::string, std::vector<std::string>> reactions;
        std::string attachmentId;  // server attachment id; display via media URL
    };

    struct ChannelMessageState {
        std::vector<ChatMessage> messages;
        int lastReadMessageId = 0;  // messages with id > this are unread when channel not focused
        int oldestLoadedMid = 0;
        int newestLoadedMid = 0;
        bool hasMoreOlder = true;
        bool hasMoreNewer = true;
        bool loadingOlder = false;
        bool loadingNewer = false;
        bool loadingLatest = false;
        bool initialPageRequested = false;  // avoid re-requesting when channel is empty
    };

    struct UserVoiceState { bool muted = false; bool deafened = false; };

    // Attachment fetched via Media_Request (TCP, no port 5557 needed)
    struct AttachmentDisplay {
        bool ready = false;
        bool failed = false;
        std::string textureId;
        int width = 0;
        int height = 0;
    };

    class Application {
    public:
        /// restoreX/restoreY >= 0: move window to that position after Create (used after invisible relaunch).
        Application(const std::string& title, int width, int height, int restoreX = -1, int restoreY = -1);
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

        void StartScreenShareProcess(int fps, int quality, int width, int height);
        void StopScreenShareProcess();
        int GetEffectiveShareFps() const;
        static void UpdateFpsWindow(std::chrono::steady_clock::time_point& windowStart,
                                    uint32_t& framesInWindow,
                                    float& outFps,
                                    const std::chrono::steady_clock::time_point& now);
        void RecordSharePerfSample(const char* metric, double ms);
        void FlushSharePerfIfNeeded();

    private:
        std::string m_ServerIP;       // from secret/secrets "server_ip", or set in Settings
        int m_ServerPort = 5555;
        std::string m_MediaBaseUrl;   // e.g. http://<server_ip>:5557 for GET /media/<id>
        std::string m_Title;
        int m_Width, m_Height;
        int m_RestoreX = -1, m_RestoreY = -1;
        AppWindow m_Window;
        AppGraphics m_Graphics;
        void* m_NetworkWakeEvent = nullptr;  // HANDLE for MsgWaitForMultipleObjects / SetEvent
        void* m_QuitEvent = nullptr;         // HANDLE signaled in WndProc on WM_DESTROY so loop exits without relying on WM_QUIT
        bool m_CleanedUp = false;

        AppState m_CurrentState = AppState::Login;
        bool m_ValidatingSession = false;  // true while auto-login from session.dat is in flight
        std::atomic<bool> m_LoginConnectInProgress{ false };  // true while manual Sign In/Register connection is in progress
        int m_SplashFrames = 0;  // 0..2: draw splash (window hidden), then show window
        NetworkClient m_NetClient;
        AudioEngine m_AudioEngine;
        TalkMe::VoiceTransport m_VoiceTransport;
        bool m_UseUdpVoice = false;  // TCP-only voice: no UDP reordering, same path as bots
        bool m_LocalEcho = false; // when true, also locally loopback outgoing voice for testing
        UserSession m_CurrentUser;

        std::vector<Server> m_ServerList;
        std::map<int, ChannelMessageState> m_ChannelStates;
        MessageCacheDb m_MessageCacheDb;
        std::chrono::steady_clock::time_point m_LastReadAnchorPersistTime{};

        std::vector<ChatMessage>& GetChannelMessages(int channelId);

        int m_SelectedServerId = -1;
        int m_SelectedChannelId = -1;
        int m_PrevSelectedChannelId = -1;
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

        std::map<std::string, UserVoiceState> m_UserMuteStates;

        std::map<std::string, float> m_TypingUsers;  // username -> timestamp of last typing indicator
        std::chrono::steady_clock::time_point m_LastTypingSentTime;

        std::set<std::string> m_OnlineUsers;
        std::map<std::string, std::string> m_UserStatuses;  // username -> custom status text
        int m_ReplyingToMessageId = 0;  // message ID being replied to (0 = not replying)

        struct ServerMember { std::string username; bool online = false; };
        std::vector<ServerMember> m_ServerMembers;
        bool m_ShowMemberList = false;

        std::map<int, int> m_UnreadCounts;  // channelId -> unread message count
        char m_SearchBuf[256] = "";
        bool m_ShowSearch = false;
        bool m_ShowShortcuts = false;
        bool m_ShowGifDebug = false;
        bool m_ShowGifPicker = false;
        std::unique_ptr<UI::GifPickerPanel> m_GifPickerPanel;
        std::function<void(float, float)> m_GifPanelRender;
        int m_EmotionsPanelTab = 2;       // 0=Emoji, 1=Stickers, 2=GIFs
        char m_StatusBuf[128] = "";
        bool m_GameMode = false;          // When true: chat-only, no images/GIFs/screen share, max performance
        int m_TargetFps = 60;             // App render target in FPS (10..1000), configurable in Settings > Performance
        std::chrono::steady_clock::time_point m_LastRenderTime;  // For FPS cap in Run()
        bool m_PendingRelaunch = false;   // When true: next frame write rect, spawn --relaunch-instead, exit

        struct NotificationSettings {
            float volume = 0.8f;
            bool muteMentions = false;
            bool muteMessages = false;
            bool muteJoinLeave = false;
        } m_NotifSettings;

        std::unordered_map<std::string, std::string> m_AvatarCache;
        std::unordered_set<std::string> m_AvatarRequested;

        // Pending image upload: after File_Transfer_Request we wait for upload_approved, then send chunks.
        std::vector<uint8_t> m_PendingUploadData;
        std::string m_PendingUploadFilename;
        int m_PendingUploadChannelId = -1;
        /// When sending an attached image, optional message text and reply_to to include when upload completes.
        std::string m_PendingMessageText;
        int m_PendingReplyToId = 0;

        /// Image attached to compose bar (not sent until user clicks Send). Cleared when sent or removed.
        std::vector<uint8_t> m_PendingAttachedImage;
        std::string m_PendingAttachedImageFilename;

        /// File paths dropped on the window; processed when channel view is shown.
        std::vector<std::string> m_PendingDroppedFiles;
        /// When true, next frame will render even if window is in background (e.g. after file drop).
        bool m_ForceRenderNextFrame = false;

        std::function<void(std::vector<uint8_t>, std::string)> m_OnImageUpload;
        std::function<void(std::vector<uint8_t>, std::string)> m_OnAttachImage;
        std::function<void(const std::string&, int)> m_OnSendWithAttachedImage;
        std::function<void(const std::string&)> m_RequestAttachmentFn;
        std::function<const AttachmentDisplay*(const std::string&)> m_GetAttachmentDisplayFn;
        std::function<void(const std::string&)> m_OnAttachmentClickFn;
        std::function<void(const std::string&)> m_OnAttachmentTextureEvictedFn;

        // TCP attachment fetch (Media_Request/Media_Response) so attachments work without port 5557
        std::unordered_map<std::string, AttachmentDisplay> m_AttachmentCache;
        std::unordered_set<std::string> m_AttachmentRequested;
        std::unordered_map<std::string, std::vector<uint8_t>> m_AttachmentFileData;  // raw file bytes for Save
        /// Decoded RGBA uploads queued from Media_Response; processed on main thread in render block after SetDevice.
        struct PendingAttachmentUpload { std::string id; std::vector<uint8_t> rgba; int w = 0; int h = 0; };
        std::vector<PendingAttachmentUpload> m_PendingAttachmentUploads;
        std::mutex m_PendingAttachmentUploadsMutex;

        // In-app image viewer (Discord-style popup)
        std::string m_ViewingAttachmentId;
        float m_AttachmentViewerZoom = 1.0f;
        bool m_AttachmentViewerOpen = false;
        double m_AttachmentViewerOpenTime = 0.0;  // time when viewer was opened (to ignore opening click)

        void StartImageUpload(std::vector<uint8_t> data, std::string filename, int channelId);
        void RequestAttachment(const std::string& id);
        const AttachmentDisplay* GetAttachmentDisplay(const std::string& id) const;
        const std::vector<uint8_t>* GetAttachmentFileData(const std::string& id) const;
        void RenderAttachmentViewer();

        std::string GetStateCachePath() const;
        std::string GetMessageCacheDbPath() const;
        void LoadStateCache();
        void SaveStateCache();
        void RequestRelaunch();  // Write window rect, spawn exe with --relaunch-instead, ExitProcess

        struct PollOption { std::string text; int votes = 0; bool iVoted = false; };
        struct Poll { int id = 0; int channelId = 0; std::string question; std::string creator; std::vector<PollOption> options; };
        std::vector<Poll> m_Polls;
        bool m_ShowPollCreator = false;

        bool m_CompactMode = false;
        float m_FontScale = 1.0f;
        std::unordered_set<std::string> m_BlockedUsers;
        bool m_IsInvisible = false;
        Soundboard m_Soundboard;
        bool m_ShowSoundboard = false;

        int m_VoiceEffect = 0; // 0=none, 1=deep, 2=high, 3=robot
        int m_DisappearingDuration = 0; // 0=off, seconds for auto-delete

        struct ChessGameState {
            bool active = false;
            std::string opponent;
            bool myTurn = false;
            bool isWhite = true;
            int selectedRow = -1, selectedCol = -1;
        } m_ChessUI;
        ChessEngine m_ChessEngine;
        RacingGame m_Racing;
        FlappyBird m_FlappyBird;
        TicTacToe m_TicTacToe;
        ScreenCapture m_ScreenCapture;
        DXGICapture m_DXGICapture;
        H264Decoder m_H264Decoder;
        AudioLoopback m_AudioLoopback;
        WebcamCapture m_WebcamCapture;

        std::mutex m_ScreenShareStreamMutex;  // protects m_ScreenShare.activeStreams (capture callback + main/network threads)

        struct CinemaQueueItem {
            std::string url;
            std::string title;
            std::string addedBy;
        };
        struct CinemaState {
            bool active = false;
            int channelId = -1;
            std::string currentUrl;
            std::string currentTitle;
            bool playing = false;
            float currentTime = 0.0f;
            float duration = 0.0f;
            std::string host;
            std::vector<CinemaQueueItem> queue;
        } m_Cinema;
        char m_CinemaUrlBuf[512] = "";
        char m_CinemaTitleBuf[128] = "";

        struct StreamInfo {
            std::string username;
            std::vector<uint8_t> lastFrameData;
            int frameWidth = 0;
            int frameHeight = 0;
            bool frameUpdated = false;
            // Incoming encoded stream FPS meter (capture/network arrival rate).
            std::chrono::steady_clock::time_point streamFpsWindowStart{};
            uint32_t streamFramesInWindow = 0;
            float streamFps = 0.0f;
            // Target FPS throttle for preview texture uploads.
            std::chrono::steady_clock::time_point lastPreviewUpdateTime{};
            // Preview FPS meter (counts actual texture uploads in Render loop).
            std::chrono::steady_clock::time_point previewFpsWindowStart{};
            uint32_t previewFramesInWindow = 0;
            float previewFps = 0.0f;
        };

        struct ScreenShareState {
            int fps = 30;
            int quality = 75;
            bool iAmSharing = false;
            std::map<std::string, StreamInfo> activeStreams;
            std::string viewingStream;
            bool maximized = false;
        } m_ScreenShare;

        bool m_SharePerfEnabled = false;
        bool m_AdaptiveSharePreview = true;
        struct SharePerfMetric {
            double totalMs = 0.0;
            double maxMs = 0.0;
            uint32_t count = 0;
            std::deque<double> samples;
        };
        std::mutex m_SharePerfMutex;
        std::unordered_map<std::string, SharePerfMetric> m_SharePerfMetrics;
        std::chrono::steady_clock::time_point m_SharePerfLastFlush{};

        struct FriendEntry { std::string username; std::string status; std::string direction; };
        std::vector<FriendEntry> m_Friends;
        bool m_ShowFriendList = false;
        char m_FriendSearchBuf[128] = "";

        struct CallState { std::string otherUser; std::string state; };
        CallState m_CurrentCall;

        struct DirectMessage { int id; std::string sender; std::string content; std::string timestamp; };
        std::vector<DirectMessage> m_DirectMessages;
        std::string m_ActiveDMUser;
        char m_DMInputBuf[1024] = "";
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
        std::string m_AttachedGifUrl;  // GIF/media attached to message bar (sent with next Send)
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