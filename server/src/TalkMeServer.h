#pragma once

#include "Protocol.h"
#include <asio.hpp>
#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace TalkMe {

    class ChatSession;

    // ---------------------------------------------------------------------------
    // UDP binding: one entry per authenticated user in a voice channel.
    // Includes token-bucket state for per-sender rate limiting and
    // server-side sequence tracking for future RTCP-Lite correlation.
    // ---------------------------------------------------------------------------
    struct UdpBinding {
        asio::ip::udp::endpoint endpoint;
        std::atomic<int64_t>   lastSeenMs{ 0 };
        int                    voiceCid{ -1 };

        // Token bucket – 60 packets/sec ceiling, burst cap 100.
        std::atomic<int>       tokens{ 100 };
        std::atomic<int64_t>   lastRefillMs{ 0 };

        // Server-side sequence tracker for Receiver_Report verification.
        std::atomic<uint32_t>  highestSeqReceived{ 0 };

        // Non-copyable because of atomics; movable for map emplace.
        UdpBinding() = default;
        UdpBinding(const UdpBinding&) = delete;
        UdpBinding& operator=(const UdpBinding&) = delete;
        UdpBinding(UdpBinding&&) = default;
        UdpBinding& operator=(UdpBinding&&) = default;
    };

    // ---------------------------------------------------------------------------
    // Per-user voice stats snapshot (written by Receiver_Report handler).
    // ---------------------------------------------------------------------------
    struct VoiceStatEntry {
        float ping_ms{ 0.f };
        float loss_pct{ 0.f };
        float jitter_ms{ 0.f };
        int   buffer_ms{ 0 };
        int   cid{ -1 };
    };

    // ---------------------------------------------------------------------------
    // Aggregated sample stored in the rolling history ring.
    // ---------------------------------------------------------------------------
    struct AggVoiceSample {
        int64_t ts{ 0 };
        float   avg_ping_ms{ 0.f };
        float   avg_loss_pct{ 0.f };
        float   avg_jitter_ms{ 0.f };
        int     avg_buffer_ms{ 0 };
        int     clients{ 0 };
    };

    // ---------------------------------------------------------------------------
    // TalkMeServer
    // ---------------------------------------------------------------------------
    class TalkMeServer {
    public:
        explicit TalkMeServer(asio::io_context& io_context, short port);

        // Called by ChatSession on connect / disconnect / channel switch.
        void JoinClient(std::shared_ptr<ChatSession> session);
        void LeaveClient(std::shared_ptr<ChatSession> session);
        void SetVoiceChannel(std::shared_ptr<ChatSession> session, int newCid, int oldCid);

        // Broadcast helpers.
        void BroadcastToAll(PacketType type, const std::string& data);
        void BroadcastToChannelMembers(int channelId, PacketType type, const std::string& payload);
        void BroadcastVoice(int cid, std::shared_ptr<ChatSession> sender,
            PacketHeader h, const std::vector<uint8_t>& body);

        // Stats ingestion (called from Receiver_Report handler).
        void RecordVoiceStats(const std::string& username, int cid,
            float ping_ms, float loss_pct,
            float jitter_ms, int buffer_ms);

        // Per-channel bitrate ceiling derived from active speaker count.
        uint32_t GetChannelBitrateLimit(int cid);

    private:
        // --- Tuning constants ---------------------------------------------------
        static constexpr size_t  kActiveSpeakerMax = 32;
        static constexpr size_t  kMaxVoiceStatsSamples = 360;
        static constexpr int     kTokenBucketMax = 150;
        static constexpr int     kPacketsPerSec = 150;
        static constexpr int64_t kActiveSpeakerWindowMs = 2'000;
        static constexpr int64_t kUdpBindingTtlMs = 60'000;
        static constexpr int64_t kVoiceIdleEvictSec = 60;

        // --- ASIO handles -------------------------------------------------------
        asio::ip::tcp::acceptor   m_Acceptor;
        asio::ip::udp::socket     m_VoiceUdpSocket;
        asio::io_context& m_IoContext;

        // --- Session registry (guarded by m_RoomMutex) -------------------------
        std::shared_mutex                                                m_RoomMutex;
        std::set<std::shared_ptr<ChatSession>>                           m_AllSessions;
        std::unordered_map<int, std::set<std::shared_ptr<ChatSession>>>  m_VoiceChannels;

        // --- UDP bindings (guarded by m_RoomMutex) -----------------------------
        std::unordered_map<std::string, UdpBinding> m_UdpBindings;

        // --- O(1) active-speaker tracking (guarded by m_SpeakerMutex) ----------
        // Maps cid -> { username -> last_spoken_timestamp_ms }
        std::mutex                                                              m_SpeakerMutex;
        std::unordered_map<int, std::unordered_map<std::string, int64_t>>      m_RecentSpeakersByChannel;

        // --- Telemetry (guarded by m_StatsMutex) --------------------------------
        std::mutex                                  m_StatsMutex;
        std::unordered_map<std::string, VoiceStatEntry> m_LastVoiceStats;
        std::deque<AggVoiceSample>                  m_VoiceStatsHistory;

        // --- UDP receive scratch space ------------------------------------------
        std::array<uint8_t, 65'535> m_VoiceRecvBuffer{};
        asio::ip::udp::endpoint     m_VoiceRecvFrom;

        // --- Internal helpers ---------------------------------------------------
        void DoAccept();
        void StartVoiceUdpReceive();
        void StartConnectionHealthCheck();
        void StartVoiceOptimizationTimer();
        void StartVoiceStatsWriteTimer();

        void HandleVoiceUdpPacket(const std::vector<uint8_t>& packet,
            const asio::ip::udp::endpoint& from);

        // Must be called while m_RoomMutex is already held (any mode).
        void RefreshChannelControlLockFree(int cid,
            const std::string& targetUser = {},
            bool isJoin = false);

        static std::shared_ptr<std::vector<uint8_t>>
            CreateBuffer(PacketType type, const std::string& data);

        static std::shared_ptr<std::vector<uint8_t>>
            CreateBufferRaw(PacketHeader h, const std::vector<uint8_t>& body);
    };

} // namespace TalkMe