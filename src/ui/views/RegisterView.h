#pragma once
#include <string>
#include "../../network/NetworkClient.h"
#include "../../app/Application.h"

namespace TalkMe::UI::Views {
    void RenderRegister(NetworkClient& netClient, AppState& currentState, char* emailBuf, char* usernameBuf, char* passwordBuf, char* passwordRepeatBuf, char* statusMessage, const std::string& serverIP, int serverPort);
}