#pragma once
#include <string>
#include <vector>
#include <cstring>
#include <nlohmann/json.hpp>
#include "Protocol.h"

namespace TalkMe {
    class PacketHandler {
    public:
        static std::string CreateLoginPayload(const std::string& email, const std::string& password, const std::string& hwid = "") {
            nlohmann::json j;
            j["e"] = email;
            j["p"] = password;
            if (!hwid.empty()) j["hwid"] = hwid;
            return j.dump();
        }

        static std::string CreateRegisterPayload(const std::string& email, const std::string& username, const std::string& password) {
            nlohmann::json j;
            j["e"] = email;
            j["u"] = username;
            j["p"] = password;
            return j.dump();
        }

        static std::string CreateServerPayload(const std::string& name, const std::string& owner) {
            nlohmann::json j;
            j["name"] = name;
            j["u"] = owner;
            return j.dump();
        }

        static std::string JoinServerPayload(const std::string& code, const std::string& username) {
            nlohmann::json j;
            j["code"] = code;
            j["u"] = username;
            return j.dump();
        }

        static std::string GetServerContentPayload(int serverId) {
            nlohmann::json j;
            j["sid"] = serverId;
            return j.dump();
        }

        static std::string CreateChannelPayload(int serverId, const std::string& name, const std::string& type) {
            nlohmann::json j;
            j["sid"] = serverId;
            j["name"] = name;
            j["type"] = type;
            return j.dump();
        }

        static std::string SelectTextChannelPayload(int cid) {
            nlohmann::json j;
            j["cid"] = cid;
            return j.dump();
        }

        /// Request a page of messages: beforeId=0 gives latest page; beforeId>0 gives older messages before that id.
        static std::string MessageHistoryPagePayload(int cid, int beforeId, int limit = 50) {
            nlohmann::json j;
            j["cid"] = cid;
            j["before"] = beforeId;
            j["limit"] = limit;
            return j.dump();
        }

        /// Request newer messages after afterId (ascending).
        static std::string MessageHistoryAfterPayload(int cid, int afterId, int limit = 50) {
            nlohmann::json j;
            j["cid"] = cid;
            j["after"] = afterId;
            j["limit"] = limit;
            return j.dump();
        }

        /// Request a window around an anchor message id.
        static std::string MessageHistoryAroundPayload(int cid, int anchorId, int beforeLimit = 100, int afterLimit = 100) {
            nlohmann::json j;
            j["cid"] = cid;
            j["anchor"] = anchorId;
            j["before_limit"] = beforeLimit;
            j["after_limit"] = afterLimit;
            return j.dump();
        }

        static std::string JoinVoiceChannelPayload(int cid) {
            nlohmann::json j;
            j["cid"] = cid;
            return j.dump();
        }

        static std::string CreateVoiceStatsPayload(int cid, float ping_ms, float loss_pct, float jitter_ms, int buffer_ms) {
            nlohmann::json j;
            j["cid"] = cid;
            j["ping_ms"] = ping_ms;
            j["loss_pct"] = loss_pct;
            j["jitter_ms"] = jitter_ms;
            j["buffer_ms"] = buffer_ms;
            return j.dump();
        }

        static std::string CreateMessagePayload(int cid, const std::string& sender, const std::string& content) {
            nlohmann::json j;
            j["cid"] = cid;
            j["u"] = sender;
            j["msg"] = content;
            return j.dump();
        }

        static std::string CreateDeleteMessagePayload(int mid, int cid, const std::string& username) {
            nlohmann::json j;
            j["mid"] = mid;
            j["cid"] = cid;
            j["u"] = username;
            return j.dump();
        }

        static std::vector<uint8_t> CreateVoicePayloadOpus(const std::string& username, const std::vector<uint8_t>& opusData, uint32_t seqNum) {
            std::vector<uint8_t> payload;

            payload.resize(sizeof(uint32_t) + 1 + username.size() + opusData.size());

            size_t offset = 0;

            uint32_t netSeq = HostToNet32(seqNum);
            std::memcpy(&payload[offset], &netSeq, sizeof(uint32_t));
            offset += sizeof(uint32_t);

            uint8_t ulen = (uint8_t)username.size();
            payload[offset] = ulen;
            offset += 1;

            std::memcpy(payload.data() + offset, username.c_str(), username.size());
            offset += username.size();

            if (!opusData.empty()) std::memcpy(payload.data() + offset, opusData.data(), opusData.size());

            return payload;
        }

        struct ParsedVoicePacket {
            uint32_t sequenceNumber = 0;
            std::string sender;
            std::vector<uint8_t> opusData;
            bool valid = false;
        };

        static ParsedVoicePacket ParseVoicePayloadOpus(const std::vector<uint8_t>& payload) {
            ParsedVoicePacket result;

            if (payload.size() < sizeof(uint32_t) + 1) {
                return result;
            }

            size_t offset = 0;

            uint32_t netSeq = 0;
            std::memcpy(&netSeq, payload.data() + offset, sizeof(uint32_t));
            result.sequenceNumber = NetToHost32(netSeq);
            offset += sizeof(uint32_t);

            uint8_t ulen = payload[offset];
            offset += 1;

            if (payload.size() < offset + ulen) {
                return result;
            }

            result.sender.assign((const char*)payload.data() + offset, ulen);
            offset += ulen;

            if (offset < payload.size()) {
                result.opusData.assign(payload.begin() + offset, payload.end());
                result.valid = true;
            }
            if (!result.valid || result.sender.empty()) {
                ParsedVoicePacket legacy;
                size_t off2 = 0;
                if (payload.size() >= 1) {
                    uint8_t ulen2 = payload[off2]; off2 += 1;
                    if (payload.size() >= off2 + ulen2) {
                        legacy.sender.assign((const char*)payload.data() + off2, ulen2);
                        off2 += ulen2;
                        if (off2 < payload.size()) legacy.opusData.assign(payload.begin() + off2, payload.end());
                        legacy.sequenceNumber = 0;
                        legacy.valid = true;
                        return legacy;
                    }
                }
            }

            return result;
        }

        static std::string CreateVoicePayload(const std::string& username, const std::vector<uint8_t>& audio) {
            std::string payload;
            uint8_t ulen = (uint8_t)username.size();
            payload.push_back((char)ulen);
            payload.append(username);
            payload.append((const char*)audio.data(), audio.size());
            return payload;
        }
    };
}
