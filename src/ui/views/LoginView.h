#pragma once
#include <string>
#include "../../network/NetworkClient.h"
#include "../../app/Application.h"

namespace TalkMe::UI::Views {
    void RenderLogin(NetworkClient& netClient, AppState& currentState, char* emailBuf, char* passwordBuf, char* statusMessage, const std::string& serverIP, int serverPort);
}