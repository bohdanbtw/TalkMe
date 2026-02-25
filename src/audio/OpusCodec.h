#pragma once

#include <vector>
#include <cstdint>
#include <cstddef>
#include <opus/opus.h>

namespace TalkMe {

// Shared constants for Opus and engine (48 kHz, 10 ms frames).
constexpr int   OPUS_FRAME_SIZE = 480;
constexpr int   SAMPLE_RATE = 48000;
constexpr size_t kOpusMaxPacket = 1276;

class OpusEncoderWrapper {
public:
    OpusEncoderWrapper();
    ~OpusEncoderWrapper();

    // Hot path: write into caller-supplied vector (pre-allocated to kOpusMaxPacket).
    // Returns bytes written, or 0 on error. No heap alloc when out has enough capacity.
    int EncodeInto(const float* pcm, std::vector<uint8_t>& out);

    // Rare path (e.g. keepalive). Allocates.
    std::vector<uint8_t> Encode(const float* pcm);

    void AdjustBitrate(float packetLossPercent);
    void SetPacketLossPercentage(float lossPercent);
    void SetTargetBitrate(int bitrate);
    int GetCurrentBitrate() const;

private:
    OpusEncoder* encoder = nullptr;
    int          currentBitrate = 32000;
    std::vector<uint8_t> m_EncodeBuffer;
};

class OpusDecoderWrapper {
public:
    OpusDecoderWrapper();
    ~OpusDecoderWrapper();

    int DecodeWithDiagnostics(const uint8_t* data, size_t len, float* pcmOut);
    int DecodeLossWithDiagnostics(float* pcmOut);

private:
    OpusDecoder* decoder = nullptr;
};

} // namespace TalkMe
