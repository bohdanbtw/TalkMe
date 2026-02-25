#include "Crypto.h"
#include <random>
#include <cstring>
#include <ctime>
#include <vector>
#include <cstdint>
#include <cstdio>

namespace {

    const char kBase32Alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";

    void Sha1(const uint8_t* data, size_t len, uint8_t out[20]) {
        uint32_t H[5] = { 0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u };
        uint8_t block[64];
        size_t i = 0;
        auto rotl = [](uint32_t x, int n) { return (x << n) | (x >> (32 - n)); };
        auto step = [&](const uint8_t* blk) {
            uint32_t W[80];
            for (int t = 0; t < 16; ++t) W[t] = (uint32_t)blk[t * 4] << 24 | (uint32_t)blk[t * 4 + 1] << 16 | (uint32_t)blk[t * 4 + 2] << 8 | blk[t * 4 + 3];
            for (int t = 16; t < 80; ++t) W[t] = rotl(W[t - 3] ^ W[t - 8] ^ W[t - 14] ^ W[t - 16], 1);
            uint32_t a = H[0], b = H[1], c = H[2], d = H[3], e = H[4];
            for (int t = 0; t < 80; ++t) {
                uint32_t f, k;
                if (t < 20) { f = (b & c) | ((~b) & d); k = 0x5A827999u; }
                else if (t < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1u; }
                else if (t < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
                else { f = b ^ c ^ d; k = 0xCA62C1D6u; }
                uint32_t temp = rotl(a, 5) + f + e + k + W[t];
                e = d; d = c; c = rotl(b, 30); b = a; a = temp;
            }
            H[0] += a; H[1] += b; H[2] += c; H[3] += d; H[4] += e;
            };
        while (i + 64 <= len) { step(data + i); i += 64; }
        std::memset(block, 0, sizeof(block));
        std::memcpy(block, data + i, len - i);
        block[len - i] = 0x80;
        if (len - i >= 56) { step(block); std::memset(block, 0, sizeof(block)); }
        uint64_t bits = len * 8;
        for (int j = 0; j < 8; ++j) block[63 - j] = (uint8_t)(bits >> (j * 8));
        step(block);
        for (int j = 0; j < 5; ++j) {
            out[j * 4 + 0] = (uint8_t)(H[j] >> 24);
            out[j * 4 + 1] = (uint8_t)(H[j] >> 16);
            out[j * 4 + 2] = (uint8_t)(H[j] >> 8);
            out[j * 4 + 3] = (uint8_t)(H[j]);
        }
    }

    void HmacSha1(const uint8_t* key, size_t keyLen, const uint8_t* msg, size_t msgLen, uint8_t out[20]) {
        uint8_t block[64];
        uint8_t ipad[64], opad[64];
        std::memset(block, 0, 64);
        if (keyLen > 64) {
            Sha1(key, keyLen, block);
            keyLen = 20;
            key = block;
        }
        else std::memcpy(block, key, keyLen);
        for (int i = 0; i < 64; ++i) { ipad[i] = block[i] ^ 0x36; opad[i] = block[i] ^ 0x5c; }
        std::vector<uint8_t> innerIn(64 + msgLen);
        std::memcpy(innerIn.data(), ipad, 64);
        std::memcpy(innerIn.data() + 64, msg, msgLen);
        uint8_t inner[20];
        Sha1(innerIn.data(), innerIn.size(), inner);
        uint8_t fin[84];
        std::memcpy(fin, opad, 64);
        std::memcpy(fin + 64, inner, 20);
        Sha1(fin, 84, out);
    }

    int Base32CharValue(char c) {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a';
        if (c >= '2' && c <= '7') return c - '2' + 26;
        return -1;
    }

    bool DecodeBase32(const std::string& encoded, std::vector<uint8_t>& out) {
        out.clear();
        int bits = 0;
        uint32_t buffer = 0;
        for (char c : encoded) {
            if (c == '=') break;
            int v = Base32CharValue(c);
            if (v < 0) return false;
            buffer = (buffer << 5) | (v & 31);
            bits += 5;
            if (bits >= 8) {
                out.push_back((uint8_t)(buffer >> (bits - 8)));
                buffer &= (1u << (bits - 8)) - 1u;
                bits -= 8;
            }
        }
        return true;
    }

} // namespace

namespace TalkMe {

    std::string GenerateBase32Secret(size_t length) {
        static const size_t kAlphabetSize = sizeof(kBase32Alphabet) - 1;
        std::string s;
        s.reserve(length);
        std::random_device rd;
        std::uniform_int_distribution<size_t> dist(0, kAlphabetSize - 1);
        for (size_t i = 0; i < length; ++i) s += kBase32Alphabet[dist(rd)];
        return s;
    }

    bool VerifyTOTP(const std::string& base32Secret, const std::string& userCode) {
        if (userCode.size() != 6) return false;
        std::vector<uint8_t> key;
        if (!DecodeBase32(base32Secret, key) || key.empty()) return false;
        std::time_t now = std::time(nullptr);
        int64_t counter = (now < 0) ? 0 : static_cast<int64_t>(now) / 30;
        uint8_t counterBuf[8];
        for (int step = -1; step <= 1; ++step) {
            int64_t c = counter + step;
            for (int i = 7; i >= 0; --i) { counterBuf[i] = (uint8_t)(c & 0xff); c >>= 8; }
            uint8_t hmac[20];
            HmacSha1(key.data(), key.size(), counterBuf, 8, hmac);
            int offset = hmac[19] & 0x0f;
            uint32_t code = ((hmac[offset] & 0x7fu) << 24) | ((uint32_t)hmac[offset + 1] << 16) |
                ((uint32_t)hmac[offset + 2] << 8) | hmac[offset + 3];
            code %= 1000000;
            char expected[7];
            std::snprintf(expected, sizeof(expected), "%06u", code);
            if (userCode == expected) return true;
        }
        return false;
    }

} // namespace TalkMe