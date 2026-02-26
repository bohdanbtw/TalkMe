#pragma once
#include <memory>
#include <functional>
#include <vector>
#include <string>
#include <cstdint>
#include "../shared/Protocol.h"

#ifdef TALKME_USE_RNNOISE
struct DenoiseState;
#endif
#ifdef TALKME_USE_WEBRTC_APM
#include <modules/audio_processing/include/audio_processing.h>
#include <rtc_base/ref_counted_object.h>
#endif

namespace TalkMe {

    enum class NoiseSuppressionMode { None = 0, RNNoise = 1, SpeexDSP = 2, WebRTC = 3 };

    struct AudioDeviceInfo {
        std::string name;
        int  index;
        bool isDefault;
    };

    struct AudioInternal;

    class AudioEngine {
    public:
        AudioEngine();
        ~AudioEngine();

        bool InitializeWithSequence(
            std::function<void(const std::vector<uint8_t>&, uint32_t seqNum)> onMicDataCaptured);

        void Update();

        void SetCaptureEnabled(bool enabled) { m_CaptureEnabled = enabled; }
        bool IsCaptureEnabled() const { return m_CaptureEnabled; }

        void SetNoiseSuppressionMode(NoiseSuppressionMode mode);
        NoiseSuppressionMode GetNoiseSuppressionMode() const;
        void SetMicTestEnabled(bool enabled);
        bool IsMicTestEnabled() const { return m_MicTestEnabled; }
        void* GetRNNoiseState() const { return m_RNNoiseState; }
        void* GetWebRtcApm()    const { return m_WebRtcApm; }

        void PushIncomingAudio(const std::string& userId,
            const std::vector<uint8_t>& opusData);
        void PushIncomingAudioWithSequence(const std::string& userId,
            const std::vector<uint8_t>& opusData,
            uint32_t seqNum);
        void SetReceiverReportCallback(
            std::function<void(const TalkMe::ReceiverReportPayload&)> callback);
        void Shutdown();
        void ClearRemoteTracks();
        void RemoveUserTrack(const std::string& userId);
        void OnVoiceStateUpdate(int memberCount);
        void ApplyConfig(int targetBufferMs, int minBufferMs, int maxBufferMs,
            int keepaliveIntervalMs = -1, int targetBitrateKbps = -1);
        void SetUserGain(const std::string& userId, float gain);
        void PushSystemAudio(const float* mono48k, int frameCount, int sourceSampleRate);
        float GetMicActivity() const;

        void SetSelfMuted(bool muted) { m_SelfMuted = muted; }
        bool IsSelfMuted()    const { return m_SelfMuted; }
        void SetSelfDeafened(bool deafened);
        bool IsSelfDeafened() const { return m_SelfDeafened; }

        std::vector<AudioDeviceInfo> GetInputDevices();
        std::vector<AudioDeviceInfo> GetOutputDevices();
        bool ReinitDevice(int inputIdx, int outputIdx);

        struct Telemetry {
            int   totalPacketsReceived = 0;
            int   totalPacketsLost = 0;
            int   totalPacketsDuplicated = 0;
            float avgJitterMs = 0.0f;
            float currentLatencyMs = 0.0f;
            float packetLossPercentage = 0.0f;
            int   bufferUnderruns = 0;
            int   bufferOverflows = 0;
            int   currentBufferMs = 0;
            int   targetBufferMs = 150;
            int   remoteMemberCount = 0;
            int   adaptiveBufferLevel = 0;
            int   currentEncoderBitrateKbps = 32;
            float currentVoiceActivityLevel = 0.0f;
        };

        Telemetry GetTelemetry();

        void OnNetworkConditions(float packetLossPercent, float avgLatencyMs);
        void ApplyProbeResults(float rttMs, float jitterMs, float lossPct);

    private:
        std::unique_ptr<AudioInternal> m_Internal;
        bool m_CaptureEnabled = false;
        bool m_SelfMuted = false;
        bool m_SelfDeafened = false;
        NoiseSuppressionMode m_NoiseSuppressionMode = NoiseSuppressionMode::RNNoise;
        bool m_MicTestEnabled = false;

#ifdef TALKME_USE_RNNOISE
        DenoiseState* m_RNNoiseState = nullptr;
#else
        void* m_RNNoiseState = nullptr;
#endif

#ifdef TALKME_USE_WEBRTC_APM
        // Owns the WebRTC APM instance through its reference count.
        // m_WebRtcApm is an unowned observer for callers that cannot include WebRTC headers.
        rtc::scoped_refptr<webrtc::AudioProcessing> m_WebRtcApmRef;
        void* m_WebRtcApm = nullptr;
#else
        void* m_WebRtcApm = nullptr;
#endif
    };

} // namespace TalkMe