#pragma once
#include <cstdint>
#include <vector>
#include <string>

namespace TalkMe {
    const int SERVER_PORT = 5555;

    enum class PacketType : uint8_t {
        // --- AUTH ---
        Register_Request,
        Register_Success,
        Register_Failed,
        Login_Request,
        Login_Success,
        Login_Failed,

        // --- DISCORD ARCHITECTURE ---
        Create_Server_Request,
        Join_Server_Request,
        Server_List_Response,

        Get_Server_Content_Request,
        Server_Content_Response,
        Create_Channel_Request,

        Select_Text_Channel,
        Join_Voice_Channel,

        // --- DATA ---
        Message_Text,
        Message_History_Response,

        Voice_Data,                 // DEPRECATED (for backwards compatibility)
        Voice_Data_Opus,            // ✅ NEW: Opus-compressed voice data
        Voice_State_Update,         // Server sends list of users in voice chat

        // --- DELETION ---
        Delete_Channel_Request,
        Delete_Message_Request
    };

    struct PacketHeader {
        PacketType type;
        uint32_t size;
    };

    // Voice packet structure (embedded in payload)
    struct VoicePacketInfo {
        uint32_t sequenceNumber;
        uint32_t timestamp;  // Optional: for future jitter calculation
    };
}