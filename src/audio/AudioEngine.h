#pragma once
#include <memory>
#include <functional>
#include <vector>
#include <string>
#include <cstdint>
#include "../shared/Protocol.h"

namespace TalkMe {

    struct AudioDeviceInfo {
        std::string name;
        int index;
        bool isDefault;
    };

    struct AudioInternal;

    class AudioEngine {
    public:
        AudioEngine();
        ~AudioEngine();

        bool Initialize(std::function<void(const std::vector<uint8_t>&)> onMicDataCaptured);
        bool InitializeWithSequence(std::function<void(const std::vector<uint8_t>&, uint32_t seqNum)> onMicDataCaptured);

        void Update();

        void SetCaptureEnabled(bool enabled) { m_CaptureEnabled = enabled; }
        bool IsCaptureEnabled() const { return m_CaptureEnabled; }

        void PushIncomingAudio(const std::string& userId, const std::vector<uint8_t>& opusData);
        void PushIncomingAudioWithSequence(const std::string& userId, const std::vector<uint8_t>& opusData, uint32_t seqNum);
        void SetReceiverReportCallback(std::function<void(const TalkMe::ReceiverReportPayload&)> callback);
        void Shutdown();
        void ClearRemoteTracks();
        void RemoveUserTrack(const std::string& userId);
        void OnVoiceStateUpdate(int memberCount);
        void ApplyConfig(int targetBufferMs, int minBufferMs, int maxBufferMs, int keepaliveIntervalMs = -1, int targetBitrateKbps = -1);
        void SetUserGain(const std::string& userId, float gain);
        float GetMicActivity() const;

        // Self mute/deafen
        void SetSelfMuted(bool muted) { m_SelfMuted = muted; }
        bool IsSelfMuted() const { return m_SelfMuted; }
        void SetSelfDeafened(bool deafened);
        bool IsSelfDeafened() const { return m_SelfDeafened; }

        // Device enumeration & switching
        std::vector<AudioDeviceInfo> GetInputDevices();
        std::vector<AudioDeviceInfo> GetOutputDevices();
        bool ReinitDevice(int inputIdx, int outputIdx);

        struct Telemetry {
            // Packet statistics
            int totalPacketsReceived = 0;
            int totalPacketsLost = 0;
            int totalPacketsDuplicated = 0;

            // Audio quality metrics
            float avgJitterMs = 0.0f;
            float currentLatencyMs = 0.0f;
            float packetLossPercentage = 0.0f;

            // Buffer health
            int bufferUnderruns = 0;
            int bufferOverflows = 0;
            int currentBufferMs = 0;
            int targetBufferMs = 150;

            // Network state
            int remoteMemberCount = 0;
            int adaptiveBufferLevel = 0;

            // Codec stats
            int currentEncoderBitrateKbps = 32;
            float currentVoiceActivityLevel = 0.0f;
        };

        Telemetry GetTelemetry();

        // Network adaptation
        void OnNetworkConditions(float packetLossPercent, float avgLatencyMs);

        // Link-probe: seed jitter buffer from probe RTT/jitter/loss (before voice traffic).
        void ApplyProbeResults(float rttMs, float jitterMs, float lossPct);

    private:
        std::unique_ptr<AudioInternal> m_Internal;
        bool m_CaptureEnabled = false;
        bool m_SelfMuted = false;
        bool m_SelfDeafened = false;
    };
}