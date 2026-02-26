#pragma once
#include <vector>
#include <string>
#include <functional>
#include "../../network/NetworkClient.h"
#include "../../app/Application.h"

namespace TalkMe::UI::Views {

    struct VoiceInfoData {
        std::string serverVersion;
        std::string voicePath;  // "UDP" or "TCP"
        float avgPingMs = 0;
        float lastPingMs = 0;
        float packetLossPercent = 0;
        std::vector<float> pingHistory;
        int packetsReceived = 0;
        int packetsLost = 0;
        int currentBufferMs = 0;
        int encoderBitrateKbps = 32;
        bool echoLiveEnabled = false;
        std::vector<float> echoLiveHistory;
        float currentEchoLossPct = 0.f;
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
        const VoiceInfoData& voiceInfo = {},
        std::function<void()> onToggleEchoLive = nullptr,
        std::function<void()> onInfPopupOpened = nullptr,
        std::map<int, int>* unreadCounts = nullptr
    );
}
