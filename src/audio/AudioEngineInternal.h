#pragma once

#include "AudioEngine.h"
#include "OpusCodec.h"
#include "NativeAudioProcessor.h"
#include "../../vendor/miniaudio.h"
#include <vector>
#include <mutex>
#include <atomic>
#include <functional>
#include <map>
#include <unordered_map>
#include <chrono>
#include <thread>
#include <condition_variable>
#include <string>
#include <memory>
#include <cmath>

#include <speex/speex_preprocess.h>
#ifdef TALKME_USE_RNNOISE
#include <rnnoise.h>
#endif
#ifdef TALKME_USE_WEBRTC_APM
#include <modules/audio_processing/include/audio_processing.h>
#endif

namespace TalkMe {

    // ---------------------------------------------------------------------------
    // Shared DSP utilities — single definition used by all TUs in this module.
    // ---------------------------------------------------------------------------

    // Returns 0 when count <= 0 rather than producing a divide-by-zero.
    [[nodiscard]] inline float ComputeRms(const float* samples, int count) noexcept {
        if (count <= 0) return 0.0f;
        float sum = 0.0f;
        for (int i = 0; i < count; ++i) sum += samples[i] * samples[i];
        return std::sqrt(sum / static_cast<float>(count));
    }

    // Cubic soft-clip: approximates tanh(x/3)*3, hard-clips outside (-3, 3).
    [[nodiscard]] inline float SoftClip(float x) noexcept {
        if (x < -3.0f) return -1.0f;
        if (x > 3.0f) return  1.0f;
        return x * (27.0f + x * x) / (27.0f + 9.0f * x * x);
    }

    // ---------------------------------------------------------------------------

    struct VoiceTrack {
        ma_pcm_rb   rb;
        bool        active = false;
        std::string userId;
        std::unique_ptr<OpusDecoderWrapper> decoder;
        uint32_t    lastSequenceNumber = 0;
        bool        firstPacket = true;
        bool        isBuffering = true;
        bool        coldStart = true;
        std::atomic<float> gain{ 1.0f };
        double      smoothedBufferLevelMs = 0.0;
    };

    struct AudioInternal {
        ma_device        device;
        ma_device_config config;
        ma_pcm_rb        captureRb;

        std::vector<std::unique_ptr<VoiceTrack>> tracks;
        std::mutex m_TracksMutex;

        /// Pre-allocated pool for VoiceTrack to avoid heap allocation when remote users connect.
        static constexpr size_t kVoiceTrackPoolSize = 16;
        std::vector<std::unique_ptr<VoiceTrack>> trackPool;

        std::unique_ptr<OpusEncoderWrapper> encoder;
        std::mutex m_EncoderMutex;  // Protects concurrent access to encoder from multiple threads
        std::function<void(const std::vector<uint8_t>&, uint32_t)> onMicData;

        std::function<void(const TalkMe::ReceiverReportPayload&)> onReceiverReport;
        std::chrono::steady_clock::time_point lastReceiverReportTime =
            std::chrono::steady_clock::now();

        uint32_t highestSeqReceived = 0;
        std::atomic<int> intervalPacketsReceived{ 0 };
        std::atomic<int> intervalPacketsLost{ 0 };

        float    currentGain = 1.0f;
        float    targetGain = 1.0f;
        float    captureRMS = 0.0f;
        float    processedRMS = 0.0f;
        float    noiseFloorRms = 0.001f;
        uint32_t outgoingSeqNum = 0;

        std::atomic<int> totalPacketsReceived{ 0 };
        std::atomic<int> totalPacketsLost{ 0 };
        std::atomic<int> totalPacketsDuplicated{ 0 };
        std::atomic<int> bufferUnderruns{ 0 };
        std::atomic<int> bufferOverflows{ 0 };

        mutable std::mutex m_TelemetryMutex;
        double avgJitterMs = 0.0;
        double currentLatencyMs = 0.0;
        double maxJitterSpikeMs = 0.0;
        double packetLossPercentage = 0.0;
        std::atomic<int> maxJitterSpikeMsAtomic{ 0 };

        int remoteMemberCount = 0;
        int targetBufferMs = 150;
        int minBufferMs = 80;
        int maxBufferMs = 300;
        int adaptiveBufferLevel = 150;
        int   currentEncoderBitrate = 32000;
        float currentVoiceActivityLevel = 0.0f;

        std::unordered_map<std::string, uint32_t>                              lastSeq;
        std::unordered_map<std::string, std::chrono::steady_clock::time_point> lastArrival;
        std::unordered_map<std::string, int>                                   trackIndex;

        bool deviceStarted = false;
        bool deviceListDirty = true;
        std::vector<AudioDeviceInfo> cachedInputDevices;
        std::vector<AudioDeviceInfo> cachedOutputDevices;
        std::atomic<bool> selfDeafened{ false };

        int callbackInvocations = 0;
        std::chrono::steady_clock::time_point lastKeepaliveTime =
            std::chrono::steady_clock::now();
        int keepaliveIntervalMs = 8000;

        std::mutex m_GainMutex;
        std::map<std::string, float> m_UserGains;

        AudioEngine* m_Engine = nullptr;

        std::vector<float> m_CaptureProcessBuffer;
        // Both buffers pre-allocated in InitializeWithSequence to 2x the actual device
        // period so that DataCallback / MixTracks never touch the heap.
        std::vector<float> mixBuffer;
        // Per-track staging buffer used inside MixTracks.
        // Replaces the former float[4096] stack array (which had no frameCount guard
        // and would overflow if the OS negotiated a period > 4096 frames).
        std::vector<float> m_PerTrackBuffer;
        std::vector<uint8_t> m_EncodeWorkBuffer;

        NativeAudioProcessor m_NativeProcessor;

        std::thread             m_EncodeThread;
        std::mutex              m_EncodeMutex;
        std::condition_variable m_EncodeCv;
        std::atomic<bool>       m_EncodeThreadShutdown{ false };

        SpeexPreprocessState* speexState = nullptr;
        std::vector<int16_t>  speexPcm;
#ifdef TALKME_USE_RNNOISE
        DenoiseState* rnnoiseState = nullptr;
#endif
#ifdef TALKME_USE_WEBRTC_APM
        // AudioProcessingBuilder().Create() returns rtc::scoped_refptr<AudioProcessing>.
        // The owning scoped_refptr lives in AudioEngine; this is a non-owning observer.
        webrtc::AudioProcessing* webrtcApm = nullptr;
#endif
    };

    void EncodeThreadFunc(AudioInternal* internal);

} // namespace TalkMe