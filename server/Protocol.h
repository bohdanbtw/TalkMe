#pragma once
#include <cstdint>
#include <vector>
#include <string>

namespace TalkMe {
    // Portable 32-bit endianness conversion (Network Byte Order = Big-Endian)
    inline uint32_t Swap32(uint32_t v) {
        return ((v & 0xFFU) << 24) | ((v & 0xFF00U) << 8) |
            ((v & 0xFF0000U) >> 8) | ((v & 0xFF000000U) >> 24);
    }

    inline uint32_t HostToNet32(uint32_t v) {
        uint32_t test = 1;
        return (*reinterpret_cast<uint8_t*>(&test) == 1) ? Swap32(v) : v;
    }

    inline uint32_t NetToHost32(uint32_t v) {
        return HostToNet32(v);
    }

    constexpr uint16_t SERVER_PORT = 5555;
    constexpr uint16_t VOICE_PORT = 5556;

    enum class PacketType : uint8_t {
        // --- AUTH ---
        Register_Request,
        Register_Success,
        Register_Failed,
        Login_Request,
        Login_Success,
        Login_Failed,

        // --- SERVER ARCHITECTURE ---
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

        Voice_Data,                 // DEPRECATED
        Voice_Data_Opus,
        Voice_State_Update,
        Voice_Config,
        Voice_Stats_Report,

        // --- PHASE 1: RTCP-LITE TELEMETRY ---
        Receiver_Report,            // Client -> Server: Downstream quality metrics
        Sender_Report,              // Server -> Client: Upstream quality metrics

        // --- DELETION ---
        Delete_Channel_Request,
        Delete_Message_Request,

        // --- DIAGNOSTIC ---
        Echo_Request,
        Echo_Response
    };

#pragma pack(push, 1)
    struct PacketHeader {
        PacketType type;
        uint32_t size;

        void ToNetwork() { size = HostToNet32(size); }
        void ToHost() { size = NetToHost32(size); }
    };

    struct VoicePacketInfo {
        uint32_t sequenceNumber;
        uint32_t timestamp;

        void ToNetwork() {
            sequenceNumber = HostToNet32(sequenceNumber);
            timestamp = HostToNet32(timestamp);
        }
        void ToHost() {
            sequenceNumber = NetToHost32(sequenceNumber);
            timestamp = NetToHost32(timestamp);
        }
    };

    // Sent by the receiver to inform the server about downstream quality
    struct ReceiverReportPayload {
        uint32_t highestSequenceReceived;
        uint32_t packetsLost;
        uint32_t jitterMs;
        uint8_t fractionLost; // 0-255 scale representing 0.0 to 100.0%

        void ToNetwork() {
            highestSequenceReceived = HostToNet32(highestSequenceReceived);
            packetsLost = HostToNet32(packetsLost);
            jitterMs = HostToNet32(jitterMs);
        }
        void ToHost() {
            highestSequenceReceived = NetToHost32(highestSequenceReceived);
            packetsLost = NetToHost32(packetsLost);
            jitterMs = NetToHost32(jitterMs);
        }
    };

    // Sent by the server to the sender to instruct bitrate adaptation
    struct SenderReportPayload {
        uint32_t suggestedBitrateKbps;
        uint8_t networkState; // 0 = Excellent, 1 = Good, 2 = Congested, 3 = Critical

        void ToNetwork() {
            suggestedBitrateKbps = HostToNet32(suggestedBitrateKbps);
        }
        void ToHost() {
            suggestedBitrateKbps = NetToHost32(suggestedBitrateKbps);
        }
    };
#pragma pack(pop)
}