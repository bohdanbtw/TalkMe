#pragma once
#include <vector>
#include <string>
#include "../../network/NetworkClient.h"
#include "../../app/Application.h" 

namespace TalkMe::UI::Views {
    void RenderSidebar(
        NetworkClient& netClient,
        UserSession& currentUser,
        AppState& currentState,
        std::vector<Server>& servers,
        int& selectedServerId,
        int& selectedChannelId, // Unified ID
        int& activeVoiceChannelId,
        std::vector<std::string>& voiceMembers, // Added to reset list on manual disconnect
        char* newServerNameBuf,
        char* newChannelNameBuf
    );
}