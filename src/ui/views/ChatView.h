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
        bool* showSearch = nullptr
    );
}
