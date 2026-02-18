#pragma once
#include <vector>
#include <string>
#include "../../network/NetworkClient.h"
#include "../../app/Application.h"

namespace TalkMe::UI::Views {

    struct VoiceInfoData {
        float avgPingMs = 0;
        float packetLossPercent = 0;
        int packetsReceived = 0;
        int packetsLost = 0;
        int currentBufferMs = 0;
        int encoderBitrateKbps = 32;
    };

    void RenderSidebar(
        NetworkClient& netClient,
        UserSession& currentUser,
        AppState& currentState,
        std::vector<Server>& servers,
        int& selectedServerId,
        int& selectedChannelId,
        int& activeVoiceChannelId,
        std::vector<std::string>& voiceMembers,
        char* newServerNameBuf,
        char* newChannelNameBuf,
        bool& showSettings,
        bool& selfMuted,
        bool& selfDeafened,
        const VoiceInfoData& voiceInfo = {}
    );
}
