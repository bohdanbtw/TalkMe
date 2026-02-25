#include "OpusCodec.h"
#include "../core/Logger.h"
#include <thread>
#include <algorithm>
#include <format>

namespace TalkMe {

    OpusEncoderWrapper::OpusEncoderWrapper() {
        m_EncodeBuffer.resize(kOpusMaxPacket);
        int err;
        encoder = opus_encoder_create(SAMPLE_RATE, 1, OPUS_APPLICATION_VOIP, &err);
        if (err == OPUS_OK) {
            opus_encoder_ctl(encoder, OPUS_SET_BITRATE(32000));
            opus_encoder_ctl(encoder, OPUS_SET_BANDWIDTH(OPUS_AUTO));
            const unsigned int cores = std::thread::hardware_concurrency();
            const int complexity = (cores <= 2) ? 5 : (cores <= 4) ? 7 : 8;
            opus_encoder_ctl(encoder, OPUS_SET_COMPLEXITY(complexity));
            opus_encoder_ctl(encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
            opus_encoder_ctl(encoder, OPUS_SET_DTX(1));
            opus_encoder_ctl(encoder, OPUS_SET_VBR(1));
            opus_encoder_ctl(encoder, OPUS_SET_INBAND_FEC(1));
            opus_encoder_ctl(encoder, OPUS_SET_PACKET_LOSS_PERC(10));
            currentBitrate = 32000;
            LOG_AUDIO("Encoder created successfully");
        }
        else {
            LOG_ERROR_BUF(
                std::format("Encoder creation failed with error code: {}", err).c_str());
        }
    }

    OpusEncoderWrapper::~OpusEncoderWrapper() {
        if (encoder) opus_encoder_destroy(encoder);
    }

    int OpusEncoderWrapper::EncodeInto(const float* pcm, std::vector<uint8_t>& out) {
        if (!encoder) return 0;
        if (out.size() < kOpusMaxPacket) out.resize(kOpusMaxPacket);
        const int bytes = opus_encode_float(encoder, pcm, OPUS_FRAME_SIZE,
            out.data(),
            static_cast<opus_int32>(kOpusMaxPacket));
        if (bytes > 0) {
            out.resize(static_cast<size_t>(bytes));
            return bytes;
        }
        return 0;
    }

    std::vector<uint8_t> OpusEncoderWrapper::Encode(const float* pcm) {
        if (!encoder) return {};
        const int bytes = opus_encode_float(encoder, pcm, OPUS_FRAME_SIZE,
            m_EncodeBuffer.data(),
            static_cast<opus_int32>(m_EncodeBuffer.size()));
        if (bytes > 0)
            return { m_EncodeBuffer.data(), m_EncodeBuffer.data() + bytes };
        return {};
    }

    void OpusEncoderWrapper::AdjustBitrate(float packetLossPercent) {
        int newBitrate = 32000;
        if (packetLossPercent > 15.0f) newBitrate = 24000;
        else if (packetLossPercent > 5.0f) newBitrate = 28000;
        else if (packetLossPercent > 2.0f) newBitrate = 32000;
        else if (packetLossPercent < 0.5f) newBitrate = 48000;

        if (newBitrate != currentBitrate && encoder) {
            opus_encoder_ctl(encoder, OPUS_SET_BITRATE(newBitrate));
            currentBitrate = newBitrate;
            LOG_AUDIO_BUF(std::format(
                "Encoder bitrate adjusted to {} kbps (packet loss: {:.1f}%)",
                newBitrate / 1000, packetLossPercent).c_str());
        }
    }

    void OpusEncoderWrapper::SetPacketLossPercentage(float lossPercent) {
        if (encoder)
            opus_encoder_ctl(encoder,
                OPUS_SET_PACKET_LOSS_PERC(static_cast<int>(lossPercent)));
    }

    void OpusEncoderWrapper::SetTargetBitrate(int bitrate) {
        if (!encoder) return;
        const int clamped = std::clamp(bitrate, 12000, 64000);
        if (clamped != currentBitrate) {
            opus_encoder_ctl(encoder, OPUS_SET_BITRATE(clamped));
            currentBitrate = clamped;
        }
    }

    int OpusEncoderWrapper::GetCurrentBitrate() const {
        return currentBitrate;
    }

    // ---------------------------------------------------------------------------

    OpusDecoderWrapper::OpusDecoderWrapper() {
        int err;
        decoder = opus_decoder_create(SAMPLE_RATE, 1, &err);
        if (err != OPUS_OK) {
            LOG_ERROR_BUF(
                std::format("Decoder creation failed with error code: {}", err).c_str());
        }
        else {
            LOG_AUDIO("Decoder created successfully");
        }
    }

    OpusDecoderWrapper::~OpusDecoderWrapper() {
        if (decoder) opus_decoder_destroy(decoder);
    }

    int OpusDecoderWrapper::DecodeWithDiagnostics(const uint8_t* data, size_t len,
        float* pcmOut) {
        if (!decoder) return -1;
        const int samples = opus_decode_float(decoder, data, static_cast<opus_int32>(len),
            pcmOut, OPUS_FRAME_SIZE, 0);
        if (samples != OPUS_FRAME_SIZE) {
            LOG_ERROR_BUF(std::format(
                "Opus decode: got {} samples (expected {}), input_size={}",
                samples, OPUS_FRAME_SIZE, len).c_str());
        }
        return samples;
    }

    int OpusDecoderWrapper::DecodeLossWithDiagnostics(float* pcmOut) {
        if (!decoder) return -1;
        const int samples = opus_decode_float(decoder, nullptr, 0, pcmOut, OPUS_FRAME_SIZE, 0);
        if (samples == OPUS_FRAME_SIZE) LOG_AUDIO("Packet loss concealment applied");
        return samples;
    }

} // namespace TalkMe