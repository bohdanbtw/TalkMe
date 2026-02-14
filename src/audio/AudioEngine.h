#pragma once
#include <memory>
#include <functional>
#include <vector>
#include <string>
#include <cstdint>

namespace TalkMe {

    struct AudioInternal;

    class AudioEngine {
    public:
        AudioEngine();
        ~AudioEngine();

        // ⚠️ DEPRECATED: Use InitializeWithSequence for new code
        bool Initialize(std::function<void(const std::vector<uint8_t>&)> onMicDataCaptured);

        // ✅ RECOMMENDED: New signature with sequence number
        bool InitializeWithSequence(std::function<void(const std::vector<uint8_t>&, uint32_t seqNum)> onMicDataCaptured);

        void Update();  // Call every frame to process microphone data

        // ⚠️ DEPRECATED: Use PushIncomingAudioWithSequence for new code
        void PushIncomingAudio(const std::string& userId, const std::vector<uint8_t>& opusData);

        // ✅ RECOMMENDED: Includes packet loss detection
        void PushIncomingAudioWithSequence(const std::string& userId, const std::vector<uint8_t>& opusData, uint32_t seqNum);

        void Shutdown();

    private:
        std::unique_ptr<AudioInternal> m_Internal;
    };
}