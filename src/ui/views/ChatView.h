#pragma once
#include <vector>
#include <string>
#include <map>
#include <functional>
#include "../../network/NetworkClient.h"
#include "../../app/Application.h"

namespace TalkMe::UI::Views {

    using UserVoiceState = TalkMe::UserVoiceState;

    void RenderChannelView(
        NetworkClient& netClient,
        UserSession& currentUser,
        const Server& currentServer,
        std::vector<ChatMessage>& messages,
        int& selectedChannelId,
        int& activeVoiceChannelId,
        std::vector<std::string>& voiceMembers,
        std::map<std::string, float>& speakingTimers,
        std::map<std::string, float>& userVolumes,
        std::function<void(const std::string&, float)> setUserVolume,
        char* chatInputBuf,
        bool selfMuted = false,
        bool selfDeafened = false,
        const std::map<std::string, UserVoiceState>* userMuteStates = nullptr,
        const std::map<std::string, float>* typingUsers = nullptr,
        std::function<void()> onUserTyping = nullptr,
        int* replyingToMessageId = nullptr,
        const std::vector<std::pair<std::string, bool>>* serverMembers = nullptr,
        bool* showMemberList = nullptr,
        char* searchBuf = nullptr,
        bool* showSearch = nullptr,
        std::function<void(int fps, int quality)> onStartScreenShare = nullptr,
        std::function<void()> onStopScreenShare = nullptr,
        bool isScreenSharing = false,
        bool someoneIsSharing = false,
        void* screenShareTexture = nullptr,
        int screenShareW = 0,
        int screenShareH = 0,
        const std::vector<std::string>* activeStreamers = nullptr,
        std::string* viewingStream = nullptr,
        bool* streamMaximized = nullptr,
        bool* showGifPicker = nullptr,
        std::function<void(float w, float h)> renderGifPanel = nullptr,
        const std::string* mediaBaseUrl = nullptr,
        std::function<void(std::vector<uint8_t>, std::string)>* onImageUpload = nullptr,
        std::function<void(const std::string&)>* requestAttachment = nullptr,
        std::function<const TalkMe::AttachmentDisplay*(const std::string&)>* getAttachmentDisplay = nullptr,
        std::function<void(const std::string&)>* onAttachmentClick = nullptr,
        std::function<void()> requestOlderMessages = nullptr,
        const bool* loadingOlder = nullptr,
        std::function<void(int channelId, int fullyVisibleMid)> onReadAnchorAdvanced = nullptr
    );
}
