#include "AppSounds.h"
#include <cmath>
#include <cstring>
#include <windows.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")

namespace TalkMe {

namespace {

// Builds a short soft sine WAV (44-byte header + PCM). No external files.
std::vector<uint8_t> BuildWavSoft(float freq, int durationMs, float volume) {
    const int sampleRate = 44100;
    const int numSamples = sampleRate * durationMs / 1000;
    const int bitsPerSample = 16;
    const int byteRate = sampleRate * bitsPerSample / 8;
    const int dataSize = numSamples * (bitsPerSample / 8);

    std::vector<uint8_t> wav(44 + dataSize);
    auto w16 = [&](size_t p, uint16_t v) { memcpy(&wav[p], &v, 2); };
    auto w32 = [&](size_t p, uint32_t v) { memcpy(&wav[p], &v, 4); };

    memcpy(&wav[0], "RIFF", 4);
    w32(4, 36 + dataSize);
    memcpy(&wav[8], "WAVE", 4);
    memcpy(&wav[12], "fmt ", 4);
    w32(16, 16);
    w16(20, 1);
    w16(22, 1);
    w32(24, sampleRate);
    w32(28, byteRate);
    w16(32, bitsPerSample / 8);
    w16(34, bitsPerSample);
    memcpy(&wav[36], "data", 4);
    w32(40, dataSize);

    int16_t* samples = (int16_t*)&wav[44];
    const float pi2 = 6.28318530f;
    float phase = 0.0f;
    const float fadeMs = 8.0f;
    const float fadeSamples = (float)(sampleRate * (fadeMs / 1000.0f));
    for (int i = 0; i < numSamples; i++) {
        float envelope = 1.0f;
        if ((float)i < fadeSamples)
            envelope = (float)i / fadeSamples;
        else if ((float)i > (float)numSamples - fadeSamples)
            envelope = (float)(numSamples - i) / fadeSamples;
        envelope *= envelope;

        phase += pi2 * freq / (float)sampleRate;
        if (phase > pi2) phase -= pi2;
        samples[i] = (int16_t)(std::sin(phase) * volume * envelope * 32767.0f);
    }
    return wav;
}

} // namespace

void AppSounds::Generate() {
    m_JoinSound = BuildWavSoft(260.0f, 45, 0.055f);
    m_LeaveSound = BuildWavSoft(240.0f, 40, 0.05f);
    m_MessageSound = BuildWavSoft(440.0f, 30, 0.04f);
    m_MentionSound = BuildWavSoft(660.0f, 80, 0.1f);
}

void AppSounds::PlayJoin() const {
    if (!m_JoinSound.empty())
        PlaySoundA((LPCSTR)m_JoinSound.data(), NULL, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
}

void AppSounds::PlayLeave() const {
    if (!m_LeaveSound.empty())
        PlaySoundA((LPCSTR)m_LeaveSound.data(), NULL, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
}

void AppSounds::PlayMessage() const {
    if (!m_MessageSound.empty())
        PlaySoundA((LPCSTR)m_MessageSound.data(), NULL, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
}

void AppSounds::PlayMention() const {
    if (!m_MentionSound.empty())
        PlaySoundA((LPCSTR)m_MentionSound.data(), NULL, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
}

} // namespace TalkMe
