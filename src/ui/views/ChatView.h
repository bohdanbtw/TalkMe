#pragma once
#include <vector>
#include <string>
#include <map>
#include "../../network/NetworkClient.h"
#include "../../app/Application.h" 

namespace TalkMe::UI::Views {
    void RenderChannelView(
        NetworkClient& netClient,
        UserSession& currentUser,
        const Server& currentServer,
        std::vector<ChatMessage>& messages,
        int& selectedChannelId,
        int& activeVoiceChannelId,
        std::vector<std::string>& voiceMembers,
        std::map<std::string, float>& speakingTimers,
        char* chatInputBuf
    );
}