#pragma once
#include <string>
#include <vector>
#include <cstring>
#include <nlohmann/json.hpp>
#include "../../TalkMe/src/shared/Protocol.h"
#include <winsock2.h>

namespace TalkMe {
    class PacketHandler {
    public:
        static std::string CreateLoginPayload(const std::string& email, const std::string& password) {
            nlohmann::json j;
            j["e"] = email;
            j["p"] = password;
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

        static std::string JoinVoiceChannelPayload(int cid) {
            nlohmann::json j;
            j["cid"] = cid;
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

        // ✅ NEW: Create Opus voice payload with sequence number
        static std::vector<uint8_t> CreateVoicePayloadOpus(const std::string& username, const std::vector<uint8_t>& opusData, uint32_t seqNum) {
            std::vector<uint8_t> payload;

            // Format: [seqNum:4][usernameLen:1][username:N][opusData:M]
            payload.resize(sizeof(uint32_t) + 1 + username.size() + opusData.size());

            size_t offset = 0;

            // Write sequence number (network byte order)
            uint32_t netSeq = htonl(seqNum);
            std::memcpy(&payload[offset], &netSeq, sizeof(uint32_t));
            offset += sizeof(uint32_t);

            // Write username length
            uint8_t ulen = (uint8_t)username.size();
            payload[offset] = ulen;
            offset += 1;

            // Write username
            std::memcpy(payload.data() + offset, username.c_str(), username.size());
            offset += username.size();

            // Write Opus data
            if (!opusData.empty()) std::memcpy(payload.data() + offset, opusData.data(), opusData.size());

            return payload;
        }

        // Parse incoming Opus voice packet
        struct ParsedVoicePacket {
            uint32_t sequenceNumber = 0;
            std::string sender;
            std::vector<uint8_t> opusData;
            bool valid = false;
        };

        static ParsedVoicePacket ParseVoicePayloadOpus(const std::vector<uint8_t>& payload) {
            ParsedVoicePacket result;

            if (payload.size() < sizeof(uint32_t) + 1) {
                return result; // Invalid
            }

            size_t offset = 0;

            // Read sequence number (convert from network byte order)
            uint32_t netSeq = 0;
            std::memcpy(&netSeq, payload.data() + offset, sizeof(uint32_t));
            result.sequenceNumber = ntohl(netSeq);
            offset += sizeof(uint32_t);

            // Read username length
            uint8_t ulen = payload[offset];
            offset += 1;

            if (payload.size() < offset + ulen) {
                return result; // Invalid
            }

            // Read username
            result.sender.assign((const char*)payload.data() + offset, ulen);
            offset += ulen;

            // Read Opus data
            if (offset < payload.size()) {
                result.opusData.assign(payload.begin() + offset, payload.end());
                result.valid = true;
            }
            // If not valid or sender empty, try legacy format: [ulen:1][username:N][opusData]
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

        // ⚠️ DEPRECATED: Old voice payload (for backward compatibility)
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