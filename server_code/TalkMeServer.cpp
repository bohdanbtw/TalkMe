#include "TalkMeServer.h"
#include "ChatSession.h"   // full definition required: TalkMeServer.cpp dereferences shared_ptr<ChatSession>
#include "Database.h"
#include "Logger.h"
#include "Protocol.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>

using json = nlohmann::json;
using asio::ip::tcp;
using asio::ip::udp;

// ---------------------------------------------------------------------------
// File-local helpers
// ---------------------------------------------------------------------------
namespace {

    constexpr uint8_t kUdpVoicePacket = 0;
    constexpr uint8_t kUdpHelloPacket = 1;
    constexpr uint8_t kUdpPingPacket = 2;
    constexpr uint8_t kUdpPongPacket = 3;
    constexpr size_t  kPingPayloadSize = 8;

    struct ParsedVoiceOpus {
        std::string          sender;
        std::vector<uint8_t> opus;
        uint32_t             sequenceNumber{ 0 }; // parsed from bytes [0-3]
        bool                 valid{ false };
    };

    struct AdaptiveVoiceProfile {
        int  keepaliveIntervalMs = 4000;
        int  voiceStateRequestIntervalSec = 4;
        int  jitterTargetMs = 90;
        int  jitterMinMs = 50;
        int  jitterMaxMs = 160;
        int  codecTargetKbps = 48;
        bool preferUdp = true;
    };

    int32_t ReadI32BE(const uint8_t* p) {
        return (static_cast<int32_t>(p[0]) << 24)
            | (static_cast<int32_t>(p[1]) << 16)
            | (static_cast<int32_t>(p[2]) << 8)
            | static_cast<int32_t>(p[3]);
    }

    // ---------------------------------------------------------------------------
    // REQ 1: Mathematical jitter profiles � no hardcoded tiers.
    // ---------------------------------------------------------------------------
    AdaptiveVoiceProfile BuildVoiceProfile(size_t memberCount) {
        AdaptiveVoiceProfile p;
        const int n = static_cast<int>(memberCount);

        p.jitterMinMs = std::clamp(30 + n * 2, 30, 100);
        p.jitterTargetMs = std::clamp(50 + n * 5, 50, 200);
        p.jitterMaxMs = std::clamp(120 + n * 10, 120, 400);
        // Inversely proportional to member count; floor at 24 kbps.
        p.codecTargetKbps = std::max(24, 64 - n * 2);
        // Keepalive and state poll scale gently with group size.
        p.keepaliveIntervalMs = std::clamp(2000 + n * 100, 2000, 6000);
        p.voiceStateRequestIntervalSec = std::clamp(3 + n / 5, 3, 6);

        return p;
    }

    // ---------------------------------------------------------------------------
    // REQ 2: Parse voice payload; extract sequence number from bytes [0-3].
    // ---------------------------------------------------------------------------
    ParsedVoiceOpus ParseVoicePayloadOpus(const std::vector<uint8_t>& payload) {
        ParsedVoiceOpus out;
        // Minimum: 4 (seq) + 1 (ulen) + 1 (username char) + 1 (opus byte) = 7
        if (payload.size() < 7) return out;

        out.sequenceNumber = static_cast<uint32_t>(ReadI32BE(payload.data()));

        size_t  offset = 4;
        uint8_t ulen = payload[offset++];
        if (payload.size() < offset + ulen || ulen == 0) return out;

        out.sender.assign(reinterpret_cast<const char*>(payload.data() + offset), ulen);
        offset += ulen;
        if (offset >= payload.size()) return out;

        out.opus.assign(payload.begin() + offset, payload.end());
        out.valid = true;
        return out;
    }

    std::string EndpointKey(const udp::endpoint& ep) {
        return ep.address().to_string() + ':' + std::to_string(ep.port());
    }

} // namespace

// ---------------------------------------------------------------------------
// TalkMeServer implementation
// ---------------------------------------------------------------------------
namespace TalkMe {

    TalkMeServer::TalkMeServer(asio::io_context& io_context, short port)
        : m_Acceptor(io_context, tcp::endpoint(tcp::v4(), port))
        , m_VoiceUdpSocket(io_context, udp::endpoint(udp::v4(), VOICE_PORT))
        , m_IoContext(io_context)
    {
        DoAccept();
        StartVoiceUdpReceive();
        StartVoiceOptimizationTimer();
        StartConnectionHealthCheck();
        StartVoiceStatsWriteTimer();
    }

    // ---------------------------------------------------------------------------
    // Public API
    // ---------------------------------------------------------------------------

    void TalkMeServer::RecordVoiceStats(const std::string& username, int cid,
        float ping_ms, float loss_pct,
        float jitter_ms, int buffer_ms)
    {
        if (username.empty() || cid < 0) return;
        std::lock_guard lock(m_StatsMutex);
        m_LastVoiceStats[username] = { ping_ms, loss_pct, jitter_ms, buffer_ms, cid };
    }

    // REQ 5: Generous SFU bitrate math � 512 kbps budget, 24 kbps floor.
    uint32_t TalkMeServer::GetChannelBitrateLimit(int cid) {
        std::lock_guard lock(m_SpeakerMutex);
        auto it = m_RecentSpeakersByChannel.find(cid);
        uint32_t activeCount = 1;
        if (it != m_RecentSpeakersByChannel.end() && !it->second.empty()) {
            const int64_t cutoff = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count()
                - kActiveSpeakerWindowMs;
            activeCount = static_cast<uint32_t>(
                std::count_if(it->second.begin(), it->second.end(),
                    [cutoff](const auto& kv) { return kv.second >= cutoff; }));
            activeCount = std::max(1u, activeCount);
        }
        return std::max(24u, std::min(64u, 512u / activeCount));
    }

    void TalkMeServer::JoinClient(std::shared_ptr<ChatSession> session) {
        std::unique_lock lock(m_RoomMutex);
        m_AllSessions.insert(std::move(session));
    }

    void TalkMeServer::LeaveClient(std::shared_ptr<ChatSession> session) {
        std::unique_lock lock(m_RoomMutex);
        m_AllSessions.erase(session);

        const std::string& user = session->GetUsername();
        if (!user.empty()) {
            bool hasActive = false;
            for (const auto& s : m_AllSessions) {
                if (s->GetUsername() == user) {
                    hasActive = true;
                    break;
                }
            }
            if (!hasActive) {
                m_UdpBindings.erase(user);
            }
        }

        int cid = session->GetVoiceChannelId();
        if (cid != -1) {
            m_VoiceChannels[cid].erase(session);
            RefreshChannelControlLockFree(cid, user, false);
        }
    }

    void TalkMeServer::SetVoiceChannel(std::shared_ptr<ChatSession> session,
        int newCid, int oldCid)
    {
        std::unique_lock lock(m_RoomMutex);
        if (oldCid != -1 && oldCid != newCid) {
            const std::string& user = session->GetUsername();
            m_VoiceChannels[oldCid].erase(session);
            if (!user.empty()) m_UdpBindings.erase(user);
            RefreshChannelControlLockFree(oldCid, user, false);
        }
        if (newCid != -1) {
            auto& ch = m_VoiceChannels[newCid];
            const std::string& user = session->GetUsername();
            // Remove stale duplicate sessions for the same user.
            for (auto it = ch.begin(); it != ch.end(); ) {
                if ((*it)->GetUsername() == user && *it != session)
                    it = ch.erase(it);
                else
                    ++it;
            }
            ch.insert(session);
            RefreshChannelControlLockFree(newCid, user, true);
        }
    }

    void TalkMeServer::BroadcastToAll(PacketType type, const std::string& data) {
        auto buf = CreateBuffer(type, data);
        std::shared_lock lock(m_RoomMutex);
        for (const auto& s : m_AllSessions) s->SendShared(buf, false);
    }

    void TalkMeServer::BroadcastVoice(int cid, std::shared_ptr<ChatSession> sender,
        PacketHeader h, const std::vector<uint8_t>& body)
    {
        auto buf = CreateBufferRaw(h, body);
        std::shared_lock lock(m_RoomMutex);
        // Bug fix: operator[] on unordered_map inserts a default entry when the key
        // is absent. Under a shared_lock that is a write operation — a data race that
        // silently corrupts the map when multiple ASIO threads relay voice
        // simultaneously. Use find() which is a pure read.
        auto it = m_VoiceChannels.find(cid);
        if (it == m_VoiceChannels.end()) return;
        for (const auto& s : it->second)
            if (s != sender && s->GetVoiceChannelId() == cid)
                s->SendShared(buf, true);
    }

    // ---------------------------------------------------------------------------
    // REQ 2 (O(1) hot path) + REQ 4 (token bucket) + REQ 7 (seq tracking)
    // ---------------------------------------------------------------------------
    void TalkMeServer::HandleVoiceUdpPacket(const std::vector<uint8_t>& packet,
        const udp::endpoint& from)
    {
        if (packet.empty()) return;
        const uint8_t kind = packet[0];

        if (kind == 0xEE && packet.size() == 13) {
            auto probeBuffer = std::make_shared<std::vector<uint8_t>>(packet);
            m_VoiceUdpSocket.async_send_to(asio::buffer(*probeBuffer), from,
                [probeBuffer](const std::error_code&, std::size_t) {});
            return;
        }

        if (kind == kUdpPingPacket && packet.size() >= 1 + kPingPayloadSize) {
            auto pongBuffer = std::make_shared<std::vector<uint8_t>>();
            pongBuffer->reserve(1 + kPingPayloadSize);
            pongBuffer->push_back(kUdpPongPacket);
            pongBuffer->insert(pongBuffer->end(), packet.begin() + 1, packet.begin() + 1 + kPingPayloadSize);
            m_VoiceUdpSocket.async_send_to(asio::buffer(*pongBuffer), from,
                [pongBuffer](const std::error_code&, std::size_t) {});
            return;
        }

        // UDP hello: register endpoint -> username binding.
        if (kind == kUdpHelloPacket) {
            if (packet.size() < 1 + 1 + 4) return;
            size_t  offset = 1;
            uint8_t ulen = packet[offset++];
            if (packet.size() < offset + ulen + 4 || ulen == 0) return;

            std::string username(reinterpret_cast<const char*>(packet.data() + offset), ulen);
            offset += ulen;
            int32_t voiceCid = ReadI32BE(packet.data() + offset);

            std::unique_lock lock(m_RoomMutex);
            if (voiceCid < 0 || username.empty()) {
                m_UdpBindings.erase(username);
                VoiceTrace::log("step=udp_hello_drop reason=invalid user=" + username);
                return;
            }
            // Validate the session exists and is in the claimed channel.
            auto sit = std::find_if(m_AllSessions.begin(), m_AllSessions.end(),
                [&username](const auto& s) { return s->GetUsername() == username; });
            if (sit == m_AllSessions.end()) {
                VoiceTrace::log("step=udp_hello_drop reason=session_not_found user=" + username);
                return;
            }
            if ((*sit)->GetVoiceChannelId() != voiceCid) {
                VoiceTrace::log("step=udp_hello_drop reason=channel_mismatch user=" + username);
                return;
            }
            auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            auto& binding = m_UdpBindings[username];
            binding.endpoint = from;
            binding.lastSeenMs.store(nowMs, std::memory_order_relaxed);
            binding.voiceCid = voiceCid;
            binding.lastRefillMs.store(nowMs, std::memory_order_relaxed);
            binding.tokens.store(kTokenBucketMax, std::memory_order_relaxed);
            VoiceTrace::log("step=udp_hello_ok user=" + username
                + " cid=" + std::to_string(voiceCid)
                + " bindings=" + std::to_string(m_UdpBindings.size()));
            return;
        }

        if (kind != kUdpVoicePacket) return;
        if (packet.size() < 2) return;

        std::vector<uint8_t> voicePayload(packet.begin() + 1, packet.end());
        auto parsed = ParseVoicePayloadOpus(voicePayload);
        if (!parsed.valid || parsed.sender.empty()) {
            VoiceTrace::log("step=server_drop reason=parse_fail size="
                + std::to_string(packet.size()));
            return;
        }

        static std::atomic<uint32_t> s_recvCount{ 0 };
        if (const uint32_t rc = ++s_recvCount; rc <= 10 || (rc % 50) == 0)
            VoiceTrace::log("step=server_recv sender=" + parsed.sender
                + " bytes=" + std::to_string(voicePayload.size()));

        std::vector<udp::endpoint>              udpTargets;
        std::vector<std::shared_ptr<ChatSession>> tcpFallback;
        int cid = -1;

        const int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();

        {
            std::shared_lock lock(m_RoomMutex);

            // --- REQ 4: Token bucket rate limiter ---------------------------------
            auto senderIt = m_UdpBindings.find(parsed.sender);
            if (senderIt == m_UdpBindings.end()) {
                VoiceTrace::log("step=server_drop reason=sender_not_bound sender="
                    + parsed.sender);
                return;
            }
            if (EndpointKey(senderIt->second.endpoint) != EndpointKey(from)) {
                VoiceTrace::log("step=server_drop reason=endpoint_mismatch sender="
                    + parsed.sender);
                return;
            }

            // Refill tokens proportional to elapsed time (ceiling: kPacketsPerSec/s).
            //
            // Fractional preservation: instead of stamping lastRefillMs = nowMs (which
            // silently discards sub-millisecond remainder and causes artificial starvation
            // at 100pps), we advance lastRefillMs by exactly the time cost of the tokens
            // we actually minted. The remainder carries forward to the next packet.
            //
            // Silence clamp: if a user was muted for > 1 second the virtual clock would
            // fall far behind, causing an instant full-bucket grant on return that could
            // swamp downstream consumers. Snap to nowMs in that case.
            {
                auto& b = senderIt->second;
                int64_t lastRefill = b.lastRefillMs.load(std::memory_order_relaxed);

                // Snap stale clock: user was silent for > 1 second.
                if (nowMs - lastRefill > 1000) {
                    b.lastRefillMs.store(nowMs, std::memory_order_relaxed);
                    lastRefill = nowMs;
                }

                const int64_t elapsed = nowMs - lastRefill;
                if (elapsed > 0) {
                    const int refill = static_cast<int>(elapsed * kPacketsPerSec / 1000);
                    if (refill > 0) {
                        const int cur = b.tokens.load(std::memory_order_relaxed);
                        b.tokens.store(std::min(kTokenBucketMax, cur + refill),
                            std::memory_order_relaxed);
                        // Advance by exact token cost only - preserves fractional remainder.
                        b.lastRefillMs.store(lastRefill + (refill * 1000 / kPacketsPerSec),
                            std::memory_order_relaxed);
                    }
                }

                if (b.tokens.fetch_sub(1, std::memory_order_relaxed) < 1) {
                    b.tokens.store(0, std::memory_order_relaxed);
                    VoiceTrace::log("step=server_drop reason=rate_limited sender="
                        + parsed.sender);
                    return;
                }
            }

            // --- REQ 2 (O(1)): Trust voiceCid stored in UdpBinding ---------------
            // No linear scan of m_AllSessions needed � the binding is kept in sync
            // by SetVoiceChannel/LeaveClient whenever the session changes channel.
            cid = senderIt->second.voiceCid;
            if (cid < 0) return;

            // --- REQ 7: Update server-side highest sequence number ---------------
            {
                auto& b = senderIt->second;
                uint32_t prev = b.highestSeqReceived.load(std::memory_order_relaxed);
                if (parsed.sequenceNumber > prev)
                    b.highestSeqReceived.store(parsed.sequenceNumber,
                        std::memory_order_relaxed);
            }

            senderIt->second.lastSeenMs.store(nowMs, std::memory_order_relaxed);

            // --- REQ 2 (O(1)): Active-speaker gate --------------------------------
            {
                std::lock_guard speakerLock(m_SpeakerMutex);
                auto& speakerMap = m_RecentSpeakersByChannel[cid];
                const int64_t cutoff2s = nowMs - kActiveSpeakerWindowMs;

                // Is the sender already counted as active?
                auto speakerIt = speakerMap.find(parsed.sender);
                bool wasActive = (speakerIt != speakerMap.end()
                    && speakerIt->second >= cutoff2s);

                if (!wasActive) {
                    // Count currently active speakers (bounded by kActiveSpeakerMax, so O(1)).
                    size_t active = std::count_if(speakerMap.begin(), speakerMap.end(),
                        [cutoff2s](const auto& kv) { return kv.second >= cutoff2s; });
                    if (active >= kActiveSpeakerMax) {
                        VoiceTrace::log("step=server_drop reason=speaker_cap_exceeded sender="
                            + parsed.sender + " cid=" + std::to_string(cid));
                        return;
                    }
                }
                // O(1) timestamp update.
                speakerMap[parsed.sender] = nowMs;
            }

            // --- Build relay target lists ----------------------------------------
            const int64_t cutoffActive = nowMs - kActiveSpeakerWindowMs;
            // Bug fix: same operator[] race as in BroadcastVoice — use find().
            auto chIt = m_VoiceChannels.find(cid);
            if (chIt != m_VoiceChannels.end()) {
                for (const auto& s : chIt->second) {
                    if (!s || s->GetUsername() == parsed.sender) continue;
                    auto bit = m_UdpBindings.find(s->GetUsername());
                    if (bit != m_UdpBindings.end()
                        && bit->second.voiceCid == cid
                        && bit->second.lastSeenMs.load(std::memory_order_relaxed) >= cutoffActive)
                    {
                        udpTargets.push_back(bit->second.endpoint);
                    }
                    else {
                        tcpFallback.push_back(s);
                    }
                }
            }
        } // release shared lock

        // --- Relay ---------------------------------------------------------------
        static std::atomic<uint32_t> s_udpVoiceCount{ 0 };
        if (const uint32_t n = ++s_udpVoiceCount; n <= 10 || (n % 50) == 0)
            VoiceTrace::log("step=server_relay sender=" + parsed.sender
                + " cid=" + std::to_string(cid)
                + " udp=" + std::to_string(udpTargets.size())
                + " tcp=" + std::to_string(tcpFallback.size())
                + " bytes=" + std::to_string(voicePayload.size()));

        // REQ 1 (Mutual Exclusion): TCP fallback is strictly the else branch of UDP.
        // Build shared buffers once; reuse across all targets (single heap allocation).
        PacketHeader h{ PacketType::Voice_Data_Opus,
                        static_cast<uint32_t>(voicePayload.size()) };
        auto tcpBuffer = CreateBufferRaw(h, voicePayload);

        for (const auto& s : tcpFallback)
            s->SendShared(tcpBuffer, true);

        if (!udpTargets.empty()) {
            // One allocation shared across all async_send_to calls (scatter-gather style).
            auto udpPacket = std::make_shared<std::vector<uint8_t>>(1 + voicePayload.size());
            (*udpPacket)[0] = kUdpVoicePacket;
            std::memcpy(udpPacket->data() + 1, voicePayload.data(), voicePayload.size());

            for (const auto& ep : udpTargets)
                m_VoiceUdpSocket.async_send_to(
                    asio::buffer(*udpPacket), ep,
                    [udpPacket](const std::error_code&, std::size_t) {});
        }
    }

    // ---------------------------------------------------------------------------
    // REQ 3: Two-phase locking for health checks.
    //   Phase 1 � shared read lock: collect dead sessions / stale UDP keys.
    //   Phase 2 � exclusive write lock: execute only the .erase() calls.
    // ---------------------------------------------------------------------------
    void TalkMeServer::StartConnectionHealthCheck() {
        auto timer = std::make_shared<asio::steady_timer>(
            m_IoContext, std::chrono::seconds(5));

        timer->async_wait([this, timer](const std::error_code& ec) {
            if (ec) return;

            const auto now = std::chrono::steady_clock::now();
            const int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                now.time_since_epoch()).count();

            std::vector<std::shared_ptr<ChatSession>>  deadSessions;
            std::vector<std::pair<int, std::shared_ptr<ChatSession>>> staleVoice;
            std::vector<std::string>                   deadUdpUsers;

            // --- Phase 1: read sweep (shared lock) ---
            {
                std::shared_lock readLock(m_RoomMutex);

                for (const auto& session : m_AllSessions) {
                    int64_t lastActMs = session->GetLastActivityTimeMs();
                    int64_t elapsedSec = (nowMs - lastActMs) / 1000;
                    // Global TCP Idle Timeout (5 minutes) - Prevents Slowloris Connection Leaks
                    if (!session->IsHealthy() || elapsedSec > 300) {
                        deadSessions.push_back(session);
                        continue;
                    }

                    int cid = session->GetVoiceChannelId();
                    if (cid != -1) {
                        // OPTIMIZATION: Check if UDP is still flowing before evicting
                        bool isUdpActive = false;
                        auto uit = m_UdpBindings.find(session->GetUsername());
                        if (uit != m_UdpBindings.end()) {
                            if (nowMs - uit->second.lastSeenMs.load(std::memory_order_relaxed) < (kVoiceIdleEvictSec * 1000)) {
                                isUdpActive = true;
                            }
                        }
                        if (elapsedSec > kVoiceIdleEvictSec && !isUdpActive) {
                            staleVoice.emplace_back(cid, session);
                        }
                    }
                }

                const int64_t udpCutoff = nowMs - kUdpBindingTtlMs;
                for (const auto& [user, binding] : m_UdpBindings)
                    if (binding.lastSeenMs.load(std::memory_order_relaxed) < udpCutoff
                        || binding.voiceCid < 0)
                        deadUdpUsers.push_back(user);
            }

            // --- Phase 2: write mutations (exclusive lock, minimal scope) ---
            {
                std::unique_lock writeLock(m_RoomMutex);
                for (const auto& session : deadSessions) {
                    m_AllSessions.erase(session);

                    int cid = session->GetVoiceChannelId();
                    if (cid != -1) {
                        m_VoiceChannels[cid].erase(session);
                        RefreshChannelControlLockFree(cid);
                    }

                    const std::string& user = session->GetUsername();
                    if (!user.empty()) {
                        bool hasActive = false;
                        for (const auto& s : m_AllSessions) {
                            if (s->GetUsername() == user) {
                                hasActive = true;
                                break;
                            }
                        }
                        if (!hasActive) {
                            m_UdpBindings.erase(user);
                        }
                    }
                }
                for (const auto& [cid, session] : staleVoice) {
                    m_VoiceChannels[cid].erase(session);
                    RefreshChannelControlLockFree(cid);
                }
                for (const auto& user : deadUdpUsers)
                    m_UdpBindings.erase(user);
            }

            StartConnectionHealthCheck();
            });
    }

    // ---------------------------------------------------------------------------
    // REQ 6: Combined m_StatsMutex scope in the write timer.
    // ---------------------------------------------------------------------------
    void TalkMeServer::StartVoiceStatsWriteTimer() {
        auto timer = std::make_shared<asio::steady_timer>(
            m_IoContext, std::chrono::seconds(10));

        timer->async_wait([this, timer](const std::error_code& ec) {
            if (ec) return;

            // Single lock scope: aggregate + append + write JSON.
            {
                std::lock_guard lock(m_StatsMutex);

                if (!m_LastVoiceStats.empty()) {
                    AggVoiceSample sample;
                    sample.ts = std::chrono::system_clock::now().time_since_epoch()
                        / std::chrono::seconds(1);

                    float sumPing = 0, sumLoss = 0, sumJitter = 0;
                    int   sumBuffer = 0;
                    for (const auto& [_, st] : m_LastVoiceStats) {
                        sumPing += st.ping_ms;
                        sumLoss += st.loss_pct;
                        sumJitter += st.jitter_ms;
                        sumBuffer += st.buffer_ms;
                    }
                    const size_t n = m_LastVoiceStats.size();
                    sample.avg_ping_ms = sumPing / static_cast<float>(n);
                    sample.avg_loss_pct = sumLoss / static_cast<float>(n);
                    sample.avg_jitter_ms = sumJitter / static_cast<float>(n);
                    sample.avg_buffer_ms = static_cast<int>(sumBuffer / static_cast<int>(n));
                    sample.clients = static_cast<int>(n);

                    m_VoiceStatsHistory.push_back(sample);
                    while (m_VoiceStatsHistory.size() > kMaxVoiceStatsSamples)
                        m_VoiceStatsHistory.pop_front();
                }

                json arr = json::array();
                for (const auto& s : m_VoiceStatsHistory)
                    arr.push_back({ {"ts",           s.ts},
                                    {"avg_ping_ms",  s.avg_ping_ms},
                                    {"avg_loss_pct", s.avg_loss_pct},
                                    {"avg_jitter_ms",s.avg_jitter_ms},
                                    {"avg_buffer_ms",s.avg_buffer_ms},
                                    {"clients",      s.clients} });

                json out; out["samples"] = arr;
                if (std::ofstream f("voice_stats.json"); f)
                    f << out.dump();
            }

            StartVoiceStatsWriteTimer();
            });
    }

    // ---------------------------------------------------------------------------
    // Channel membership + config broadcast (called under m_RoomMutex).
    // ---------------------------------------------------------------------------
    void TalkMeServer::RefreshChannelControlLockFree(int cid,
        const std::string& targetUser,
        bool isJoin)
    {
        if (cid == -1) return;
        auto& membersSet = m_VoiceChannels[cid];
        const size_t memberCount = membersSet.size();
        auto profile = BuildVoiceProfile(memberCount);

        for (const auto& s : membersSet) s->SetVoiceLoad(memberCount);

        json cfg;
        cfg["keepalive_interval_ms"] = profile.keepaliveIntervalMs;
        cfg["voice_state_request_interval_sec"] = profile.voiceStateRequestIntervalSec;
        cfg["jitter_buffer_target_ms"] = profile.jitterTargetMs;
        cfg["jitter_buffer_min_ms"] = profile.jitterMinMs;
        cfg["jitter_buffer_max_ms"] = profile.jitterMaxMs;
        cfg["codec_target_kbps"] = profile.codecTargetKbps;
        cfg["prefer_udp"] = profile.preferUdp;
        cfg["server_version"] = "1.2";
        auto cfgBuffer = CreateBuffer(PacketType::Voice_Config, cfg.dump());

        if (!targetUser.empty()) {
            json deltaPayload;
            deltaPayload["cid"] = cid;
            deltaPayload["u"] = targetUser;
            deltaPayload["action"] = isJoin ? "join" : "leave";
            auto deltaBuffer = CreateBuffer(PacketType::Voice_State_Update,
                deltaPayload.dump());
            if (isJoin) {
                json members = json::array();
                for (const auto& s : membersSet) members.push_back(s->GetUsername());
                json fullPayload;
                fullPayload["cid"] = cid;
                fullPayload["members"] = members;
                auto fullStateBuffer = CreateBuffer(PacketType::Voice_State_Update,
                    fullPayload.dump());
                for (const auto& s : membersSet) {
                    s->SendShared(
                        (s->GetUsername() == targetUser) ? fullStateBuffer : deltaBuffer,
                        false);
                    s->SendShared(cfgBuffer, false);
                }
            }
            else {
                for (const auto& s : membersSet) {
                    s->SendShared(deltaBuffer, false);
                    s->SendShared(cfgBuffer, false);
                }
            }
        }
        else {
            json members = json::array();
            for (const auto& s : membersSet) members.push_back(s->GetUsername());
            json payload;
            payload["cid"] = cid;
            payload["members"] = members;
            auto stateBuffer = CreateBuffer(PacketType::Voice_State_Update, payload.dump());
            for (const auto& s : membersSet) {
                s->SendShared(stateBuffer, false);
                s->SendShared(cfgBuffer, false);
            }
        }
    }

    // ---------------------------------------------------------------------------
    // UDP receive loop
    // ---------------------------------------------------------------------------
    void TalkMeServer::StartVoiceUdpReceive() {
        m_VoiceUdpSocket.async_receive_from(
            asio::buffer(m_VoiceRecvBuffer), m_VoiceRecvFrom,
            [this](const std::error_code& ec, std::size_t bytes) {
                if (!ec && bytes > 0) {
                    HandleVoiceUdpPacket(
                        { m_VoiceRecvBuffer.begin(),
                          m_VoiceRecvBuffer.begin() + bytes },
                        m_VoiceRecvFrom);
                }
                StartVoiceUdpReceive();
            });
    }

    // ---------------------------------------------------------------------------
    // Periodic cleanup: evict empty voice channels.
    // ---------------------------------------------------------------------------
    void TalkMeServer::StartVoiceOptimizationTimer() {
        auto timer = std::make_shared<asio::steady_timer>(
            m_IoContext, std::chrono::seconds(30));

        timer->async_wait([this, timer](const std::error_code& ec) {
            if (ec) return;
            {
                std::unique_lock lock(m_RoomMutex);
                std::lock_guard  speakerLock(m_SpeakerMutex);
                for (auto it = m_VoiceChannels.begin(); it != m_VoiceChannels.end(); ) {
                    if (it->second.empty()) {
                        m_RecentSpeakersByChannel.erase(it->first);
                        it = m_VoiceChannels.erase(it);
                    }
                    else {
                        ++it;
                    }
                }
            }
            StartVoiceOptimizationTimer();
            });
    }

    // ---------------------------------------------------------------------------
    // TCP accept loop
    // ---------------------------------------------------------------------------
    void TalkMeServer::DoAccept() {
        m_Acceptor.async_accept([this](std::error_code ec, tcp::socket socket) {
            if (!ec) {
                std::fprintf(stderr, "[TalkMe Server] New TCP client connected\n");
                socket.set_option(tcp::no_delay(true));
                std::make_shared<ChatSession>(std::move(socket), *this)->Start();
            }
            DoAccept();
            });
    }

    // ---------------------------------------------------------------------------
    // Buffer factories
    // ---------------------------------------------------------------------------
    std::shared_ptr<std::vector<uint8_t>>
        TalkMeServer::CreateBuffer(PacketType type, const std::string& data) {
        const uint32_t size = static_cast<uint32_t>(data.size());
        PacketHeader header{ type, size };
        header.ToNetwork();
        auto buf = std::make_shared<std::vector<uint8_t>>(sizeof(header) + size);
        std::memcpy(buf->data(), &header, sizeof(header));
        if (size > 0) std::memcpy(buf->data() + sizeof(header), data.data(), size);
        return buf;
    }

    std::shared_ptr<std::vector<uint8_t>>
        TalkMeServer::CreateBufferRaw(PacketHeader h, const std::vector<uint8_t>& body) {
        h.ToNetwork();
        auto buf = std::make_shared<std::vector<uint8_t>>(sizeof(h) + body.size());
        std::memcpy(buf->data(), &h, sizeof(h));
        if (!body.empty()) std::memcpy(buf->data() + sizeof(h), body.data(), body.size());
        return buf;
    }

    void TalkMeServer::BroadcastToChannelMembers(int channelId, PacketType type, const std::string& payload) {
        std::vector<std::string> allowedUsers = Database::Get().GetUsersInServerByChannel(channelId);
        if (allowedUsers.empty()) return;
        auto buffer = CreateBuffer(type, payload);

        std::shared_lock lock(m_RoomMutex);
        for (const auto& session : m_AllSessions) {
            const std::string& username = session->GetUsername();
            if (!username.empty() && std::find(allowedUsers.begin(), allowedUsers.end(), username) != allowedUsers.end()) {
                session->SendShared(buffer, false);
            }
        }
    }

} // namespace TalkMe