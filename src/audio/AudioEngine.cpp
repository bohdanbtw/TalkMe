#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "AudioEngine.h"
#include "../core/Logger.h"
#include "../shared/Protocol.h" 
#include <vector>
#include <cstring> 
#include <map>
#include <string>
#include <cmath>
#include <algorithm> 
#include <chrono>
#include <mutex>
#include <atomic>
#include <sstream>

#include <opus/opus.h> 

#define MINIAUDIO_IMPLEMENTATION
#include "../../vendor/miniaudio.h" 

namespace TalkMe {

    static constexpr int MAX_TRACKS = 100;
    static constexpr int OPUS_FRAME_SIZE = 480;
    static constexpr int SAMPLE_RATE = 48000;
    static constexpr float SILENCE_THRESHOLD = 0.0001f;
    static constexpr int AUDIO_LOG_SAMPLE_EVERY = 100;
    static constexpr int VOICE_TRACE_SAMPLE_EVERY = 50;

    class OpusEncoderWrapper {
    public:
        OpusEncoderWrapper() {
            int err;
            encoder = opus_encoder_create(SAMPLE_RATE, 1, OPUS_APPLICATION_VOIP, &err);
            if (err == OPUS_OK) {
                opus_encoder_ctl(encoder, OPUS_SET_BITRATE(32000));
                opus_encoder_ctl(encoder, OPUS_SET_COMPLEXITY(10));
                opus_encoder_ctl(encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
                opus_encoder_ctl(encoder, OPUS_SET_DTX(0));
                opus_encoder_ctl(encoder, OPUS_SET_VBR(1));
                opus_encoder_ctl(encoder, OPUS_SET_INBAND_FEC(1));
                opus_encoder_ctl(encoder, OPUS_SET_PACKET_LOSS_PERC(10));
                currentBitrate = 32000;
                LOG_AUDIO("Encoder created successfully");
            }
            else {
                std::stringstream ss;
                ss << "Encoder creation failed with error code: " << err;
                LOG_ERROR(ss.str());
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

        void AdjustBitrate(float packetLossPercent) {
            int newBitrate = 32000;
            if (packetLossPercent > 15.0f) newBitrate = 24000;
            else if (packetLossPercent > 5.0f) newBitrate = 28000;
            else if (packetLossPercent > 2.0f) newBitrate = 32000;
            else if (packetLossPercent < 0.5f) newBitrate = 48000;

            if (newBitrate != currentBitrate) {
                opus_encoder_ctl(encoder, OPUS_SET_BITRATE(newBitrate));
                currentBitrate = newBitrate;
                std::stringstream ss;
                ss << "Encoder bitrate adjusted to " << (newBitrate / 1000)
                    << "kbps (packet loss: " << packetLossPercent << "%)";
                LOG_AUDIO(ss.str());
            }
        }

        void SetPacketLossPercentage(float lossPercent) {
            opus_encoder_ctl(encoder, OPUS_SET_PACKET_LOSS_PERC((int)lossPercent));
        }

        void SetTargetBitrate(int bitrate) {
            if (!encoder) return;
            int clamped = std::clamp(bitrate, 12000, 64000);
            if (clamped != currentBitrate) {
                opus_encoder_ctl(encoder, OPUS_SET_BITRATE(clamped));
                currentBitrate = clamped;
            }
        }

        int GetCurrentBitrate() const { return currentBitrate; }

    private:
        OpusEncoder* encoder = nullptr;
        int currentBitrate = 32000;
    };

    class OpusDecoderWrapper {
    public:
        OpusDecoderWrapper() {
            int err;
            decoder = opus_decoder_create(SAMPLE_RATE, 1, &err);
            if (err != OPUS_OK) {
                std::stringstream ss;
                ss << "Decoder creation failed with error code: " << err;
                LOG_ERROR(ss.str());
            }
            else {
                LOG_AUDIO("Decoder created successfully");
            }
        }
        ~OpusDecoderWrapper() { if (decoder) opus_decoder_destroy(decoder); }

        int DecodeWithDiagnostics(const uint8_t* data, size_t len, float* pcmOut) {
            if (!decoder) return -1;
            int samples = opus_decode_float(decoder, data, (int)len, pcmOut, OPUS_FRAME_SIZE, 0);
            if (samples != OPUS_FRAME_SIZE) {
                std::stringstream ss;
                ss << "Opus decode error: returned " << samples << " samples (expected " << OPUS_FRAME_SIZE
                    << "), input_size=" << len;
                LOG_ERROR(ss.str());
            }
            return samples;
        }

        int DecodeLossWithDiagnostics(float* pcmOut) {
            if (!decoder) return -1;
            int samples = opus_decode_float(decoder, nullptr, 0, pcmOut, OPUS_FRAME_SIZE, 0);
            if (samples == OPUS_FRAME_SIZE) LOG_AUDIO("Packet loss concealment applied");
            return samples;
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
        bool coldStart = true;
        std::atomic<float> gain{ 1.0f };
        double smoothedBufferLevelMs = 0.0;
    };

    struct AudioInternal {
        ma_device device;
        ma_device_config config;
        ma_pcm_rb captureRb;
        VoiceTrack tracks[MAX_TRACKS];
        std::unique_ptr<OpusEncoderWrapper> encoder;
        std::function<void(const std::vector<uint8_t>&, uint32_t)> onMicData;

        std::function<void(const TalkMe::ReceiverReportPayload&)> onReceiverReport;
        std::chrono::steady_clock::time_point lastReceiverReportTime = std::chrono::steady_clock::now();

        uint32_t highestSeqReceived = 0;
        int intervalPacketsReceived = 0;
        int intervalPacketsLost = 0;

        float currentGain = 1.0f;
        float targetGain = 1.0f;
        float captureRMS = 0.0f;
        const float VAD_THRESHOLD = 0.002f;
        uint32_t outgoingSeqNum = 0;

        int totalPacketsReceived = 0;
        int totalPacketsLost = 0;
        int totalPacketsDuplicated = 0;
        int bufferUnderruns = 0;
        int bufferOverflows = 0;
        double avgJitterMs = 0.0;
        double currentLatencyMs = 0.0;
        double maxJitterSpikeMs = 0.0;
        double packetLossPercentage = 0.0;

        int remoteMemberCount = 0;
        int targetBufferMs = 150;
        int minBufferMs = 80;
        int maxBufferMs = 300;
        int adaptiveBufferLevel = 150;

        int currentEncoderBitrate = 32000;
        float currentVoiceActivityLevel = 0.0f;

        std::map<std::string, uint32_t> lastSeq;
        std::map<std::string, std::chrono::steady_clock::time_point> lastArrival;
        bool deviceStarted = false;
        std::atomic<bool> selfDeafened{ false };
        int callbackInvocations = 0;
        std::chrono::steady_clock::time_point lastKeepaliveTime = std::chrono::steady_clock::now();
        int keepaliveIntervalMs = 8000;
        std::mutex m_GainMutex;
        std::map<std::string, float> m_UserGains;
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

    bool IsVoiceActive(const float* pcm, int samples) {
        float energy = 0.0f;
        for (int i = 0; i < samples; i++) energy += pcm[i] * pcm[i];
        return (energy / samples) > SILENCE_THRESHOLD;
    }

    void DataCallback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount) {
        auto* internal = (AudioInternal*)pDevice->pUserData;
        if (!internal) return;

        internal->callbackInvocations++;
        if (internal->callbackInvocations == 1) LOG_AUDIO("Audio callback started");

        if (pInput && internal->onMicData) {
            const float* pInputFloat = (const float*)pInput;
            float rms = CalculateRMS(pInputFloat, frameCount);
            internal->captureRMS = rms * 0.2f + internal->captureRMS * 0.8f;
            internal->currentVoiceActivityLevel = internal->captureRMS;

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
        if (internal->selfDeafened.load(std::memory_order_relaxed)) return;

        int activeCount = 0;
        float mixBuffer[4096];
        if (frameCount > 4096) return;
        std::memset(mixBuffer, 0, frameCount * sizeof(float));

        for (int i = 0; i < MAX_TRACKS; ++i) {
            if (!internal->tracks[i].active) continue;
            ma_uint32 available = ma_pcm_rb_available_read(&internal->tracks[i].rb);
            int targetMs = internal->adaptiveBufferLevel;

            if (internal->tracks[i].isBuffering) {
                int recoveryMs = (std::max)(40, (std::min)(static_cast<int>(internal->maxJitterSpikeMs) + 20, targetMs));
                int thresholdMs = internal->tracks[i].coldStart ? targetMs : recoveryMs;

                ma_uint32 requiredFrames = (ma_uint32)((thresholdMs * SAMPLE_RATE) / 1000);
                if (requiredFrames < OPUS_FRAME_SIZE) requiredFrames = OPUS_FRAME_SIZE;
                if (requiredFrames > 48000) requiredFrames = 48000;

                if (available < requiredFrames) continue;

                internal->tracks[i].isBuffering = false;
                internal->tracks[i].smoothedBufferLevelMs = (available * 1000.0) / SAMPLE_RATE;
            } else {
                double availMs = (available * 1000.0) / SAMPLE_RATE;
                internal->tracks[i].smoothedBufferLevelMs = internal->tracks[i].smoothedBufferLevelMs * 0.998 + availMs * 0.002;

                if (internal->tracks[i].smoothedBufferLevelMs > targetMs + 40.0 && available >= OPUS_FRAME_SIZE * 2) {
                    void* pRead;
                    ma_uint32 chunk = OPUS_FRAME_SIZE;
                    if (ma_pcm_rb_acquire_read(&internal->tracks[i].rb, &chunk, &pRead) == MA_SUCCESS) {
                        ma_pcm_rb_commit_read(&internal->tracks[i].rb, chunk);
                        internal->tracks[i].smoothedBufferLevelMs -= 10.0;
                        available -= OPUS_FRAME_SIZE;
                    }
                }
                else if (internal->tracks[i].smoothedBufferLevelMs < targetMs - 30.0 && available > 0 && available < OPUS_FRAME_SIZE * 3) {
                    internal->tracks[i].smoothedBufferLevelMs += 10.0;
                    continue;
                }
            }

            if (available < frameCount) {
                internal->bufferUnderruns++;
                internal->tracks[i].isBuffering = true;
                internal->tracks[i].coldStart = false;

                internal->adaptiveBufferLevel = (std::min)(internal->adaptiveBufferLevel + 20, internal->maxBufferMs);
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
            float gain = internal->tracks[i].gain.load(std::memory_order_relaxed);
            for (ma_uint32 s = 0; s < read; ++s) mixBuffer[s] += trackBuffer[s] * gain;
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

    void AudioEngine::SetReceiverReportCallback(std::function<void(const TalkMe::ReceiverReportPayload&)> callback) {
        if (m_Internal) m_Internal->onReceiverReport = std::move(callback);
    }

    bool AudioEngine::InitializeWithSequence(std::function<void(const std::vector<uint8_t>&, uint32_t)> onMicDataCaptured) {
        m_Internal->onMicData = onMicDataCaptured;
        m_Internal->encoder = std::make_unique<OpusEncoderWrapper>();
        ma_uint32 bufferFrames = SAMPLE_RATE * 1;
        if (ma_pcm_rb_init(ma_format_f32, 1, bufferFrames, nullptr, nullptr, &m_Internal->captureRb) != MA_SUCCESS) {
            LOG_ERROR("Failed to initialize capture ring buffer");
            return false;
        }
        for (int i = 0; i < MAX_TRACKS; ++i) {
            if (ma_pcm_rb_init(ma_format_f32, 1, bufferFrames, nullptr, nullptr, &m_Internal->tracks[i].rb) != MA_SUCCESS) {
                return false;
            }
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
        if (ma_device_init(nullptr, &m_Internal->config, &m_Internal->device) != MA_SUCCESS) {
            return false;
        }
        if (ma_device_start(&m_Internal->device) == MA_SUCCESS) {
            m_Internal->deviceStarted = true;
            return true;
        }
        return false;
    }

    bool AudioEngine::Initialize(std::function<void(const std::vector<uint8_t>&)> onMicDataCaptured) {
        return InitializeWithSequence([onMicDataCaptured](const std::vector<uint8_t>& d, uint32_t s) { onMicDataCaptured(d); });
    }

    void AudioEngine::Update() {
        if (!m_Internal || !m_Internal->onMicData) return;

        if (!m_CaptureEnabled) {
            ma_uint32 available = ma_pcm_rb_available_read(&m_Internal->captureRb);
            while (available >= OPUS_FRAME_SIZE) {
                void* pRead;
                ma_uint32 framesRead = OPUS_FRAME_SIZE;
                if (ma_pcm_rb_acquire_read(&m_Internal->captureRb, &framesRead, &pRead) != MA_SUCCESS) break;
                ma_pcm_rb_commit_read(&m_Internal->captureRb, framesRead);
                available -= framesRead;
            }
            return;
        }

        auto now = std::chrono::steady_clock::now();

        int rrElapsedMs = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(now - m_Internal->lastReceiverReportTime).count());
        if (rrElapsedMs >= 2000) {
            if (m_Internal->onReceiverReport) {
                TalkMe::ReceiverReportPayload rr{};
                rr.highestSequenceReceived = m_Internal->highestSeqReceived;
                rr.packetsLost = static_cast<uint32_t>(m_Internal->totalPacketsLost);
                rr.jitterMs = static_cast<uint32_t>(m_Internal->avgJitterMs);

                int expected = m_Internal->intervalPacketsReceived + m_Internal->intervalPacketsLost;
                rr.fractionLost = (expected > 0) ? static_cast<uint8_t>((std::min)((static_cast<float>(m_Internal->intervalPacketsLost) / expected) * 256.0f, 255.0f)) : 0;

                m_Internal->onReceiverReport(rr);
            }

            m_Internal->intervalPacketsReceived = 0;
            m_Internal->intervalPacketsLost = 0;

            // Slow-Decay: Gradually relax the buffer as the network stabilizes
            m_Internal->maxJitterSpikeMs *= 0.9;
            m_Internal->lastReceiverReportTime = now;
        }

        int elapsedMs = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(now - m_Internal->lastKeepaliveTime).count());
        if (elapsedMs >= m_Internal->keepaliveIntervalMs) {
            m_Internal->lastKeepaliveTime = now;
            float silence[OPUS_FRAME_SIZE] = { 0 };
            std::vector<uint8_t> packet = m_Internal->encoder->Encode(silence);
            if (!packet.empty())
                m_Internal->onMicData(packet, m_Internal->outgoingSeqNum++);
        }

        ma_uint32 available = ma_pcm_rb_available_read(&m_Internal->captureRb);
        if (available >= OPUS_FRAME_SIZE) {
            float pcmBuffer[OPUS_FRAME_SIZE];
            void* pRead;
            ma_uint32 framesRead = OPUS_FRAME_SIZE;
            if (ma_pcm_rb_acquire_read(&m_Internal->captureRb, &framesRead, &pRead) == MA_SUCCESS) {
                std::memcpy(pcmBuffer, pRead, framesRead * sizeof(float));
                ma_pcm_rb_commit_read(&m_Internal->captureRb, framesRead);

                bool hasVoice = IsVoiceActive(pcmBuffer, OPUS_FRAME_SIZE);
                if (hasVoice && !m_SelfMuted) {
                    std::vector<uint8_t> packet = m_Internal->encoder->Encode(pcmBuffer);
                    if (!packet.empty()) {
                        m_Internal->onMicData(packet, m_Internal->outgoingSeqNum++);
                    }
                }
            }
        }
    }

    void AudioEngine::PushIncomingAudioWithSequence(const std::string& userId, const std::vector<uint8_t>& opusData, uint32_t seqNum) {
        if (!m_Internal) return;
        auto now = std::chrono::steady_clock::now();

        if (seqNum > m_Internal->highestSeqReceived) {
            m_Internal->highestSeqReceived = seqNum;
        }

        auto itLast = m_Internal->lastSeq.find(userId);
        if (itLast != m_Internal->lastSeq.end()) {
            uint32_t last = itLast->second;

            if (seqNum == last) {
                m_Internal->totalPacketsDuplicated++;
                return;
            }

            if (seqNum < last) return;

            uint32_t expectedSeq = last + 1;
            if (seqNum > expectedSeq) {
                int lost = seqNum - expectedSeq;
                m_Internal->totalPacketsLost += lost;
                m_Internal->intervalPacketsLost += lost;
            }
        }

        m_Internal->totalPacketsReceived++;
        m_Internal->intervalPacketsReceived++;

        auto itTime = m_Internal->lastArrival.find(userId);
        if (itTime != m_Internal->lastArrival.end() && itLast != m_Internal->lastSeq.end()) {
            uint32_t seqDiff = seqNum - itLast->second;
            if (seqDiff > 0 && seqDiff < 100) {
                double expectedDeltaMs = seqDiff * 10.0;
                double actualDeltaMs = static_cast<double>(std::chrono::duration_cast<std::chrono::milliseconds>(now - itTime->second).count());
                double transitDiff = std::abs(actualDeltaMs - expectedDeltaMs);
                if (transitDiff <= 200.0) {
                    if (transitDiff > m_Internal->avgJitterMs) {
                        m_Internal->maxJitterSpikeMs = (std::max)(m_Internal->maxJitterSpikeMs, transitDiff);
                    }
                    m_Internal->avgJitterMs = m_Internal->avgJitterMs * 0.9375 + transitDiff * 0.0625;
                }
                m_Internal->currentLatencyMs = actualDeltaMs;
            }
        }
        m_Internal->lastSeq[userId] = seqNum;
        m_Internal->lastArrival[userId] = now;

        int trackIdx = -1;
        for (int i = 0; i < MAX_TRACKS; ++i) {
            if (m_Internal->tracks[i].active && m_Internal->tracks[i].userId == userId) {
                trackIdx = i;
                break;
            }
        }

        if (trackIdx == -1) {
            for (int i = 0; i < MAX_TRACKS; ++i) {
                if (!m_Internal->tracks[i].active) {
                    m_Internal->tracks[i].userId = userId;
                    m_Internal->tracks[i].active = true;
                    m_Internal->tracks[i].decoder = std::make_unique<OpusDecoderWrapper>();
                    m_Internal->tracks[i].firstPacket = true;
                    m_Internal->tracks[i].isBuffering = true;
                    m_Internal->tracks[i].coldStart = true;
                    m_Internal->tracks[i].lastSequenceNumber = seqNum - 1;
                    {
                        std::lock_guard<std::mutex> lock(m_Internal->m_GainMutex);
                        auto itGain = m_Internal->m_UserGains.find(userId);
                        m_Internal->tracks[i].gain.store(
                            itGain != m_Internal->m_UserGains.end() ? itGain->second : 1.0f,
                            std::memory_order_relaxed);
                    }
                    trackIdx = i;
                    break;
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
                    int samples = track.decoder->DecodeLossWithDiagnostics(plc);
                    if (samples == OPUS_FRAME_SIZE) {
                        void* pW;
                        ma_uint32 c = OPUS_FRAME_SIZE;
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
        int samples = track.decoder->DecodeWithDiagnostics(opusData.data(), opusData.size(), pcm);
        if (samples == OPUS_FRAME_SIZE) {
            ma_uint32 avail = ma_pcm_rb_available_read(&track.rb);
            if (avail > 96000) {
                ma_uint32 toSeek = avail - 48000;
                ma_pcm_rb_seek_read(&track.rb, toSeek);
                m_Internal->bufferOverflows++;
            }

            void* pW;
            ma_uint32 frames = OPUS_FRAME_SIZE;
            if (ma_pcm_rb_acquire_write(&track.rb, &frames, &pW) == MA_SUCCESS) {
                std::memcpy(pW, pcm, frames * sizeof(float));
                ma_pcm_rb_commit_write(&track.rb, frames);
            }
        }
    }

    void AudioEngine::PushIncomingAudio(const std::string& userId, const std::vector<uint8_t>& data) {
        PushIncomingAudioWithSequence(userId, data, 0);
    }

    void AudioEngine::ClearRemoteTracks() {
        if (!m_Internal) return;
        for (int i = 0; i < MAX_TRACKS; ++i) {
            auto& t = m_Internal->tracks[i];
            ma_uint32 avail = ma_pcm_rb_available_read(&t.rb);
            while (avail > 0) {
                ma_uint32 chunk = avail;
                void* pRead;
                if (ma_pcm_rb_acquire_read(&t.rb, &chunk, &pRead) != MA_SUCCESS) break;
                ma_pcm_rb_commit_read(&t.rb, chunk);
                avail -= chunk;
            }
            t.active = false;
            t.decoder.reset();
            t.userId.clear();
            t.firstPacket = true;
            t.isBuffering = true;
            t.coldStart = true;
            t.lastSequenceNumber = 0;
            t.smoothedBufferLevelMs = 0.0;
            t.gain.store(1.0f, std::memory_order_relaxed);
        }
        m_Internal->lastSeq.clear();
        m_Internal->lastArrival.clear();
    }

    void AudioEngine::RemoveUserTrack(const std::string& userId) {
        if (!m_Internal || userId.empty()) return;
        for (int i = 0; i < MAX_TRACKS; ++i) {
            auto& t = m_Internal->tracks[i];
            if (!t.active || t.userId != userId) continue;
            ma_uint32 avail = ma_pcm_rb_available_read(&t.rb);
            while (avail > 0) {
                ma_uint32 chunk = avail;
                void* pRead;
                if (ma_pcm_rb_acquire_read(&t.rb, &chunk, &pRead) != MA_SUCCESS) break;
                ma_pcm_rb_commit_read(&t.rb, chunk);
                avail -= chunk;
            }
            t.active = false;
            t.decoder.reset();
            t.userId.clear();
            t.firstPacket = true;
            t.isBuffering = true;
            t.coldStart = true;
            t.lastSequenceNumber = 0;
            t.smoothedBufferLevelMs = 0.0;
            t.gain.store(1.0f, std::memory_order_relaxed);
        }
        m_Internal->lastSeq.erase(userId);
        m_Internal->lastArrival.erase(userId);
    }

    void AudioEngine::ApplyConfig(int targetBufferMs, int minBufferMs, int maxBufferMs, int keepaliveIntervalMs, int targetBitrateKbps) {
        if (!m_Internal) return;
        if (targetBufferMs >= 0) { m_Internal->targetBufferMs = targetBufferMs; m_Internal->adaptiveBufferLevel = targetBufferMs; }
        if (minBufferMs >= 0) m_Internal->minBufferMs = minBufferMs;
        if (maxBufferMs >= 0) m_Internal->maxBufferMs = maxBufferMs;
        if (keepaliveIntervalMs >= 0) m_Internal->keepaliveIntervalMs = keepaliveIntervalMs;
        if (targetBitrateKbps > 0 && m_Internal->encoder) {
            m_Internal->encoder->SetTargetBitrate(targetBitrateKbps * 1000);
            m_Internal->currentEncoderBitrate = m_Internal->encoder->GetCurrentBitrate();
        }
    }

    void AudioEngine::SetUserGain(const std::string& userId, float gain) {
        if (!m_Internal) return;
        {
            std::lock_guard<std::mutex> lock(m_Internal->m_GainMutex);
            m_Internal->m_UserGains[userId] = gain;
        }
        for (int i = 0; i < MAX_TRACKS; ++i) {
            if (m_Internal->tracks[i].active && m_Internal->tracks[i].userId == userId) {
                m_Internal->tracks[i].gain.store(gain, std::memory_order_relaxed);
                break;
            }
        }
    }

    float AudioEngine::GetMicActivity() const {
        if (!m_Internal) return 0.0f;
        return m_Internal->captureRMS;
    }

    void AudioEngine::Shutdown() {
        if (m_Internal) {
            if (m_Internal->deviceStarted) ma_device_uninit(&m_Internal->device);
            ma_pcm_rb_uninit(&m_Internal->captureRb);
            for (int i = 0; i < MAX_TRACKS; ++i) {
                ma_pcm_rb_uninit(&m_Internal->tracks[i].rb);
                m_Internal->tracks[i].decoder.reset();
            }
            m_Internal->encoder.reset();
        }
    }

    void AudioEngine::OnVoiceStateUpdate(int memberCount) {
        if (!m_Internal) return;
        m_Internal->remoteMemberCount = memberCount;
        int base = 100;
        int jitterMs = (int)std::ceil(m_Internal->avgJitterMs);
        int memberBonus = memberCount * 15;
        int adapt = base + jitterMs + memberBonus;
        if (adapt > m_Internal->maxBufferMs) adapt = m_Internal->maxBufferMs;
        if (adapt < m_Internal->minBufferMs) adapt = m_Internal->minBufferMs;
        m_Internal->adaptiveBufferLevel = adapt;
    }

    void AudioEngine::OnNetworkConditions(float packetLossPercent, float avgLatencyMs) {
        if (!m_Internal) return;
        int totalSamples = m_Internal->totalPacketsReceived + m_Internal->totalPacketsLost;
        if (totalSamples > 0) {
            m_Internal->packetLossPercentage = (100.0f * m_Internal->totalPacketsLost) / totalSamples;
        } else {
            m_Internal->packetLossPercentage = packetLossPercent;
        }

        // Utilize the maximum spike, not just the smoothed average
        int effectiveJitterMs = static_cast<int>(std::ceil((std::max)(m_Internal->avgJitterMs, m_Internal->maxJitterSpikeMs)));
        int lossBurstMs = static_cast<int>(m_Internal->packetLossPercentage * 1.5f);
        int targetMs = static_cast<int>(avgLatencyMs) + 3 * effectiveJitterMs + lossBurstMs + 20;

        targetMs = (std::max)(targetMs, m_Internal->minBufferMs);
        targetMs = (std::min)(targetMs, m_Internal->maxBufferMs);
        m_Internal->adaptiveBufferLevel = targetMs;

        if (m_Internal->encoder) {
            m_Internal->encoder->AdjustBitrate(static_cast<float>(m_Internal->packetLossPercentage));
            m_Internal->encoder->SetPacketLossPercentage(static_cast<float>(m_Internal->packetLossPercentage));
            m_Internal->currentEncoderBitrate = m_Internal->encoder->GetCurrentBitrate();
        }
    }

    void AudioEngine::ApplyProbeResults(float rttMs, float jitterMs, float lossPct) {
        if (!m_Internal) return;
        m_Internal->avgJitterMs = jitterMs;
        m_Internal->maxJitterSpikeMs = jitterMs; // Initialize spike tracker
        m_Internal->packetLossPercentage = lossPct;

        float oneWayMs = rttMs / 2.f;
        int effectiveJitterMs = static_cast<int>(std::ceil(jitterMs));
        int lossBurstMs = static_cast<int>(lossPct * 1.5f);
        int targetMs = static_cast<int>(oneWayMs) + 3 * effectiveJitterMs + lossBurstMs + 20;

        targetMs = (std::max)(targetMs, m_Internal->minBufferMs);
        targetMs = (std::min)(targetMs, m_Internal->maxBufferMs);
        m_Internal->adaptiveBufferLevel = targetMs;

        if (m_Internal->encoder) {
            m_Internal->encoder->AdjustBitrate(lossPct);
            m_Internal->encoder->SetPacketLossPercentage(lossPct);
        }
    }

    void AudioEngine::SetSelfDeafened(bool deafened) {
        m_SelfDeafened = deafened;
        if (deafened) m_SelfMuted = true;
        if (m_Internal) m_Internal->selfDeafened.store(deafened, std::memory_order_relaxed);
    }

    std::vector<AudioDeviceInfo> AudioEngine::GetInputDevices() {
        std::vector<AudioDeviceInfo> result;
        ma_context ctx;
        if (ma_context_init(nullptr, 0, nullptr, &ctx) != MA_SUCCESS) return result;
        ma_device_info* pCapture; ma_uint32 captureCount;
        ma_device_info* pPlayback; ma_uint32 playbackCount;
        if (ma_context_get_devices(&ctx, &pPlayback, &playbackCount, &pCapture, &captureCount) == MA_SUCCESS) {
            for (ma_uint32 i = 0; i < captureCount; i++) {
                AudioDeviceInfo d;
                d.name = pCapture[i].name;
                d.index = (int)i;
                d.isDefault = pCapture[i].isDefault != 0;
                result.push_back(d);
            }
        }
        ma_context_uninit(&ctx);
        return result;
    }

    std::vector<AudioDeviceInfo> AudioEngine::GetOutputDevices() {
        std::vector<AudioDeviceInfo> result;
        ma_context ctx;
        if (ma_context_init(nullptr, 0, nullptr, &ctx) != MA_SUCCESS) return result;
        ma_device_info* pCapture; ma_uint32 captureCount;
        ma_device_info* pPlayback; ma_uint32 playbackCount;
        if (ma_context_get_devices(&ctx, &pPlayback, &playbackCount, &pCapture, &captureCount) == MA_SUCCESS) {
            for (ma_uint32 i = 0; i < playbackCount; i++) {
                AudioDeviceInfo d;
                d.name = pPlayback[i].name;
                d.index = (int)i;
                d.isDefault = pPlayback[i].isDefault != 0;
                result.push_back(d);
            }
        }
        ma_context_uninit(&ctx);
        return result;
    }

    bool AudioEngine::ReinitDevice(int inputIdx, int outputIdx) {
        if (!m_Internal) return false;

        if (m_Internal->deviceStarted) {
            ma_device_stop(&m_Internal->device);
            ma_device_uninit(&m_Internal->device);
            m_Internal->deviceStarted = false;
        }

        ma_context ctx;
        if (ma_context_init(nullptr, 0, nullptr, &ctx) != MA_SUCCESS) return false;

        ma_device_info* pCapture; ma_uint32 captureCount;
        ma_device_info* pPlayback; ma_uint32 playbackCount;
        bool gotDevices = ma_context_get_devices(&ctx, &pPlayback, &playbackCount, &pCapture, &captureCount) == MA_SUCCESS;

        ma_device_id captureId{}, playbackId{};
        ma_device_id* pCaptureId = nullptr;
        ma_device_id* pPlaybackId = nullptr;

        if (gotDevices) {
            if (inputIdx >= 0 && (ma_uint32)inputIdx < captureCount) {
                captureId = pCapture[inputIdx].id;
                pCaptureId = &captureId;
            }
            if (outputIdx >= 0 && (ma_uint32)outputIdx < playbackCount) {
                playbackId = pPlayback[outputIdx].id;
                pPlaybackId = &playbackId;
            }
        }
        ma_context_uninit(&ctx);

        m_Internal->config = ma_device_config_init(ma_device_type_duplex);
        m_Internal->config.sampleRate = SAMPLE_RATE;
        m_Internal->config.capture.channels = 1;
        m_Internal->config.playback.channels = 1;
        m_Internal->config.capture.format = ma_format_f32;
        m_Internal->config.playback.format = ma_format_f32;
        m_Internal->config.periodSizeInFrames = OPUS_FRAME_SIZE;
        m_Internal->config.dataCallback = DataCallback;
        m_Internal->config.pUserData = m_Internal.get();
        m_Internal->config.capture.pDeviceID = pCaptureId;
        m_Internal->config.playback.pDeviceID = pPlaybackId;

        if (ma_device_init(nullptr, &m_Internal->config, &m_Internal->device) != MA_SUCCESS) return false;
        if (ma_device_start(&m_Internal->device) == MA_SUCCESS) {
            m_Internal->deviceStarted = true;
            return true;
        }
        return false;
    }

    AudioEngine::Telemetry AudioEngine::GetTelemetry() {
        Telemetry t;
        if (!m_Internal) return t;

        t.totalPacketsReceived = m_Internal->totalPacketsReceived;
        t.totalPacketsLost = m_Internal->totalPacketsLost;
        t.totalPacketsDuplicated = m_Internal->totalPacketsDuplicated;

        int totalSamples = m_Internal->totalPacketsReceived + m_Internal->totalPacketsLost;
        t.packetLossPercentage = (totalSamples > 0) ? (100.0f * m_Internal->totalPacketsLost) / totalSamples : 0.0f;

        t.avgJitterMs = (float)m_Internal->avgJitterMs;
        t.currentLatencyMs = (float)m_Internal->currentLatencyMs;

        t.bufferUnderruns = m_Internal->bufferUnderruns;
        t.bufferOverflows = m_Internal->bufferOverflows;
        t.currentBufferMs = m_Internal->adaptiveBufferLevel;
        t.targetBufferMs = m_Internal->targetBufferMs;

        t.remoteMemberCount = m_Internal->remoteMemberCount;
        t.adaptiveBufferLevel = m_Internal->adaptiveBufferLevel;

        t.currentEncoderBitrateKbps = m_Internal->currentEncoderBitrate / 1000;
        t.currentVoiceActivityLevel = m_Internal->currentVoiceActivityLevel;

        return t;
    }
}