#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "WebcamCapture.h"
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <algorithm>
#include <cstdio>
#include <cstring>

#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")

namespace TalkMe {

std::vector<WebcamDeviceInfo> WebcamCapture::EnumerateDevices() {
    std::vector<WebcamDeviceInfo> result;
    IMFAttributes* pAttributes = nullptr;
    if (FAILED(MFCreateAttributes(&pAttributes, 1))) return result;
    pAttributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);

    IMFActivate** ppDevices = nullptr;
    UINT32 count = 0;
    if (SUCCEEDED(MFEnumDeviceSources(pAttributes, &ppDevices, &count))) {
        for (UINT32 i = 0; i < count; i++) {
            wchar_t* name = nullptr;
            UINT32 nameLen = 0;
            ppDevices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name, &nameLen);
            wchar_t* symLink = nullptr;
            UINT32 symLen = 0;
            ppDevices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &symLink, &symLen);

            WebcamDeviceInfo info;
            if (name) {
                char utf8[512] = {};
                ::WideCharToMultiByte(CP_UTF8, 0, name, -1, utf8, 512, nullptr, nullptr);
                info.name = utf8;
                CoTaskMemFree(name);
            }
            if (symLink) {
                char utf8[1024] = {};
                ::WideCharToMultiByte(CP_UTF8, 0, symLink, -1, utf8, 1024, nullptr, nullptr);
                info.symbolicLink = utf8;
                CoTaskMemFree(symLink);
            }
            result.push_back(std::move(info));
            ppDevices[i]->Release();
        }
        CoTaskMemFree(ppDevices);
    }
    pAttributes->Release();
    return result;
}

void WebcamCapture::Start(const std::string& deviceSymLink, int fps, int width, int height, FrameCallback onFrame) {
    Stop();
    m_DeviceSymLink = deviceSymLink;
    m_Fps = fps;
    m_Width = width;
    m_Height = height;
    m_OnFrame = std::move(onFrame);
    m_Running.store(true);
    m_Thread = std::thread([this]() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        CaptureLoop();
        CoUninitialize();
        m_Running.store(false);
    });
}

void WebcamCapture::Stop() {
    m_Running.store(false);
    if (m_Thread.joinable()) m_Thread.join();
}

void WebcamCapture::CaptureLoop() {
    // Convert symbolic link to wide string
    wchar_t wSymLink[1024] = {};
    ::MultiByteToWideChar(CP_UTF8, 0, m_DeviceSymLink.c_str(), -1, wSymLink, 1024);

    IMFAttributes* pAttr = nullptr;
    MFCreateAttributes(&pAttr, 2);
    pAttr->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    pAttr->SetString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, wSymLink);

    IMFMediaSource* pSource = nullptr;
    HRESULT hr = MFCreateDeviceSource(pAttr, &pSource);
    pAttr->Release();
    if (FAILED(hr)) return;

    IMFSourceReader* pReader = nullptr;
    IMFAttributes* pReaderAttr = nullptr;
    MFCreateAttributes(&pReaderAttr, 1);
    pReaderAttr->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    hr = MFCreateSourceReaderFromMediaSource(pSource, pReaderAttr, &pReader);
    pReaderAttr->Release();
    if (FAILED(hr)) { pSource->Release(); return; }

    // Request RGB32 output for simplicity
    IMFMediaType* pType = nullptr;
    MFCreateMediaType(&pType);
    pType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    pType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    pReader->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, pType);
    pType->Release();

    const auto frameInterval = std::chrono::microseconds(1'000'000 / (std::max)(1, m_Fps));
    auto nextDeadline = std::chrono::steady_clock::now();

    while (m_Running.load()) {
        auto now = std::chrono::steady_clock::now();
        if (now < nextDeadline) {
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(nextDeadline - now).count();
            if (ms > 0) Sleep((DWORD)ms);
        }

        IMFSample* pSample = nullptr;
        DWORD streamIdx = 0, flags = 0;
        LONGLONG timestamp = 0;
        hr = pReader->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &streamIdx, &flags, &timestamp, &pSample);
        if (FAILED(hr) || !pSample) {
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
            nextDeadline += frameInterval;
            continue;
        }

        IMFMediaBuffer* pBuf = nullptr;
        pSample->ConvertToContiguousBuffer(&pBuf);
        if (pBuf) {
            BYTE* pData = nullptr;
            DWORD len = 0;
            pBuf->Lock(&pData, nullptr, &len);

            // Get actual dimensions from current media type
            IMFMediaType* pCurType = nullptr;
            int w = m_Width, h = m_Height;
            if (SUCCEEDED(pReader->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &pCurType))) {
                UINT32 tw = 0, th = 0;
                MFGetAttributeSize(pCurType, MF_MT_FRAME_SIZE, &tw, &th);
                if (tw > 0 && th > 0) { w = tw; h = th; }
                pCurType->Release();
            }

            if ((int)len >= w * h * 4 && m_OnFrame) {
                // RGB32 (BGRA) -> RGBA
                std::vector<uint8_t> rgba(w * h * 4);
                for (int i = 0; i < w * h; i++) {
                    rgba[i * 4 + 0] = pData[i * 4 + 2]; // R
                    rgba[i * 4 + 1] = pData[i * 4 + 1]; // G
                    rgba[i * 4 + 2] = pData[i * 4 + 0]; // B
                    rgba[i * 4 + 3] = 255;
                }
                m_OnFrame(rgba, w, h);
            }
            pBuf->Unlock();
            pBuf->Release();
        }
        pSample->Release();

        nextDeadline += frameInterval;
        auto n = std::chrono::steady_clock::now();
        if (nextDeadline <= n)
            do { nextDeadline += frameInterval; } while (nextDeadline <= n);
    }

    pReader->Release();
    pSource->Shutdown();
    pSource->Release();
}

} // namespace TalkMe
