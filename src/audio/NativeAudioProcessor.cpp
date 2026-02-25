#include "NativeAudioProcessor.h"
#include <cmath>
#include <algorithm>

namespace TalkMe {

float NativeAudioProcessor::Rms(const float* pcm, int samples) {
    if (samples <= 0) return 0.0f;
    float sum = 0.0f;
    for (int i = 0; i < samples; ++i)
        sum += pcm[i] * pcm[i];
    return std::sqrt(sum / static_cast<float>(samples));
}

void NativeAudioProcessor::Process(float* pcm, int samples, bool enableGate, bool enableApm) {
    if (enableApm && samples > 0) {
        for (int i = 0; i < samples; ++i) {
            float y = kHpAlpha * (m_hp_y1 + pcm[i] - m_hp_x1);
            m_hp_x1 = pcm[i];
            m_hp_y1 = y;
            pcm[i] = y;
        }

        float rms = Rms(pcm, samples);
        float targetGain = (rms > 1e-6f) ? (kTargetRms / rms) : m_agcGain;
        float coeff = (targetGain < m_agcGain) ? kAttack : kRelease;
        m_agcGain += (targetGain - m_agcGain) * coeff;
        m_agcGain = std::clamp(m_agcGain, 0.1f, 20.0f);

        for (int i = 0; i < samples; ++i)
            pcm[i] = std::clamp(pcm[i] * m_agcGain, -1.0f, 1.0f);
    }

    if (enableGate) {
        for (int i = 0; i < samples; ++i) {
            float absSample = std::abs(pcm[i]);
            m_envelope = (absSample > m_envelope)
                ? m_envelope * 0.8f + absSample * 0.2f
                : m_envelope * 0.999f + absSample * 0.001f;
            float targetGate = (m_envelope < 0.015f)
                ? (m_envelope / 0.015f) : 1.0f;
            m_gateGain = m_gateGain * 0.99f + targetGate * 0.01f;
            pcm[i] = std::clamp(pcm[i] * m_gateGain, -1.0f, 1.0f);
        }
    }
}

} // namespace TalkMe
