#pragma once
#include <cstdint>
#include <cstring>
#include <vector>
#include <string>

namespace TalkMe {

    // ---------------------------------------------------------------------------
    // Endianness detection — C++17 compatible.
    //
    // std::memcpy into uint8_t replaces the reinterpret_cast<const uint8_t*>
    // idiom. Reading any object through unsigned char* is permitted by the
    // aliasing rules, but memcpy is more explicit and does not trigger
    // static-analyser warnings about pointer-cast aliasing.
    // ---------------------------------------------------------------------------
    namespace detail {
        inline bool IsLittleEndian() noexcept {
            static constexpr uint32_t kOne = 1u;
            uint8_t b;
            std::memcpy(&b, &kOne, 1);
            return b == 1u;
        }
    }

    inline uint32_t Swap32(uint32_t v) noexcept {
        return ((v & 0xFFU) << 24) | ((v & 0xFF00U) << 8)
            | ((v & 0xFF0000U) >> 8) | ((v & 0xFF000000U) >> 24);
    }

    inline uint64_t Swap64(uint64_t v) noexcept {
        return ((v & 0xFFULL) << 56) | ((v & 0xFF00ULL) << 40)
            | ((v & 0xFF0000ULL) << 24) | ((v & 0xFF000000ULL) << 8)
            | ((v & 0xFF00000000ULL) >> 8) | ((v & 0xFF0000000000ULL) >> 24)
            | ((v & 0xFF000000000000ULL) >> 40) | ((v & 0xFF00000000000000ULL) >> 56);
    }

    inline uint32_t HostToNet32(uint32_t v) noexcept {
        return detail::IsLittleEndian() ? Swap32(v) : v;
    }
    inline uint32_t NetToHost32(uint32_t v) noexcept { return HostToNet32(v); }

    inline uint64_t HostToNet64(uint64_t v) noexcept {
        return detail::IsLittleEndian() ? Swap64(v) : v;
    }
    inline uint64_t NetToHost64(uint64_t v) noexcept { return HostToNet64(v); }

    // Append a 4-byte or 8-byte value in big-endian order.
    inline void AppendBE(std::vector<uint8_t>& out, uint32_t value) {
        uint32_t net = HostToNet32(value);
        uint8_t  tmp[4];
        std::memcpy(tmp, &net, 4);
        for (uint8_t b : tmp) out.push_back(b);
    }

    inline void AppendBE(std::vector<uint8_t>& out, uint64_t value) {
        uint64_t net = HostToNet64(value);
        uint8_t  tmp[8];
        std::memcpy(tmp, &net, 8);
        for (uint8_t b : tmp) out.push_back(b);
    }

    // Read a 64-bit big-endian value from a raw byte pointer.
    // The shift loop produces the host-order result directly — no second swap needed.
    inline uint64_t ReadU64BE(const uint8_t* p) noexcept {
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i) v = (v << 8) | static_cast<uint64_t>(p[i]);
        return v;
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
        Login_Requires_2FA,
        Validate_Session_Request,
        Validate_Session_Response,
        Generate_2FA_Secret_Request,
        Generate_2FA_Secret_Response,
        Verify_2FA_Setup_Request,
        Submit_2FA_Login_Request,
        Disable_2FA_Request,
        Disable_2FA_Response,

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
        Message_Edit,
        Message_Delete,
        Message_History_Response,
        File_Transfer_Request,
        File_Transfer_Chunk,
        File_Transfer_Complete,

        Voice_Data,          // DEPRECATED
        Voice_Data_Opus,
        Voice_State_Update,
        Voice_Config,
        Voice_Stats_Report,

        // --- PHASE 1: RTCP-LITE TELEMETRY ---
        Receiver_Report,     // Client -> Server: downstream quality metrics
        Sender_Report,       // Server -> Client: upstream quality metrics + network state

        // --- DELETION / EDIT ---
        Delete_Channel_Request,
        Delete_Message_Request,
        Edit_Message_Request,
        Pin_Message_Request,

        // --- PRESENCE ---
        Voice_Mute_State,    // Client -> Server -> All: user muted/deafened state change
        Typing_Indicator,    // Client -> Server -> Channel: user is typing
        Presence_Update,     // Server -> Client: user online/offline status change
        Member_List_Request, // Client -> Server: request member list for a server
        Member_List_Response,// Server -> Client: list of members in a server with online status

        // --- DIAGNOSTIC ---
        Echo_Request,
        Echo_Response
    };

    enum Permissions : uint32_t {
        Perm_None = 0,
        Perm_Delete_Messages = 1 << 0,
        Perm_Pin_Messages = 1 << 1,
        Perm_Kick_Users = 1 << 2,
        Perm_Admin = 1 << 3
    };

#pragma pack(push, 1)
    struct PacketHeader {
        PacketType type;
        uint32_t   size;

        void ToNetwork() { size = HostToNet32(size); }
        void ToHost() { size = NetToHost32(size); }
    };

    struct ReceiverReportPayload {
        uint32_t highestSequenceReceived;
        uint32_t packetsLost;
        uint32_t jitterMs;
        uint8_t  fractionLost;

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

    // networkState values: 0=stable, 1=degraded, 2=critical
    struct SenderReportPayload {
        uint32_t suggestedBitrateKbps;
        uint32_t estimatedRttMs;
        uint8_t  networkState;
        uint8_t  reserved[3];

        void ToNetwork() {
            suggestedBitrateKbps = HostToNet32(suggestedBitrateKbps);
            estimatedRttMs = HostToNet32(estimatedRttMs);
        }
        void ToHost() {
            suggestedBitrateKbps = NetToHost32(suggestedBitrateKbps);
            estimatedRttMs = NetToHost32(estimatedRttMs);
        }
    };
#pragma pack(pop)

} // namespace TalkMe
