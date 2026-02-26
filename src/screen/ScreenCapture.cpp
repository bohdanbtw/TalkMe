#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "ScreenCapture.h"
#include <chrono>
#include <algorithm>
#include <windows.h>
#include <wingdi.h>
#include <objidl.h>
#include <gdiplus.h>
#include <shlwapi.h>
#include <cstdio>

#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "ole32.lib")

namespace TalkMe {

namespace {

int GetEncoderClsid(const WCHAR* format, CLSID* pClsid) {
    UINT num = 0, size = 0;
    Gdiplus::GetImageEncodersSize(&num, &size);
    if (size == 0) return -1;
    std::vector<uint8_t> buf(size);
    auto* pEncoders = reinterpret_cast<Gdiplus::ImageCodecInfo*>(buf.data());
    Gdiplus::GetImageEncoders(num, size, pEncoders);
    for (UINT i = 0; i < num; ++i) {
        if (wcscmp(pEncoders[i].MimeType, format) == 0) {
            *pClsid = pEncoders[i].Clsid;
            return (int)i;
        }
    }
    return -1;
}

} // namespace

void ScreenCapture::Start(const CaptureSettings& settings, FrameCallback onFrame) {
    std::fprintf(stderr, "[ScreenCapture] Start called, running=%d\n", m_Running.load() ? 1 : 0);
    std::fflush(stderr);
    if (m_Running.load()) return;
    m_Settings = settings;
    m_OnFrame = std::move(onFrame);
    m_Running.store(true);

    if (m_Thread.joinable()) m_Thread.join();

    m_Thread = std::thread([this]() {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

        std::fprintf(stderr, "[ScreenCapture] Thread started, initializing GDI+\n");
        std::fflush(stderr);
        Gdiplus::GdiplusStartupInput gdipInput;
        ULONG_PTR gdipToken = 0;
        auto status = Gdiplus::GdiplusStartup(&gdipToken, &gdipInput, nullptr);
        std::fprintf(stderr, "[ScreenCapture] GDI+ init status=%d\n", (int)status);
        std::fflush(stderr);

        if (status == Gdiplus::Ok)
            CaptureLoop();

        Gdiplus::GdiplusShutdown(gdipToken);
        CoUninitialize();
        std::fprintf(stderr, "[ScreenCapture] Thread exiting\n");
        std::fflush(stderr);
    });
}

void ScreenCapture::Stop() {
    m_Running.store(false);
    if (m_Thread.joinable()) m_Thread.join();
}

void ScreenCapture::CaptureLoop() {
    int intervalMs = 1000 / (std::max)(1, m_Settings.fps);
    std::fprintf(stderr, "[ScreenCapture] CaptureLoop started, interval=%dms\n", intervalMs);
    std::fflush(stderr);

    CLSID jpegClsid;
    int encoderIdx = GetEncoderClsid(L"image/jpeg", &jpegClsid);
    std::fprintf(stderr, "[ScreenCapture] JPEG encoder index=%d\n", encoderIdx);
    std::fflush(stderr);
    if (encoderIdx < 0) { m_Running.store(false); return; }

    Gdiplus::EncoderParameters encoderParams;
    encoderParams.Count = 1;
    encoderParams.Parameter[0].Guid = Gdiplus::EncoderQuality;
    encoderParams.Parameter[0].Type = Gdiplus::EncoderParameterValueTypeLong;
    encoderParams.Parameter[0].NumberOfValues = 1;
    ULONG quality = (ULONG)m_Settings.quality;
    encoderParams.Parameter[0].Value = &quality;

    while (m_Running.load()) {
        auto start = std::chrono::steady_clock::now();

        HDC screenDC = GetDC(nullptr);
        if (!screenDC) { Sleep(100); continue; }

        int screenW = GetSystemMetrics(SM_CXSCREEN);
        int screenH = GetSystemMetrics(SM_CYSCREEN);

        float scale = 1.0f;
        if (screenW > m_Settings.maxWidth || screenH > m_Settings.maxHeight) {
            float sx = (float)m_Settings.maxWidth / screenW;
            float sy = (float)m_Settings.maxHeight / screenH;
            scale = (std::min)(sx, sy);
        }
        int outW = (int)(screenW * scale);
        int outH = (int)(screenH * scale);

        HDC memDC = CreateCompatibleDC(screenDC);
        HBITMAP hBmp = CreateCompatibleBitmap(screenDC, outW, outH);
        HBITMAP oldBmp = (HBITMAP)SelectObject(memDC, hBmp);
        SetStretchBltMode(memDC, HALFTONE);
        StretchBlt(memDC, 0, 0, outW, outH, screenDC, 0, 0, screenW, screenH, SRCCOPY);
        SelectObject(memDC, oldBmp);

        // Use CreateStreamOnHGlobal (proven Windows API) instead of custom IStream
        IStream* pStream = nullptr;
        HRESULT hr = CreateStreamOnHGlobal(nullptr, TRUE, &pStream);
        if (SUCCEEDED(hr) && pStream) {
            Gdiplus::Bitmap bmp(hBmp, nullptr);
            auto saveStatus = bmp.Save(pStream, &jpegClsid, &encoderParams);

            static int s_frameCount = 0;
            if (s_frameCount < 5) {
                std::fprintf(stderr, "[ScreenCapture] Frame %d: save_status=%d, %dx%d\n",
                    s_frameCount, (int)saveStatus, outW, outH);
                std::fflush(stderr);
            }

            if (saveStatus == Gdiplus::Ok) {
                // Read JPEG data from stream
                LARGE_INTEGER liZero = {};
                pStream->Seek(liZero, STREAM_SEEK_SET, nullptr);
                STATSTG stat = {};
                pStream->Stat(&stat, STATFLAG_NONAME);
                ULONG jpegSize = (ULONG)stat.cbSize.QuadPart;

                if (jpegSize > 0) {
                    std::vector<uint8_t> jpegData(jpegSize);
                    ULONG bytesRead = 0;
                    pStream->Read(jpegData.data(), jpegSize, &bytesRead);

                    if (s_frameCount < 5) {
                        std::fprintf(stderr, "[ScreenCapture] Frame %d: jpeg_bytes=%lu\n", s_frameCount, bytesRead);
                        std::fflush(stderr);
                    }

                    if (bytesRead > 0 && m_OnFrame)
                        m_OnFrame(jpegData, outW, outH);
                }
            }
            s_frameCount++;
            pStream->Release();
        }

        DeleteObject(hBmp);
        DeleteDC(memDC);
        ReleaseDC(nullptr, screenDC);

        auto elapsed = std::chrono::steady_clock::now() - start;
        int sleepMs = intervalMs - (int)std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
        if (sleepMs > 1) Sleep(sleepMs);
    }
}

std::vector<uint8_t> ScreenCapture::CaptureFrame(int& outWidth, int& outHeight) {
    outWidth = 0; outHeight = 0;
    return {};
}

std::vector<uint8_t> ScreenCapture::CompressBMP(const uint8_t*, int, int, int) {
    return {};
}

} // namespace TalkMe
