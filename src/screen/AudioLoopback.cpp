#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "AudioLoopback.h"
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <vector>

#pragma comment(lib, "ole32.lib")

namespace TalkMe {

void AudioLoopback::Start(AudioCallback onAudio) {
    if (m_Running.load()) return;
    m_OnAudio = std::move(onAudio);
    m_Running.store(true);

    m_Thread = std::thread([this]() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        CaptureLoop();
        CoUninitialize();
    });
}

void AudioLoopback::Stop() {
    m_Running.store(false);
    if (m_Thread.joinable()) m_Thread.join();
}

void AudioLoopback::CaptureLoop() {
    IMMDeviceEnumerator* pEnumerator = nullptr;
    IMMDevice* pDevice = nullptr;
    IAudioClient* pAudioClient = nullptr;
    IAudioCaptureClient* pCaptureClient = nullptr;

    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
        CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&pEnumerator);
    if (FAILED(hr)) {
        std::fprintf(stderr, "[AudioLoopback] Failed to create device enumerator: 0x%08lx\n", hr);
        m_Running.store(false);
        return;
    }

    hr = pEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &pDevice);
    if (FAILED(hr)) {
        std::fprintf(stderr, "[AudioLoopback] No default audio output device\n");
        pEnumerator->Release();
        m_Running.store(false);
        return;
    }

    hr = pDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&pAudioClient);
    if (FAILED(hr)) {
        pDevice->Release(); pEnumerator->Release();
        m_Running.store(false);
        return;
    }

    WAVEFORMATEX* pwfx = nullptr;
    pAudioClient->GetMixFormat(&pwfx);

    // Request loopback capture
    hr = pAudioClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_LOOPBACK,
        200000, // 20ms buffer
        0, pwfx, nullptr);

    if (FAILED(hr)) {
        std::fprintf(stderr, "[AudioLoopback] AudioClient Initialize failed: 0x%08lx\n", hr);
        CoTaskMemFree(pwfx);
        pAudioClient->Release(); pDevice->Release(); pEnumerator->Release();
        m_Running.store(false);
        return;
    }

    hr = pAudioClient->GetService(__uuidof(IAudioCaptureClient), (void**)&pCaptureClient);
    if (FAILED(hr)) {
        CoTaskMemFree(pwfx);
        pAudioClient->Release(); pDevice->Release(); pEnumerator->Release();
        m_Running.store(false);
        return;
    }

    std::fprintf(stderr, "[AudioLoopback] Started: %dHz, %d channels, %d bits\n",
        pwfx->nSamplesPerSec, pwfx->nChannels, pwfx->wBitsPerSample);
    std::fflush(stderr);

    int sampleRate = pwfx->nSamplesPerSec;
    int channels = pwfx->nChannels;
    bool isFloat = (pwfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) ||
        (pwfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
         pwfx->cbSize >= 22 &&
         ((WAVEFORMATEXTENSIBLE*)pwfx)->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);

    pAudioClient->Start();

    while (m_Running.load()) {
        Sleep(10);

        UINT32 packetLength = 0;
        pCaptureClient->GetNextPacketSize(&packetLength);

        while (packetLength > 0 && m_Running.load()) {
            BYTE* pData = nullptr;
            UINT32 numFrames = 0;
            DWORD flags = 0;

            hr = pCaptureClient->GetBuffer(&pData, &numFrames, &flags, nullptr, nullptr);
            if (FAILED(hr)) break;

            if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT) && pData && numFrames > 0 && m_OnAudio) {
                if (isFloat) {
                    m_OnAudio(reinterpret_cast<const float*>(pData), numFrames, sampleRate, channels);
                } else {
                    // Convert int16 to float
                    int totalSamples = numFrames * channels;
                    std::vector<float> floatBuf(totalSamples);
                    const int16_t* src = reinterpret_cast<const int16_t*>(pData);
                    for (int i = 0; i < totalSamples; i++)
                        floatBuf[i] = src[i] / 32768.0f;
                    m_OnAudio(floatBuf.data(), numFrames, sampleRate, channels);
                }
            }

            pCaptureClient->ReleaseBuffer(numFrames);
            pCaptureClient->GetNextPacketSize(&packetLength);
        }
    }

    pAudioClient->Stop();
    CoTaskMemFree(pwfx);
    pCaptureClient->Release();
    pAudioClient->Release();
    pDevice->Release();
    pEnumerator->Release();

    std::fprintf(stderr, "[AudioLoopback] Stopped\n");
}

} // namespace TalkMe
