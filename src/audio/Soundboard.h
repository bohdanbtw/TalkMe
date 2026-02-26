#pragma once
#include <vector>
#include <string>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <windows.h>
#include <mmsystem.h>

#pragma comment(lib, "winmm.lib")

namespace TalkMe {

struct SoundEffect {
    std::string name;
    std::vector<uint8_t> wavData;
};

class Soundboard {
public:
    void Generate() {
        m_Sounds.clear();
        m_Sounds.push_back({ "Airhorn", BuildTone(220.0f, 300, 0.15f) });
        m_Sounds.push_back({ "Drum Roll", BuildNoise(400, 0.1f) });
        m_Sounds.push_back({ "Sad Trombone", BuildTone(180.0f, 500, 0.08f) });
        m_Sounds.push_back({ "Applause", BuildNoise(800, 0.06f) });
        m_Sounds.push_back({ "Ding", BuildTone(880.0f, 100, 0.12f) });
        m_Sounds.push_back({ "Buzzer", BuildTone(150.0f, 400, 0.1f) });
        m_Sounds.push_back({ "Whistle", BuildTone(1200.0f, 200, 0.08f) });
        m_Sounds.push_back({ "Boing", BuildTone(300.0f, 150, 0.1f) });
    }

    void Play(int index) const {
        if (index < 0 || index >= (int)m_Sounds.size()) return;
        PlaySoundA((LPCSTR)m_Sounds[index].wavData.data(), NULL, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
    }

    const std::vector<SoundEffect>& GetSounds() const { return m_Sounds; }

private:
    std::vector<SoundEffect> m_Sounds;

    static std::vector<uint8_t> BuildTone(float freq, int durationMs, float volume) {
        const int sampleRate = 44100;
        const int numSamples = sampleRate * durationMs / 1000;
        const int dataSize = numSamples * 2;
        std::vector<uint8_t> wav(44 + dataSize);
        auto w16 = [&](size_t p, uint16_t v) { memcpy(&wav[p], &v, 2); };
        auto w32 = [&](size_t p, uint32_t v) { memcpy(&wav[p], &v, 4); };
        memcpy(&wav[0], "RIFF", 4); w32(4, 36 + dataSize);
        memcpy(&wav[8], "WAVE", 4); memcpy(&wav[12], "fmt ", 4);
        w32(16, 16); w16(20, 1); w16(22, 1); w32(24, sampleRate);
        w32(28, sampleRate * 2); w16(32, 2); w16(34, 16);
        memcpy(&wav[36], "data", 4); w32(40, dataSize);
        int16_t* samples = (int16_t*)&wav[44];
        for (int i = 0; i < numSamples; i++) {
            float env = 1.0f;
            float fadeS = sampleRate * 0.01f;
            if ((float)i < fadeS) env = (float)i / fadeS;
            else if ((float)i > numSamples - fadeS) env = (float)(numSamples - i) / fadeS;
            samples[i] = (int16_t)(sinf(6.28318f * freq * i / sampleRate) * volume * env * 32767.0f);
        }
        return wav;
    }

    static std::vector<uint8_t> BuildNoise(int durationMs, float volume) {
        const int sampleRate = 44100;
        const int numSamples = sampleRate * durationMs / 1000;
        const int dataSize = numSamples * 2;
        std::vector<uint8_t> wav(44 + dataSize);
        auto w16 = [&](size_t p, uint16_t v) { memcpy(&wav[p], &v, 2); };
        auto w32 = [&](size_t p, uint32_t v) { memcpy(&wav[p], &v, 4); };
        memcpy(&wav[0], "RIFF", 4); w32(4, 36 + dataSize);
        memcpy(&wav[8], "WAVE", 4); memcpy(&wav[12], "fmt ", 4);
        w32(16, 16); w16(20, 1); w16(22, 1); w32(24, sampleRate);
        w32(28, sampleRate * 2); w16(32, 2); w16(34, 16);
        memcpy(&wav[36], "data", 4); w32(40, dataSize);
        int16_t* samples = (int16_t*)&wav[44];
        srand(42);
        for (int i = 0; i < numSamples; i++) {
            float env = 1.0f;
            float fadeS = sampleRate * 0.02f;
            if ((float)i < fadeS) env = (float)i / fadeS;
            else if ((float)i > numSamples - fadeS) env = (float)(numSamples - i) / fadeS;
            float noise = ((rand() % 32768) / 16384.0f - 1.0f);
            samples[i] = (int16_t)(noise * volume * env * 32767.0f);
        }
        return wav;
    }
};

} // namespace TalkMe
