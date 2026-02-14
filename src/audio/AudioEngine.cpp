#include "AudioEngine.h"
#include <iostream>
#include <vector>
#include <cstring> 
#include <map>
#include <string>
#include <cmath>
#include <algorithm> 
#include <chrono>
#include <mutex>

#include <opus/opus.h> 

#define MINIAUDIO_IMPLEMENTATION
#include "../../vendor/miniaudio.h" 

#ifndef NOMINMAX
#define NOMINMAX
#endif

namespace TalkMe {

    static const int MAX_TRACKS = 100;
    // Lower frame size => lower latency (480 samples = 10ms at 48kHz)
    static const int OPUS_FRAME_SIZE = 480;
    static const int SAMPLE_RATE = 48000;

    class OpusEncoderWrapper {
    public:
        OpusEncoderWrapper() {
            int err;
            encoder = opus_encoder_create(SAMPLE_RATE, 1, OPUS_APPLICATION_VOIP, &err);
            if (err == OPUS_OK) {
                // Increase bitrate and complexity for better quality
                opus_encoder_ctl(encoder, OPUS_SET_BITRATE(32000));
                opus_encoder_ctl(encoder, OPUS_SET_COMPLEXITY(10));
                opus_encoder_ctl(encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
                // Disable DTX for lower latency and smoother audio
                opus_encoder_ctl(encoder, OPUS_SET_DTX(0));
                opus_encoder_ctl(encoder, OPUS_SET_VBR(1));
                // Enable in-band FEC to improve resilience to packet loss
                opus_encoder_ctl(encoder, OPUS_SET_INBAND_FEC(1));
                opus_encoder_ctl(encoder, OPUS_SET_PACKET_LOSS_PERC(10));
            }
        }
        ~OpusEncoderWrapper() { if (encoder) opus_encoder_destroy(encoder); }
        std::vector<uint8_t> Encode(const float* pcm) {
            if (!encoder) return {};
            std::vector<uint8_t> out(4000);
            int bytes = opus_encode_float(encoder, pcm, OPUS_FRAME_SIZE, out.data(), (int)out.size());
            if (bytes > 0) { out.resize(bytes); return out; }
            return {};
        }
    private:
        OpusEncoder* encoder = nullptr;
    };

    class OpusDecoderWrapper {
    public:
        OpusDecoderWrapper() {
            int err;
            decoder = opus_decoder_create(SAMPLE_RATE, 1, &err);
        }
        ~OpusDecoderWrapper() { if (decoder) opus_decoder_destroy(decoder); }
        bool Decode(const uint8_t* data, size_t len, float* pcmOut) {
            if (!decoder) return false;
            int samples = opus_decode_float(decoder, data, (int)len, pcmOut, OPUS_FRAME_SIZE, 0);
            return (samples == OPUS_FRAME_SIZE);
        }
        bool DecodeLoss(float* pcmOut) {
            if (!decoder) return false;
            int samples = opus_decode_float(decoder, nullptr, 0, pcmOut, OPUS_FRAME_SIZE, 0);
            return (samples == OPUS_FRAME_SIZE);
        }
    private:
        OpusDecoder* decoder = nullptr;
    };

    struct VoiceTrack {
        ma_pcm_rb rb;
        bool active = false;
        std::string userId;
        std::unique_ptr<OpusDecoderWrapper> decoder;
        uint32_t lastSequenceNumber = 0;
        bool firstPacket = true;
        bool isBuffering = true;
    };

    struct AudioInternal {
        ma_device device;
        ma_device_config config;
        ma_pcm_rb captureRb;
        VoiceTrack tracks[MAX_TRACKS];
        std::unique_ptr<OpusEncoderWrapper> encoder;
        std::function<void(const std::vector<uint8_t>&, uint32_t)> onMicData;
        float currentGain = 1.0f;
        float targetGain = 1.0f;
        float captureRMS = 0.0f;
        const float VAD_THRESHOLD = 0.002f;
        uint32_t outgoingSeqNum = 0;
    };

    inline float CalculateRMS(const float* samples, ma_uint32 count) {
        float sum = 0.0f;
        for (ma_uint32 i = 0; i < count; ++i) sum += samples[i] * samples[i];
        return std::sqrt(sum / count);
    }

    inline float SoftClip(float x) {
        if (x < -3.0f) return -1.0f;
        if (x > 3.0f) return 1.0f;
        return x * (27.0f + x * x) / (27.0f + 9.0f * x * x);
    }

    void DataCallback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount) {
        auto* internal = (AudioInternal*)pDevice->pUserData;
        if (!internal) return;

        if (pInput && internal->onMicData) {
            const float* pInputFloat = (const float*)pInput;
            float rms = CalculateRMS(pInputFloat, frameCount);
            internal->captureRMS = rms * 0.2f + internal->captureRMS * 0.8f;
            if (internal->captureRMS > internal->VAD_THRESHOLD) {
                void* pWrite;
                ma_uint32 toWrite = frameCount;
                while (toWrite > 0) {
                    ma_uint32 chunk = toWrite;
                    if (ma_pcm_rb_acquire_write(&internal->captureRb, &chunk, &pWrite) != MA_SUCCESS) break;
                    float* dest = (float*)pWrite;
                    for (ma_uint32 i = 0; i < chunk; ++i) {
                        float boosted = pInputFloat[i] * 15.0f;
                        dest[i] = (boosted > 1.0f) ? 1.0f : (boosted < -1.0f ? -1.0f : boosted);
                    }
                    ma_pcm_rb_commit_write(&internal->captureRb, chunk);
                    pInputFloat += chunk;
                    toWrite -= chunk;
                }
            }
        }

        float* pOutputFloat = (float*)pOutput;
        std::memset(pOutputFloat, 0, frameCount * sizeof(float));
        int activeCount = 0;
        float mixBuffer[4096];
        if (frameCount > 4096) return;
        std::memset(mixBuffer, 0, frameCount * sizeof(float));

        for (int i = 0; i < MAX_TRACKS; ++i) {
            if (!internal->tracks[i].active) continue;
            ma_uint32 available = ma_pcm_rb_available_read(&internal->tracks[i].rb);

            if (internal->tracks[i].isBuffering) {
                if (available < 7200) continue;
                internal->tracks[i].isBuffering = false;
            }

            if (available < frameCount) {
                internal->tracks[i].isBuffering = true;
                continue;
            }

            activeCount++;
            float trackBuffer[4096];
            ma_uint32 read = 0;
            while (read < frameCount && read < available) {
                ma_uint32 chunk = (std::min)(frameCount - read, available - read);
                void* pRead;
                if (ma_pcm_rb_acquire_read(&internal->tracks[i].rb, &chunk, &pRead) != MA_SUCCESS) break;
                if (chunk == 0) break;
                std::memcpy(trackBuffer + read, pRead, chunk * sizeof(float));
                ma_pcm_rb_commit_read(&internal->tracks[i].rb, chunk);
                read += chunk;
            }
            for (ma_uint32 s = 0; s < read; ++s) mixBuffer[s] += trackBuffer[s];
        }

        if (activeCount > 0) {
            float mixRMS = CalculateRMS(mixBuffer, frameCount);
            internal->targetGain = (mixRMS > 0.001f) ? (std::min)(0.5f / (mixRMS * std::sqrt((float)activeCount)), 1.0f) : 1.0f;
            internal->currentGain = internal->currentGain * 0.95f + internal->targetGain * 0.05f;
            for (ma_uint32 i = 0; i < frameCount; ++i) pOutputFloat[i] = SoftClip(mixBuffer[i] * internal->currentGain);
        }
    }

    AudioEngine::AudioEngine() : m_Internal(std::make_unique<AudioInternal>()) {}
    AudioEngine::~AudioEngine() { Shutdown(); }

    bool AudioEngine::InitializeWithSequence(std::function<void(const std::vector<uint8_t>&, uint32_t)> onMicDataCaptured) {
        m_Internal->onMicData = onMicDataCaptured;
        m_Internal->encoder = std::make_unique<OpusEncoderWrapper>();
        // Keep ring buffers relatively small to reduce latency (1 second)
        ma_uint32 bufferFrames = SAMPLE_RATE * 1;
        if (ma_pcm_rb_init(ma_format_f32, 1, bufferFrames, nullptr, nullptr, &m_Internal->captureRb) != MA_SUCCESS) return false;
        for (int i = 0; i < MAX_TRACKS; ++i) {
            if (ma_pcm_rb_init(ma_format_f32, 1, bufferFrames, nullptr, nullptr, &m_Internal->tracks[i].rb) != MA_SUCCESS) return false;
            m_Internal->tracks[i].active = false;
        }
        m_Internal->config = ma_device_config_init(ma_device_type_duplex);
        m_Internal->config.sampleRate = SAMPLE_RATE;
        m_Internal->config.capture.channels = 1;
        m_Internal->config.playback.channels = 1;
        m_Internal->config.capture.format = ma_format_f32;
        m_Internal->config.playback.format = ma_format_f32;
        m_Internal->config.periodSizeInFrames = OPUS_FRAME_SIZE;
        m_Internal->config.dataCallback = DataCallback;
        m_Internal->config.pUserData = m_Internal.get();
        if (ma_device_init(nullptr, &m_Internal->config, &m_Internal->device) != MA_SUCCESS) return false;
        return (ma_device_start(&m_Internal->device) == MA_SUCCESS);
    }

    bool AudioEngine::Initialize(std::function<void(const std::vector<uint8_t>&)> onMicDataCaptured) {
        return InitializeWithSequence([onMicDataCaptured](const std::vector<uint8_t>& d, uint32_t s) { onMicDataCaptured(d); });
    }

    void AudioEngine::Update() {
        if (!m_Internal || !m_Internal->onMicData) return;
        ma_uint32 available = ma_pcm_rb_available_read(&m_Internal->captureRb);
        if (available >= OPUS_FRAME_SIZE) {
            float pcmBuffer[OPUS_FRAME_SIZE];
            void* pRead;
            ma_uint32 framesRead = OPUS_FRAME_SIZE;
            if (ma_pcm_rb_acquire_read(&m_Internal->captureRb, &framesRead, &pRead) == MA_SUCCESS) {
                std::memcpy(pcmBuffer, pRead, framesRead * sizeof(float));
                ma_pcm_rb_commit_read(&m_Internal->captureRb, framesRead);
                std::vector<uint8_t> packet = m_Internal->encoder->Encode(pcmBuffer);
                if (!packet.empty()) {
                    m_Internal->onMicData(packet, m_Internal->outgoingSeqNum++);
                }
            }
        }
    }

    void AudioEngine::PushIncomingAudioWithSequence(const std::string& userId, const std::vector<uint8_t>& opusData, uint32_t seqNum) {
        if (!m_Internal) return;
        int trackIdx = -1;
        for (int i = 0; i < MAX_TRACKS; ++i) {
            if (m_Internal->tracks[i].active && m_Internal->tracks[i].userId == userId) { trackIdx = i; break; }
        }
        if (trackIdx == -1) {
            for (int i = 0; i < MAX_TRACKS; ++i) {
                if (!m_Internal->tracks[i].active) {
                    m_Internal->tracks[i].userId = userId;
                    m_Internal->tracks[i].active = true;
                    m_Internal->tracks[i].decoder = std::make_unique<OpusDecoderWrapper>();
                    m_Internal->tracks[i].firstPacket = true;
                    m_Internal->tracks[i].isBuffering = true;
                    trackIdx = i; break;
                }
            }
        }
        if (trackIdx == -1) return;

        auto& track = m_Internal->tracks[trackIdx];
        if (!track.firstPacket && seqNum != 0) {
            uint32_t expected = track.lastSequenceNumber + 1;
            if (seqNum > expected && seqNum < expected + 10) {
                for (uint32_t i = 0; i < (seqNum - expected); ++i) {
                    float plc[OPUS_FRAME_SIZE];
                    if (track.decoder && track.decoder->DecodeLoss(plc)) {
                        void* pW; ma_uint32 c = OPUS_FRAME_SIZE;
                        if (ma_pcm_rb_acquire_write(&track.rb, &c, &pW) == MA_SUCCESS) {
                            std::memcpy(pW, plc, c * sizeof(float));
                            ma_pcm_rb_commit_write(&track.rb, c);
                        }
                    }
                }
            }
        }
        track.lastSequenceNumber = seqNum;
        track.firstPacket = false;

        float pcm[OPUS_FRAME_SIZE];
        if (track.decoder && track.decoder->Decode(opusData.data(), opusData.size(), pcm)) {
            ma_uint32 avail = ma_pcm_rb_available_read(&track.rb);
            if (avail > 96000) ma_pcm_rb_seek_read(&track.rb, avail - 48000);

            void* pW; ma_uint32 frames = OPUS_FRAME_SIZE;
            if (ma_pcm_rb_acquire_write(&track.rb, &frames, &pW) == MA_SUCCESS) {
                std::memcpy(pW, pcm, frames * sizeof(float));
                ma_pcm_rb_commit_write(&track.rb, frames);
            }
        }
    }

    void AudioEngine::PushIncomingAudio(const std::string& userId, const std::vector<uint8_t>& data) {
        PushIncomingAudioWithSequence(userId, data, 0);
    }

    void AudioEngine::Shutdown() {
        if (m_Internal) {
            ma_device_uninit(&m_Internal->device);
            ma_pcm_rb_uninit(&m_Internal->captureRb);
            for (int i = 0; i < MAX_TRACKS; ++i) {
                ma_pcm_rb_uninit(&m_Internal->tracks[i].rb);
                m_Internal->tracks[i].decoder.reset();
            }
            m_Internal->encoder.reset();
        }
    }
}