#pragma once

namespace TalkMe {

// In-place DSP: high-pass filter, AGC, and optional gate.
// No miniaudio or Opus; float PCM in/out only.
// HP runs before RMS so DC offset does not inflate the gain estimate.
// AGC: attack (fast) when reducing gain, release (slow) when increasing.
class NativeAudioProcessor {
public:
    void Process(float* pcm, int samples, bool enableGate, bool enableApm);

private:
    float m_hp_x1 = 0.0f;
    float m_hp_y1 = 0.0f;
    float m_envelope = 0.0f;
    float m_gateGain = 1.0f;
    float m_agcGain = 1.0f;

    static constexpr float kHpAlpha = 0.989f;
    static constexpr float kTargetRms = 0.4f;
    static constexpr float kAttack = 0.02f;
    static constexpr float kRelease = 0.0005f;

    static float Rms(const float* pcm, int samples);
};

} // namespace TalkMe
