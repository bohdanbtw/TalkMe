#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "AudioEngine.h"
#include "OpusCodec.h"
#include "../core/Logger.h"
#include "../shared/Protocol.h"
#include <vector>
#include <cstring>
#include <map>
#include <unordered_map>
#include <string>
#include <cmath>
#include <algorithm>
#include <chrono>
#include <mutex>
#include <atomic>
#include <array>
#include <thread>
#include <condition_variable>

#include <opus/opus.h>
#include <speex/speex_preprocess.h>
#ifdef TALKME_USE_RNNOISE
#include <rnnoise.h>
#endif
#ifdef TALKME_USE_WEBRTC_APM
#include <modules/audio_processing/include/audio_processing.h>
#endif

#include "AudioEngineDevice.h"
#include "NativeAudioProcessor.h"
#include "AudioEngineInternal.h"
#include "AudioEnginePlayback.h"
#include "../../vendor/miniaudio.h"

namespace TalkMe {

    static constexpr float SILENCE_THRESHOLD = 0.0001f;

    inline bool SeqLT(uint32_t a, uint32_t b) { return static_cast<int32_t>(a - b) < 0; }
    inline bool SeqGT(uint32_t a, uint32_t b) { return static_cast<int32_t>(a - b) > 0; }

    // -------------------------------------------------------------------------
    // DataCallback
    // Invoked by the miniaudio driver on a realtime hardware thread.
    // RULES:
    //   - No heap allocation (no malloc, new, vector resize that increases capacity).
    //   - No blocking: no mutex that any other thread can hold for more than a
    //     few microseconds — the audio period is only 10 ms.
    //   - m_TelemetryMutex is NOT acquired here; the atomic mirror
    //     maxJitterSpikeMsAtomic is used for the one value needed.
    //   - m_TracksMutex IS acquired for the playback mix loop.
    //   - m_EncodeCv.notify_one() is called WITHOUT holding m_EncodeMutex.
    //     The mutex is only required when waiting, not when notifying, and
    //     holding it here would risk a priority-inversion stall on the RT thread.
    // -------------------------------------------------------------------------
    void DataCallback(ma_device* pDevice, void* pOutput, const void* pInput,
        ma_uint32 frameCount)
    {
        auto* internal = static_cast<AudioInternal*>(pDevice->pUserData);
        if (!internal) return;

        try {
        internal->callbackInvocations++;
        if (internal->callbackInvocations == 1) LOG_AUDIO("Audio callback started");

        // --- Capture path ---------------------------------------------------
        const bool captureActive =
            pInput != nullptr &&
            internal->onMicData &&
            frameCount <= static_cast<ma_uint32>(internal->m_CaptureProcessBuffer.size());

        if (captureActive) {
            const float* pInputFloat = static_cast<const float*>(pInput);

            internal->captureRMS =
                ComputeRms(pInputFloat, static_cast<int>(frameCount)) * 0.2f +
                internal->captureRMS * 0.8f;

            const NoiseSuppressionMode mode =
                internal->m_Engine
                ? internal->m_Engine->GetNoiseSuppressionMode()
                : NoiseSuppressionMode::None;

            const bool needProcess =
                internal->m_Engine &&
                (internal->m_Engine->IsMicTestEnabled() || mode != NoiseSuppressionMode::None);

            if (needProcess) {
                float* processedMicBuffer = internal->m_CaptureProcessBuffer.data();
                std::memcpy(processedMicBuffer, pInputFloat, frameCount * sizeof(float));

                if (mode == NoiseSuppressionMode::SpeexDSP &&
                    internal->speexState &&
                    internal->speexPcm.size() == frameCount)
                {
                    for (ma_uint32 i = 0; i < frameCount; ++i) {
                        float s = processedMicBuffer[i] * 32768.0f;
                        internal->speexPcm[i] =
                            static_cast<int16_t>(std::clamp(s, -32768.0f, 32767.0f));
                    }
                    speex_preprocess_run(internal->speexState, internal->speexPcm.data());
                    for (ma_uint32 i = 0; i < frameCount; ++i)
                        processedMicBuffer[i] = internal->speexPcm[i] / 32768.0f;
                }
#ifdef TALKME_USE_RNNOISE
                else if (mode == NoiseSuppressionMode::RNNoise &&
                    internal->rnnoiseState &&
                    frameCount == OPUS_FRAME_SIZE)
                {
                    // rnnoise_process_frame works in-place with 32-bit floats
                    // scaled to the int16 range.
                    float rnnoiseBuffer[OPUS_FRAME_SIZE];
                    for (int i = 0; i < static_cast<int>(frameCount); ++i)
                        rnnoiseBuffer[i] = processedMicBuffer[i] * 32768.0f;
                    rnnoise_process_frame(internal->rnnoiseState,
                        rnnoiseBuffer, rnnoiseBuffer);
                    for (int i = 0; i < static_cast<int>(frameCount); ++i)
                        processedMicBuffer[i] = rnnoiseBuffer[i] / 32768.0f;
                }
#endif
#ifdef TALKME_USE_WEBRTC_APM
                else if (mode == NoiseSuppressionMode::WebRTC &&
                    internal->webrtcApm &&
                    frameCount == OPUS_FRAME_SIZE)
                {
                    webrtc::StreamConfig streamConfig(SAMPLE_RATE, 1);
                    const float* src[] = { processedMicBuffer };
                    float* dest[] = { processedMicBuffer };
                    internal->webrtcApm->ProcessStream(
                        src, streamConfig, streamConfig, dest);
                }
#endif
                else if (mode != NoiseSuppressionMode::None) {
                    const bool doGate = (mode == NoiseSuppressionMode::RNNoise);
                    const bool doApm = (mode == NoiseSuppressionMode::WebRTC);
                    internal->m_NativeProcessor.Process(
                        processedMicBuffer, static_cast<int>(frameCount), doGate, doApm);
                }

                pInputFloat = processedMicBuffer;
            }

            const float rms = ComputeRms(pInputFloat, static_cast<int>(frameCount));
            internal->processedRMS = rms * 0.2f + internal->processedRMS * 0.8f;
            internal->currentVoiceActivityLevel = internal->processedRMS;

            // Dynamic noise-floor tracking — no locking needed (only written here).
            if (rms < internal->noiseFloorRms * 1.5f)
                internal->noiseFloorRms = internal->noiseFloorRms * 0.995f + rms * 0.005f;
            else
                internal->noiseFloorRms = internal->noiseFloorRms * 0.9995f + rms * 0.0005f;

            const float vadThreshold = internal->noiseFloorRms * 3.981f;

            if (internal->processedRMS > vadThreshold) {
                ma_uint32 toWrite = frameCount;
                while (toWrite > 0) {
                    void* pWrite;
                    ma_uint32 chunk = toWrite;
                    if (ma_pcm_rb_acquire_write(&internal->captureRb, &chunk, &pWrite)
                        != MA_SUCCESS) break;
                    const float* src = pInputFloat + (frameCount - toWrite);
                    std::memcpy(pWrite, src, chunk * sizeof(float));
                    ma_pcm_rb_commit_write(&internal->captureRb, chunk);
                    toWrite -= chunk;
                }
                // notify_one does NOT require holding m_EncodeMutex.
                // Acquiring a mutex here would risk priority inversion on the RT thread.
                if (ma_pcm_rb_available_read(&internal->captureRb) >= OPUS_FRAME_SIZE)
                    internal->m_EncodeCv.notify_one();
            }
        }
        else if (pInput && internal->onMicData) {
            // Pre-allocated buffer was too small for this period (driver reported a
            // larger period than anticipated). Log once and skip; the buffer will be
            // resized on the next ReinitDevice call.
            LOG_ERROR_BUF("DataCallback: capture period > pre-allocated buffer — skipping");
        }

        // --- Playback path --------------------------------------------------
        float* pOutputFloat = static_cast<float*>(pOutput);
        MixTracks(internal, pOutputFloat, frameCount);

        if (internal->m_Engine &&
            internal->m_Engine->IsMicTestEnabled() &&
            pOutput &&
            internal->m_CaptureProcessBuffer.size() >= frameCount)
        {
            const float* src = internal->m_CaptureProcessBuffer.data();
            for (ma_uint32 i = 0; i < frameCount; ++i)
                pOutputFloat[i] += src[i];
        }
        }
        catch (...) {
            // Real-time callback: do not log (may allocate). Swallow to avoid std::terminate.
        }
    }

    // =========================================================================
    // AudioEngine
    // =========================================================================
    AudioEngine::AudioEngine() : m_Internal(std::make_unique<AudioInternal>()) {
#ifdef TALKME_USE_RNNOISE
        m_Internal->rnnoiseState = rnnoise_create(nullptr);
        m_RNNoiseState = m_Internal->rnnoiseState;
#endif
#ifdef TALKME_USE_WEBRTC_APM
        // AudioProcessingBuilder::Create() returns rtc::scoped_refptr<AudioProcessing>.
        // We take a raw observer pointer for the internal struct; the scoped_refptr
        // itself must be stored to manage the refcount.  Store it as a member.
        // IMPORTANT: change m_WebRtcApmRef (below) to rtc::scoped_refptr<> in AudioEngine.h
        // if/when the WebRTC APM path is productionised.
        auto apmRef = webrtc::AudioProcessingBuilder().Create();
        m_Internal->webrtcApm = apmRef.get();
        if (m_Internal->webrtcApm) {
            webrtc::AudioProcessing::Config apmConfig;
            apmConfig.high_pass_filter.enabled = true;
            apmConfig.noise_suppression.enabled = true;
            apmConfig.noise_suppression.level =
                webrtc::AudioProcessing::Config::NoiseSuppression::kHigh;
            apmConfig.gain_controller1.enabled = true;
            apmConfig.gain_controller1.mode =
                webrtc::AudioProcessing::Config::GainController1::kAdaptiveDigital;
            m_Internal->webrtcApm->ApplyConfig(apmConfig);
        }
        m_WebRtcApm = m_Internal->webrtcApm;
        // Keep the scoped_refptr alive for the engine's lifetime.
        m_WebRtcApmRef = std::move(apmRef);
#endif
    }

    AudioEngine::~AudioEngine() { Shutdown(); }

    void AudioEngine::SetNoiseSuppressionMode(NoiseSuppressionMode mode) {
        m_NoiseSuppressionMode = mode;
    }
    NoiseSuppressionMode AudioEngine::GetNoiseSuppressionMode() const {
        return m_NoiseSuppressionMode;
    }
    void AudioEngine::SetMicTestEnabled(bool enabled) { m_MicTestEnabled = enabled; }

    void AudioEngine::SetReceiverReportCallback(
        std::function<void(const TalkMe::ReceiverReportPayload&)> callback)
    {
        if (m_Internal) m_Internal->onReceiverReport = std::move(callback);
    }

    namespace {
        void ResetVoiceTrackForPool(VoiceTrack* tr) {
            tr->userId.clear();
            tr->active = false;
            tr->firstPacket = true;
            tr->isBuffering = true;
            tr->coldStart = true;
            tr->lastSequenceNumber = 0;
            tr->gain.store(1.0f, std::memory_order_relaxed);
            tr->smoothedBufferLevelMs = 0.0;
        }
    }

    bool AudioEngine::InitializeWithSequence(
        std::function<void(const std::vector<uint8_t>&, uint32_t)> onMicDataCaptured)
    {
        m_Internal->onMicData = std::move(onMicDataCaptured);
        m_Internal->encoder = std::make_unique<OpusEncoderWrapper>();

        // Pre-allocate VoiceTrack pool so new users don't allocate on the hot path.
        // Protect pool access with mutex (Bug #2: race condition fix)
        {
            std::lock_guard<std::mutex> lk(m_Internal->m_TracksMutex);
            if (m_Internal->trackPool.empty()) {
                for (size_t i = 0; i < AudioInternal::kVoiceTrackPoolSize; ++i)
                    m_Internal->trackPool.push_back(std::make_unique<VoiceTrack>());
            }
        }

        const ma_uint32 bufferFrames = SAMPLE_RATE * 1;
        if (ma_pcm_rb_init(ma_format_f32, 1, bufferFrames, nullptr, nullptr,
            &m_Internal->captureRb) != MA_SUCCESS)
        {
            LOG_ERROR("Failed to initialize capture ring buffer");
            return false;
        }

        {
            std::lock_guard<std::mutex> lk(m_Internal->m_TracksMutex);
            m_Internal->tracks.clear();
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
        m_Internal->m_Engine = this;

        const int requestedPeriod = static_cast<int>(m_Internal->config.periodSizeInFrames);

        if (m_Internal->speexState) {
            speex_preprocess_state_destroy(m_Internal->speexState);
            m_Internal->speexState = nullptr;
        }
        m_Internal->speexState = speex_preprocess_state_init(requestedPeriod, SAMPLE_RATE);
        if (m_Internal->speexState) {
            int denoise = 1;
            speex_preprocess_ctl(m_Internal->speexState,
                SPEEX_PREPROCESS_SET_DENOISE, &denoise);
        }
        m_Internal->speexPcm.resize(static_cast<size_t>(requestedPeriod));

        if (ma_device_init(nullptr, &m_Internal->config, &m_Internal->device) != MA_SUCCESS)
            return false;

        // After device init the driver may have negotiated a period larger than
        // requested.  Pre-allocate all audio-thread buffers to 2x the actual
        // period now so that DataCallback / MixTracks never call resize().
        {
            ma_uint32 actualPeriod =
                m_Internal->device.capture.internalPeriodSizeInFrames;
            if (actualPeriod == 0)
                actualPeriod = static_cast<ma_uint32>(requestedPeriod);
            const ma_uint32 prealloc = actualPeriod * 2;
            m_Internal->m_CaptureProcessBuffer.assign(prealloc, 0.0f);
            m_Internal->mixBuffer.assign(prealloc, 0.0f);
            m_Internal->m_PerTrackBuffer.assign(prealloc, 0.0f);
        }

        m_Internal->m_EncodeWorkBuffer.reserve(kOpusMaxPacket);
        m_Internal->m_EncodeWorkBuffer.resize(kOpusMaxPacket);

        if (ma_device_start(&m_Internal->device) != MA_SUCCESS) return false;
        m_Internal->deviceStarted = true;

        m_Internal->m_EncodeThreadShutdown.store(false, std::memory_order_relaxed);
        m_Internal->m_EncodeThread = std::thread(EncodeThreadFunc, m_Internal.get());
        return true;
    }

    void AudioEngine::Update() {
        if (!m_Internal || !m_Internal->onMicData) return;

        const auto now = std::chrono::steady_clock::now();

        const int rrElapsedMs = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                now - m_Internal->lastReceiverReportTime).count());

        if (rrElapsedMs >= 2000) {
            if (m_Internal->onReceiverReport) {
                TalkMe::ReceiverReportPayload rr{};
                rr.highestSequenceReceived = m_Internal->highestSeqReceived;
                rr.packetsLost = static_cast<uint32_t>(
                    m_Internal->totalPacketsLost.load(std::memory_order_relaxed));
                {
                    std::lock_guard<std::mutex> lock(m_Internal->m_TelemetryMutex);
                    rr.jitterMs = static_cast<uint32_t>(m_Internal->avgJitterMs);
                }

                const int rxSnap = m_Internal->intervalPacketsReceived.exchange(0, std::memory_order_relaxed);
                const int lostSnap = m_Internal->intervalPacketsLost.exchange(0, std::memory_order_relaxed);
                const int expected = rxSnap + lostSnap;
                rr.fractionLost = (expected > 0)
                    ? static_cast<uint8_t>((std::min)(
                        (static_cast<float>(lostSnap) / static_cast<float>(expected)) * 256.0f,
                        255.0f))
                    : 0;

                m_Internal->onReceiverReport(rr);
            }

            {
                std::lock_guard<std::mutex> lock(m_Internal->m_TelemetryMutex);
                m_Internal->maxJitterSpikeMs *= 0.9;
                m_Internal->maxJitterSpikeMsAtomic.store(
                    static_cast<int>(m_Internal->maxJitterSpikeMs),
                    std::memory_order_relaxed);
            }
            m_Internal->lastReceiverReportTime = now;
        }

        const int keepaliveElapsedMs = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                now - m_Internal->lastKeepaliveTime).count());

        if (keepaliveElapsedMs >= m_Internal->keepaliveIntervalMs) {
            m_Internal->lastKeepaliveTime = now;
            // Keepalive fires ~once every 8 s — allocating here is acceptable.
            float silence[OPUS_FRAME_SIZE] = {};
            if (m_Internal->encoder) {
                std::vector<uint8_t> packet;
                {
                    std::lock_guard<std::mutex> lock(m_Internal->m_EncoderMutex);
                    packet = m_Internal->encoder->Encode(silence);
                }
                if (!packet.empty() && m_Internal->onMicData)
                    m_Internal->onMicData(packet, m_Internal->outgoingSeqNum++);
            }
        }
    }

    void AudioEngine::PushIncomingAudioWithSequence(
        const std::string& userId,
        const std::vector<uint8_t>& opusData,
        uint32_t seqNum)
    {
        if (!m_Internal) return;
        const auto now = std::chrono::steady_clock::now();

        if (SeqGT(seqNum, m_Internal->highestSeqReceived))
            m_Internal->highestSeqReceived = seqNum;

        auto itLast = m_Internal->lastSeq.find(userId);
        if (itLast != m_Internal->lastSeq.end()) {
            const uint32_t last = itLast->second;
            if (seqNum == last) {
                m_Internal->totalPacketsDuplicated.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            if (SeqLT(seqNum, last)) return;
            const uint32_t expectedSeq = last + 1;
            if (SeqGT(seqNum, expectedSeq)) {
                const int lost = static_cast<int>(seqNum - expectedSeq);
                m_Internal->totalPacketsLost.fetch_add(lost, std::memory_order_relaxed);
                m_Internal->intervalPacketsLost.fetch_add(lost, std::memory_order_relaxed);
            }
        }

        m_Internal->totalPacketsReceived.fetch_add(1, std::memory_order_relaxed);
        m_Internal->intervalPacketsReceived.fetch_add(1, std::memory_order_relaxed);

        auto itTime = m_Internal->lastArrival.find(userId);
        if (itTime != m_Internal->lastArrival.end() && itLast != m_Internal->lastSeq.end()) {
            const uint32_t seqDiff = seqNum - itLast->second;
            if (seqDiff > 0 && seqDiff < 100) {
                const double expectedDeltaMs = seqDiff * 10.0;
                const double actualDeltaMs = static_cast<double>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        now - itTime->second).count());
                const double transitDiff = (std::abs)(actualDeltaMs - expectedDeltaMs);
                if (transitDiff <= 200.0) {
                    std::lock_guard<std::mutex> lock(m_Internal->m_TelemetryMutex);
                    if (transitDiff > m_Internal->avgJitterMs)
                        m_Internal->maxJitterSpikeMs =
                        (std::max)(m_Internal->maxJitterSpikeMs, transitDiff);
                    m_Internal->avgJitterMs =
                        m_Internal->avgJitterMs * 0.9375 + transitDiff * 0.0625;
                    m_Internal->currentLatencyMs = actualDeltaMs;
                    m_Internal->maxJitterSpikeMsAtomic.store(
                        static_cast<int>(m_Internal->maxJitterSpikeMs),
                        std::memory_order_relaxed);
                }
            }
        }
        m_Internal->lastSeq[userId] = seqNum;
        m_Internal->lastArrival[userId] = now;

        // --- Track lookup / creation ----------------------------------------
        int trackIdx = -1;
        {
            std::lock_guard<std::mutex> lk(m_Internal->m_TracksMutex);
            auto itTrack = m_Internal->trackIndex.find(userId);
            if (itTrack != m_Internal->trackIndex.end())
                trackIdx = itTrack->second;

            if (trackIdx == -1) {
                std::unique_ptr<VoiceTrack> tr;
                if (!m_Internal->trackPool.empty()) {
                    tr = std::move(m_Internal->trackPool.back());
                    m_Internal->trackPool.pop_back();
                } else {
                    tr = std::make_unique<VoiceTrack>();
                }
                const ma_uint32 trackBuf = SAMPLE_RATE * 1;
                std::memset(&tr->rb, 0, sizeof(tr->rb));  // pool entries may have uninitialized rb; init requires clean state
                if (ma_pcm_rb_init(ma_format_f32, 1, trackBuf, nullptr, nullptr,
                    &tr->rb) != MA_SUCCESS) {
                    ResetVoiceTrackForPool(tr.get());
                    if (m_Internal->trackPool.size() < AudioInternal::kVoiceTrackPoolSize)
                        m_Internal->trackPool.push_back(std::move(tr));
                    return;
                }
                tr->userId = userId;
                tr->active = true;
                tr->decoder = std::make_unique<OpusDecoderWrapper>();
                tr->firstPacket = true;
                tr->isBuffering = true;
                tr->coldStart = true;
                tr->lastSequenceNumber = seqNum - 1;
                {
                    std::lock_guard<std::mutex> gainLock(m_Internal->m_GainMutex);
                    auto itGain = m_Internal->m_UserGains.find(userId);
                    tr->gain.store(
                        itGain != m_Internal->m_UserGains.end() ? itGain->second : 1.0f,
                        std::memory_order_relaxed);
                }
                trackIdx = static_cast<int>(m_Internal->tracks.size());
                m_Internal->trackIndex[userId] = trackIdx;
                m_Internal->tracks.push_back(std::move(tr));
            }
        }

        if (trackIdx == -1) return;

        // Hold m_TracksMutex for all use of the track. The UDP receive thread calls
        // this while the main thread can call ClearRemoteTracks(); without the lock
        // we could use track.decoder / track.rb after they were uninited -> crash / crisp audio.
        {
            std::lock_guard<std::mutex> lk(m_Internal->m_TracksMutex);
            if (trackIdx >= static_cast<int>(m_Internal->tracks.size())) return;
            VoiceTrack* tp = m_Internal->tracks[trackIdx].get();
            VoiceTrack& track = *tp;
            if (!track.firstPacket && seqNum != 0) {
                const uint32_t expected = track.lastSequenceNumber + 1;
                if (seqNum > expected && seqNum < expected + 10) {
                    for (uint32_t i = 0; i < (seqNum - expected); ++i) {
                        float plc[OPUS_FRAME_SIZE];
                        const int s = track.decoder->DecodeLossWithDiagnostics(plc);
                        if (s == OPUS_FRAME_SIZE) {
                            ma_uint32 written = 0;
                            while (written < OPUS_FRAME_SIZE) {
                                void* pW;
                                ma_uint32 chunk = OPUS_FRAME_SIZE - written;
                                if (ma_pcm_rb_acquire_write(&track.rb, &chunk, &pW) != MA_SUCCESS) break;
                                if (chunk == 0) break;
                                std::memcpy(pW, plc + written, chunk * sizeof(float));
                                ma_pcm_rb_commit_write(&track.rb, chunk);
                                written += chunk;
                            }
                        }
                    }
                }
            }
            track.lastSequenceNumber = seqNum;
            track.firstPacket = false;
            float pcm[OPUS_FRAME_SIZE];
            const int samples = track.decoder->DecodeWithDiagnostics(
                opusData.data(), opusData.size(), pcm);
            if (samples == OPUS_FRAME_SIZE) {
                const ma_uint32 avail = ma_pcm_rb_available_read(&track.rb);
                if (avail > 96000) {
                    ma_pcm_rb_seek_read(&track.rb, avail - 48000);
                    m_Internal->bufferOverflows.fetch_add(1, std::memory_order_relaxed);
                }
                ma_uint32 written = 0;
                while (written < OPUS_FRAME_SIZE) {
                    void* pW;
                    ma_uint32 chunk = OPUS_FRAME_SIZE - written;
                    if (ma_pcm_rb_acquire_write(&track.rb, &chunk, &pW) != MA_SUCCESS) break;
                    if (chunk == 0) break;
                    std::memcpy(pW, pcm + written, chunk * sizeof(float));
                    ma_pcm_rb_commit_write(&track.rb, chunk);
                    written += chunk;
                }
            }
        }
    }

    void AudioEngine::PushIncomingAudio(const std::string& userId,
        const std::vector<uint8_t>& data)
    {
        PushIncomingAudioWithSequence(userId, data, 0);
    }

    void AudioEngine::ClearRemoteTracks() {
        if (!m_Internal) return;
        std::lock_guard<std::mutex> lk(m_Internal->m_TracksMutex);

        // Collect tracks to return to pool FIRST (Bug #1 fix: avoid use-after-move in range-for)
        std::vector<std::unique_ptr<VoiceTrack>> tracksToReturn;
        tracksToReturn.reserve(m_Internal->tracks.size());

        for (auto& tp : m_Internal->tracks) {
            ma_pcm_rb_uninit(&tp->rb);
            tp->decoder.reset();
            ResetVoiceTrackForPool(tp.get());
            tracksToReturn.push_back(std::move(tp));
        }

        m_Internal->tracks.clear();
        m_Internal->trackIndex.clear();
        m_Internal->lastSeq.clear();
        m_Internal->lastArrival.clear();

        // Transfer to pool after loop completes (avoids moved-from state access)
        // Bug #4 fix: add pool size check
        for (auto& tr : tracksToReturn) {
            if (m_Internal->trackPool.size() < AudioInternal::kVoiceTrackPoolSize)
                m_Internal->trackPool.push_back(std::move(tr));
        }
    }

    void AudioEngine::RemoveUserTrack(const std::string& userId) {
        if (!m_Internal || userId.empty()) return;
        std::lock_guard<std::mutex> lk(m_Internal->m_TracksMutex);

        auto it = m_Internal->trackIndex.find(userId);
        if (it == m_Internal->trackIndex.end()) return;

        const size_t i = static_cast<size_t>(it->second);
        const size_t last = m_Internal->tracks.size() - 1;

        if (i != last) {
            m_Internal->tracks[i].swap(m_Internal->tracks[last]);
            m_Internal->trackIndex[m_Internal->tracks[i]->userId] = static_cast<int>(i);
        }

        std::unique_ptr<VoiceTrack> tr = std::move(m_Internal->tracks[last]);
        m_Internal->tracks.pop_back();
        ma_pcm_rb_uninit(&tr->rb);
        tr->decoder.reset();
        ResetVoiceTrackForPool(tr.get());

        // Bug #4 fix: only return to pool if not full
        if (m_Internal->trackPool.size() < AudioInternal::kVoiceTrackPoolSize)
            m_Internal->trackPool.push_back(std::move(tr));

        m_Internal->trackIndex.erase(userId);
        m_Internal->lastSeq.erase(userId);
        m_Internal->lastArrival.erase(userId);
    }

    void AudioEngine::ApplyConfig(int targetBufferMs, int minBufferMs, int maxBufferMs,
        int keepaliveIntervalMs, int targetBitrateKbps)
    {
        if (!m_Internal) return;
        if (targetBufferMs >= 0) {
            m_Internal->targetBufferMs = targetBufferMs;
            m_Internal->adaptiveBufferLevel = targetBufferMs;
        }
        if (minBufferMs >= 0)        m_Internal->minBufferMs = minBufferMs;
        if (maxBufferMs >= 0)        m_Internal->maxBufferMs = maxBufferMs;
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
        std::lock_guard<std::mutex> lk(m_Internal->m_TracksMutex);
        for (auto& tp : m_Internal->tracks) {
            if (tp->active && tp->userId == userId) {
                tp->gain.store(gain, std::memory_order_relaxed);
                break;
            }
        }
    }

    void AudioEngine::PushSystemAudio(const float* monoSamples, int frameCount, int sourceSampleRate) {
        if (!m_Internal || !m_Internal->deviceStarted || frameCount <= 0) return;

        // Initialize system audio ring buffer on first call
        if (!m_Internal->systemAudioRbInit) {
            ma_pcm_rb_init(ma_format_f32, 1, 48000, nullptr, nullptr, &m_Internal->systemAudioRb);
            m_Internal->systemAudioRbInit = true;
        }

        // Resample to 48kHz if needed
        std::vector<float> resampled;
        const float* data = monoSamples;
        int count = frameCount;
        if (sourceSampleRate != SAMPLE_RATE && sourceSampleRate > 0) {
            double ratio = (double)SAMPLE_RATE / sourceSampleRate;
            count = (int)(frameCount * ratio);
            resampled.resize(count);
            for (int i = 0; i < count; i++) {
                int srcIdx = (std::min)((int)(i / ratio), frameCount - 1);
                resampled[i] = monoSamples[srcIdx];
            }
            data = resampled.data();
        }

        // Write into separate system audio ring buffer (NOT the capture buffer)
        void* pWrite = nullptr;
        ma_uint32 toWrite = (ma_uint32)count;
        if (ma_pcm_rb_acquire_write(&m_Internal->systemAudioRb, &toWrite, &pWrite) == MA_SUCCESS && toWrite > 0) {
            ma_uint32 actual = (std::min)(toWrite, (ma_uint32)count);
            memcpy(pWrite, data, actual * sizeof(float));
            ma_pcm_rb_commit_write(&m_Internal->systemAudioRb, actual);
        }
    }

    void AudioEngine::SetSystemAudioVolume(float vol) {
        if (m_Internal) m_Internal->systemAudioVolume.store(vol, std::memory_order_relaxed);
    }

    float AudioEngine::GetMicActivity() const {
        return m_Internal ? m_Internal->captureRMS : 0.0f;
    }

    void AudioEngine::Shutdown() {
        if (!m_Internal) return;

        m_Internal->m_EncodeThreadShutdown.store(true, std::memory_order_relaxed);
        m_Internal->m_EncodeCv.notify_one();
        if (m_Internal->m_EncodeThread.joinable())
            m_Internal->m_EncodeThread.join();

#ifdef TALKME_USE_RNNOISE
        if (m_Internal->rnnoiseState) {
            rnnoise_destroy(m_Internal->rnnoiseState);
            m_Internal->rnnoiseState = nullptr;
        }
        m_RNNoiseState = nullptr;
#endif
#ifdef TALKME_USE_WEBRTC_APM
        // Release via the scoped_refptr, not raw delete.
        m_Internal->webrtcApm = nullptr;
        m_WebRtcApmRef.reset();
        m_WebRtcApm = nullptr;
#endif
        if (m_Internal->speexState) {
            speex_preprocess_state_destroy(m_Internal->speexState);
            m_Internal->speexState = nullptr;
        }
        if (m_Internal->deviceStarted)
            ma_device_uninit(&m_Internal->device);
        ma_pcm_rb_uninit(&m_Internal->captureRb);
        if (m_Internal->systemAudioRbInit) {
            ma_pcm_rb_uninit(&m_Internal->systemAudioRb);
            m_Internal->systemAudioRbInit = false;
        }
        {
            std::lock_guard<std::mutex> lk(m_Internal->m_TracksMutex);
            for (auto& tp : m_Internal->tracks) {
                ma_pcm_rb_uninit(&tp->rb);
                tp->decoder.reset();
            }
            m_Internal->tracks.clear();
            // Pool entries are either never inited or already uninited when returned.
            m_Internal->trackPool.clear();
        }
        m_Internal->encoder.reset();
        m_Internal->m_Engine = nullptr;
        m_Internal.reset();
    }

    void AudioEngine::OnVoiceStateUpdate(int memberCount) {
        if (!m_Internal) return;
        m_Internal->remoteMemberCount = memberCount;
        double avgJitter;
        {
            std::lock_guard<std::mutex> lock(m_Internal->m_TelemetryMutex);
            avgJitter = m_Internal->avgJitterMs;
        }
        int adapt = std::clamp(
            100 + static_cast<int>(std::ceil(avgJitter)) + memberCount * 15,
            m_Internal->minBufferMs,
            m_Internal->maxBufferMs);
        m_Internal->adaptiveBufferLevel = adapt;
    }

    void AudioEngine::OnNetworkConditions(float packetLossPercent, float /*avgLatencyMs*/) {
        if (!m_Internal) return;
        const int received = m_Internal->totalPacketsReceived.load(std::memory_order_relaxed);
        const int lost = m_Internal->totalPacketsLost.load(std::memory_order_relaxed);
        const int total = received + lost;
        m_Internal->packetLossPercentage =
            (total > 0) ? (100.0 * lost) / total
            : static_cast<double>(packetLossPercent);

        double jitterMs;
        {
            std::lock_guard<std::mutex> lock(m_Internal->m_TelemetryMutex);
            jitterMs = m_Internal->avgJitterMs;
        }
        const int targetMs = std::clamp(
            static_cast<int>(2.5 * jitterMs + 20.0),
            m_Internal->minBufferMs,
            m_Internal->maxBufferMs);
        // OPTIMIZATION: Fast Attack, Slow Release.
        // Instantly grow the buffer to prevent starvation during jitter spikes,
        // but decay it very slowly to prevent buffer thrashing (white noise/popping).
        if (targetMs > m_Internal->adaptiveBufferLevel) {
            m_Internal->adaptiveBufferLevel = targetMs;
        } else {
            m_Internal->adaptiveBufferLevel = (std::max)(targetMs, m_Internal->adaptiveBufferLevel - 2);
        }
        if (m_Internal->encoder) {
            const float loss = static_cast<float>(m_Internal->packetLossPercentage);
            m_Internal->encoder->AdjustBitrate(loss);
            m_Internal->encoder->SetPacketLossPercentage(loss);
            m_Internal->currentEncoderBitrate = m_Internal->encoder->GetCurrentBitrate();
        }
    }

    void AudioEngine::ApplyProbeResults(float rttMs, float jitterMs, float lossPct) {
        if (!m_Internal) return;
        {
            std::lock_guard<std::mutex> lock(m_Internal->m_TelemetryMutex);
            m_Internal->avgJitterMs = static_cast<double>(jitterMs);
            m_Internal->maxJitterSpikeMs = static_cast<double>(jitterMs);
            m_Internal->maxJitterSpikeMsAtomic.store(
                static_cast<int>(jitterMs), std::memory_order_relaxed);
        }
        m_Internal->packetLossPercentage = static_cast<double>(lossPct);

        const int targetMs = std::clamp(
            static_cast<int>(rttMs / 2.0f) +
            3 * static_cast<int>(std::ceil(jitterMs)) +
            static_cast<int>(lossPct * 1.5f) + 20,
            m_Internal->minBufferMs,
            m_Internal->maxBufferMs);
        m_Internal->adaptiveBufferLevel = targetMs;

        if (m_Internal->encoder) {
            m_Internal->encoder->AdjustBitrate(lossPct);
            m_Internal->encoder->SetPacketLossPercentage(lossPct);
        }
    }

    void AudioEngine::SetSelfDeafened(bool deafened) {
        m_SelfDeafened = deafened;
        if (deafened) m_SelfMuted = true;
        if (m_Internal)
            m_Internal->selfDeafened.store(deafened, std::memory_order_relaxed);
    }

    std::vector<AudioDeviceInfo> AudioEngine::GetInputDevices() {
        if (m_Internal)
            RefreshDeviceList(&m_Internal->cachedInputDevices,
                &m_Internal->cachedOutputDevices,
                &m_Internal->deviceListDirty);
        return m_Internal ? m_Internal->cachedInputDevices : std::vector<AudioDeviceInfo>{};
    }

    std::vector<AudioDeviceInfo> AudioEngine::GetOutputDevices() {
        if (m_Internal)
            RefreshDeviceList(&m_Internal->cachedInputDevices,
                &m_Internal->cachedOutputDevices,
                &m_Internal->deviceListDirty);
        return m_Internal ? m_Internal->cachedOutputDevices : std::vector<AudioDeviceInfo>{};
    }

    bool AudioEngine::ReinitDevice(int inputIdx, int outputIdx) {
        if (!m_Internal) return false;
        return ReinitDeviceImpl(&m_Internal->device, &m_Internal->config,
            &m_Internal->deviceStarted, &m_Internal->deviceListDirty,
            inputIdx, outputIdx, SAMPLE_RATE, OPUS_FRAME_SIZE,
            DataCallback, m_Internal.get());
    }

} // namespace TalkMe