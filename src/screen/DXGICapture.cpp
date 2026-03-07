#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "DXGICapture.h"
#include <d3d11.h>
#include <dxgi1_2.h>
#include <windows.h>
#include <mmsystem.h>
#include <chrono>
#include <algorithm>
#include <cstdio>
#include <cstring>

// GDI+ for JPEG fallback
#include <objidl.h>
#include <gdiplus.h>
#include <shlwapi.h>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "winmm.lib")

namespace TalkMe {

namespace {

struct TimerResolutionGuard {
    bool active = false;
    TimerResolutionGuard() { active = (::timeBeginPeriod(1) == TIMERR_NOERROR); }
    ~TimerResolutionGuard() { if (active) ::timeEndPeriod(1); }
};

int GetJpegEncoderClsid(CLSID* pClsid) {
    UINT num = 0, size = 0;
    Gdiplus::GetImageEncodersSize(&num, &size);
    if (size == 0) return -1;
    std::vector<uint8_t> buf(size);
    auto* enc = reinterpret_cast<Gdiplus::ImageCodecInfo*>(buf.data());
    Gdiplus::GetImageEncoders(num, size, enc);
    for (UINT i = 0; i < num; ++i) {
        if (wcscmp(enc[i].MimeType, L"image/jpeg") == 0) {
            *pClsid = enc[i].Clsid;
            return (int)i;
        }
    }
    return -1;
}

std::vector<uint8_t> EncodeRGBAtoJPEG(const uint8_t* bgra, int w, int h, int quality) {
    std::vector<uint8_t> result;

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    HDC hdc = GetDC(nullptr);
    void* bits = nullptr;
    HBITMAP hBmp = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!hBmp || !bits) { ReleaseDC(nullptr, hdc); return result; }
    memcpy(bits, bgra, w * h * 4);

    CLSID jpegClsid;
    if (GetJpegEncoderClsid(&jpegClsid) < 0) { DeleteObject(hBmp); ReleaseDC(nullptr, hdc); return result; }

    Gdiplus::EncoderParameters params;
    params.Count = 1;
    params.Parameter[0].Guid = Gdiplus::EncoderQuality;
    params.Parameter[0].Type = Gdiplus::EncoderParameterValueTypeLong;
    params.Parameter[0].NumberOfValues = 1;
    ULONG q = (ULONG)quality;
    params.Parameter[0].Value = &q;

    IStream* pStream = nullptr;
    CreateStreamOnHGlobal(nullptr, TRUE, &pStream);
    if (pStream) {
        Gdiplus::Bitmap bmp(hBmp, nullptr);
        if (bmp.Save(pStream, &jpegClsid, &params) == Gdiplus::Ok) {
            LARGE_INTEGER zero = {};
            pStream->Seek(zero, STREAM_SEEK_SET, nullptr);
            STATSTG stat = {};
            pStream->Stat(&stat, STATFLAG_NONAME);
            ULONG sz = (ULONG)stat.cbSize.QuadPart;
            result.resize(sz);
            ULONG read = 0;
            pStream->Read(result.data(), sz, &read);
            result.resize(read);
        }
        pStream->Release();
    }
    DeleteObject(hBmp);
    ReleaseDC(nullptr, hdc);
    return result;
}

void DrawCursorOntoBgra(uint8_t* bgra, int bufW, int bufH, int cursorX, int cursorY) {
    CURSORINFO ci = {};
    ci.cbSize = sizeof(ci);
    if (!GetCursorInfo(&ci) || !ci.hCursor || (ci.flags != CURSOR_SHOWING)) return;
    int x = cursorX;
    int y = cursorY;
    if (x >= bufW || y >= bufH || x + 32 <= 0 || y + 32 <= 0) return;
    HDC screenDc = GetDC(nullptr);
    if (!screenDc) return;
    HDC memDc = CreateCompatibleDC(screenDc);
    if (!memDc) { ReleaseDC(nullptr, screenDc); return; }
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
    bmi.bmiHeader.biWidth = bufW;
    bmi.bmiHeader.biHeight = -bufH;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP hBmp = CreateDIBSection(memDc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!hBmp || !bits) { DeleteDC(memDc); ReleaseDC(nullptr, screenDc); return; }
    HBITMAP old = (HBITMAP)SelectObject(memDc, hBmp);
    memcpy(bits, bgra, (size_t)bufW * bufH * 4);
    SetStretchBltMode(memDc, HALFTONE);
    DrawIconEx(memDc, x, y, ci.hCursor, 0, 0, 0, nullptr, DI_NORMAL);
    memcpy(bgra, bits, (size_t)bufW * bufH * 4);
    SelectObject(memDc, old);
    DeleteObject(hBmp);
    DeleteDC(memDc);
    ReleaseDC(nullptr, screenDc);
}

void BoxDownscaleBgra(const uint8_t* src, int srcW, int srcH, int srcPitch,
    uint8_t* dst, int dstW, int dstH) {
    const float fx = (float)srcW / (float)(std::max)(1, dstW);
    const float fy = (float)srcH / (float)(std::max)(1, dstH);
    for (int y = 0; y < dstH; y++) {
        int sy = (int)(y * fy);
        if (sy >= srcH) sy = srcH - 1;
        const uint8_t* row = src + (size_t)sy * srcPitch;
        uint8_t* outRow = dst + (size_t)y * dstW * 4;
        for (int x = 0; x < dstW; x++) {
            int sx = (int)(x * fx);
            if (sx >= srcW) sx = srcW - 1;
            int idx = sx * 4;
            outRow[x * 4 + 0] = row[idx + 0];
            outRow[x * 4 + 1] = row[idx + 1];
            outRow[x * 4 + 2] = row[idx + 2];
            outRow[x * 4 + 3] = row[idx + 3];
        }
    }
}

} // namespace

void DXGICapture::Start(const DXGICaptureSettings& settings, FrameCallback onFrame) {
    Stop(); // ensure previous session is fully cleaned up
    m_Settings = settings;
    m_OnFrame = std::move(onFrame);
    m_UseJpegFallback = false;
    m_Running.store(true);

    m_Thread = std::thread([this]() {
        // 1ms timers are important for accurate 60/120 FPS pacing in this worker.
        TimerResolutionGuard timerResolutionGuard;
        // Run at normal priority so selected stream FPS is actually reachable.
        ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_NORMAL);
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        Gdiplus::GdiplusStartupInput gdipInput;
        ULONG_PTR gdipToken = 0;
        Gdiplus::GdiplusStartup(&gdipToken, &gdipInput, nullptr);

        bool dxgiOk = InitDXGI();
        if (dxgiOk) {
            std::fprintf(stderr, "[DXGICapture] DXGI initialized: %dx%d\n", m_CaptureWidth, m_CaptureHeight);
            std::fflush(stderr);
            CaptureLoop();
        } else {
            std::fprintf(stderr, "[DXGICapture] DXGI init failed, falling back to GDI capture\n");
            std::fflush(stderr);
            GDIFallbackLoop();
        }

        Cleanup();
        Gdiplus::GdiplusShutdown(gdipToken);
        CoUninitialize();
        m_Running.store(false);
    });
}

void DXGICapture::Stop() {
    m_Running.store(false);
    if (m_Thread.joinable()) m_Thread.join();
    m_H264Encoder.Shutdown();
}

void DXGICapture::RunLoopSynchronous(const DXGICaptureSettings& settings, std::atomic<bool>* stopFlag, FrameCallback onFrame) {
    if (!stopFlag) return;
    m_Settings = settings;
    m_OnFrame = std::move(onFrame);
    m_UseJpegFallback = false;
    TimerResolutionGuard timerResolutionGuard;
    ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_NORMAL);

    bool dxgiOk = InitDXGI();
    if (dxgiOk) {
        std::fprintf(stderr, "[DXGICapture] RunLoopSynchronous: %dx%d\n", m_CaptureWidth, m_CaptureHeight);
        CaptureLoop(stopFlag);
    } else {
        std::fprintf(stderr, "[DXGICapture] RunLoopSynchronous: DXGI failed, GDI fallback\n");
        GDIFallbackLoop(stopFlag);
    }
    Cleanup();
    m_H264Encoder.Shutdown();
}

bool DXGICapture::InitDXGI() {
    D3D_FEATURE_LEVEL featureLevel;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
        &m_Device, &featureLevel, &m_Context);
    if (FAILED(hr)) {
        std::fprintf(stderr, "[DXGICapture] D3D11CreateDevice failed: 0x%08lx\n", hr);
        return false;
    }

    IDXGIDevice* dxgiDevice = nullptr;
    m_Device->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDevice);
    IDXGIAdapter* adapter = nullptr;
    dxgiDevice->GetAdapter(&adapter);
    IDXGIOutput* output = nullptr;
    hr = adapter->EnumOutputs(0, &output);
    if (FAILED(hr)) {
        std::fprintf(stderr, "[DXGICapture] EnumOutputs failed\n");
        adapter->Release(); dxgiDevice->Release();
        return false;
    }

    IDXGIOutput1* output1 = nullptr;
    output->QueryInterface(__uuidof(IDXGIOutput1), (void**)&output1);

    DXGI_OUTPUT_DESC desc;
    output->GetDesc(&desc);
    m_CaptureWidth = desc.DesktopCoordinates.right - desc.DesktopCoordinates.left;
    m_CaptureHeight = desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top;

    hr = output1->DuplicateOutput(m_Device, &m_Duplication);
    if (FAILED(hr)) {
        std::fprintf(stderr, "[DXGICapture] DuplicateOutput failed: 0x%08lx\n", hr);
        output1->Release(); output->Release(); adapter->Release(); dxgiDevice->Release();
        return false;
    }

    // Create staging texture for CPU readback
    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = m_CaptureWidth;
    texDesc.Height = m_CaptureHeight;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_STAGING;
    texDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    hr = m_Device->CreateTexture2D(&texDesc, nullptr, &m_StagingTexture);

    output1->Release();
    output->Release();
    adapter->Release();
    dxgiDevice->Release();

    return SUCCEEDED(hr);
}

void DXGICapture::CaptureLoop(std::atomic<bool>* externalStop) {
    const int targetFps = (std::max)(1, m_Settings.fps);
    const auto frameInterval = std::chrono::microseconds(1'000'000 / targetFps);
    auto nextFrameDeadline = std::chrono::steady_clock::now();
    int frameCount = 0;

    std::vector<uint8_t> bgraBuffer;
    std::vector<uint8_t> packetBuffer;
    int prevOutW = 0, prevOutH = 0;

    // Smart timeout: Don't spin too fast at high FPS
    // Use longer timeout (25ms) to reduce CPU usage and jitter
    // DXGI will return earlier if frame is available
    UINT waitTimeoutMs = 25;  // Fixed 25ms - good compromise for all FPS

    while ((externalStop ? !externalStop->load() : m_Running.load())) {
        // Wait for next frame with appropriate timeout
        IDXGIResource* desktopResource = nullptr;
        DXGI_OUTDUPL_FRAME_INFO frameInfo;
        HRESULT hr = m_Duplication->AcquireNextFrame(waitTimeoutMs, &frameInfo, &desktopResource);

        if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
            continue;
        }

        if (hr == DXGI_ERROR_ACCESS_LOST) {
            Cleanup();
            Sleep(500);
            if (!InitDXGI()) { m_Running.store(false); return; }
            continue;
        }

        if (FAILED(hr)) {
            Sleep(50);
            continue;
        }

        // Rate limit: if the monitor provides frames faster than our target, skip excess
        auto now = std::chrono::steady_clock::now();
        if (now < nextFrameDeadline) {
            desktopResource->Release();
            m_Duplication->ReleaseFrame();
            continue;
        }

        ID3D11Texture2D* desktopTexture = nullptr;
        desktopResource->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&desktopTexture);
        desktopResource->Release();

        m_Context->CopyResource(m_StagingTexture, desktopTexture);
        desktopTexture->Release();
        m_Duplication->ReleaseFrame();

        D3D11_MAPPED_SUBRESOURCE mapped;
        hr = m_Context->Map(m_StagingTexture, 0, D3D11_MAP_READ, 0, &mapped);
        if (SUCCEEDED(hr)) {
            // Region of interest (default: entire desktop)
            int srcX = 0, srcY = 0, srcW = m_CaptureWidth, srcH = m_CaptureHeight;
            HWND targetHwnd = static_cast<HWND>(m_Settings.targetWindow);
            if (targetHwnd && ::IsWindow(targetHwnd)) {
                RECT wr = {};
                if (::GetWindowRect(targetHwnd, &wr)) {
                    srcX = (std::max)(0, (int)wr.left);
                    srcY = (std::max)(0, (int)wr.top);
                    srcW = (std::min)(m_CaptureWidth - srcX, (int)(wr.right - wr.left));
                    srcH = (std::min)(m_CaptureHeight - srcY, (int)(wr.bottom - wr.top));
                    if (srcW < 1) srcW = 1;
                    if (srcH < 1) srcH = 1;
                }
            }

            // SOLUTION D: Automatic Resolution Reduction at High FPS
            // At 120+ FPS, cap to 720p (50% fewer pixels)
            // At 100-119 FPS, cap to 800p (38% fewer pixels)  
            // At 80-99 FPS, cap to 900p (19% fewer pixels)
            // At 60-79 FPS, keep native resolution

            int maxEncW = m_Settings.maxWidth;
            int maxEncH = m_Settings.maxHeight;
            bool resolutionLimited = false;

            // Solution D: Cap resolution at high FPS for faster encoding
            if (targetFps >= 120) {
                maxEncH = 720;  // 50% pixel reduction
                resolutionLimited = true;
            } else if (targetFps >= 100) {
                maxEncH = 800;  // 38% pixel reduction
                resolutionLimited = true;
            } else if (targetFps >= 80) {
                maxEncH = 900;  // 19% pixel reduction
                resolutionLimited = true;
            }

            // Log when resolution is limited for performance
            if (resolutionLimited && targetFps % 30 == 0) {  // Log once per ~30 frames
                static int logCounter = 0;
                if (logCounter++ % 30 == 0) {
                    std::fprintf(stderr, "[DXGICapture] Resolution capped: %dp @ %d FPS (50%% encoding speedup)\n",
                               maxEncH, targetFps);
                    std::fflush(stderr);
                }
            }

            // Maintain aspect ratio
            maxEncW = (int)(maxEncW * (maxEncH / (float)m_CaptureHeight));

            float scale = 1.0f;
            if (srcW > maxEncW || srcH > maxEncH)
                scale = (std::min)((float)maxEncW / srcW, (float)maxEncH / srcH);
            int outW = (std::max)(1, (int)(srcW * scale));
            int outH = (std::max)(1, (int)(srcH * scale));

            const size_t bgraSize = (size_t)outW * outH * 4;
            if (outW != prevOutW || outH != prevOutH) {
                bgraBuffer.resize(bgraSize);
                prevOutW = outW;
                prevOutH = outH;
                if (resolutionLimited) {
                    std::fprintf(stderr, "[DXGICapture] Resolution changed: %dx%d (50%% fewer pixels)\n", 
                               outW, outH);
                    std::fflush(stderr);
                }
            }
            // Offset source pointer to the region of interest
            const uint8_t* srcBase = (const uint8_t*)mapped.pData + (size_t)srcY * mapped.RowPitch + (size_t)srcX * 4;
            if (outW == srcW && outH == srcH) {
                for (int y = 0; y < outH; y++)
                    memcpy(bgraBuffer.data() + (size_t)y * outW * 4,
                        srcBase + (size_t)y * mapped.RowPitch, (size_t)outW * 4);
            } else {
                BoxDownscaleBgra(srcBase, srcW, srcH, (int)mapped.RowPitch,
                    bgraBuffer.data(), outW, outH);
            }
            m_Context->Unmap(m_StagingTexture, 0);

            CURSORINFO ci = {};
            ci.cbSize = sizeof(ci);
            if (GetCursorInfo(&ci) && ci.hCursor && ci.flags == CURSOR_SHOWING)
                DrawCursorOntoBgra(bgraBuffer.data(), outW, outH,
                    (int)((ci.ptScreenPos.x - srcX) * scale),
                    (int)((ci.ptScreenPos.y - srcY) * scale));

            if (!m_UseJpegFallback && !m_H264Encoder.IsInitialized()) {
                // Calculate bitrate using current quality (may be adaptive)
                int effectiveQuality = m_Settings.quality;

                // Check if adaptive quality is available and different
                if (m_Settings.pAdaptiveQuality) {
                    int adaptiveQuality = m_Settings.pAdaptiveQuality->load();
                    if (adaptiveQuality != effectiveQuality) {
                        effectiveQuality = adaptiveQuality;
                        std::fprintf(stderr, "[DXGICapture] Using adaptive quality: %d%%\n", effectiveQuality);
                        std::fflush(stderr);
                    }
                }

                // Conservative bitrate scaling - prioritize quality over bandwidth
                // Artifacts are worse than higher bandwidth!
                int baseBitrate = 500 + effectiveQuality * 12;

                // Minimal reduction at high FPS - H.264 needs room to encode
                if (targetFps >= 120) {
                    baseBitrate = (baseBitrate * 85) / 100;  // Only 15% reduction
                } else if (targetFps >= 100) {
                    baseBitrate = (baseBitrate * 90) / 100;  // Only 10% reduction
                } else if (targetFps >= 80) {
                    baseBitrate = (baseBitrate * 95) / 100;  // Only 5% reduction
                }
                // At ≤60 FPS: NO reduction

                // Keyframe-only mode NOT supported with H.264 - disable it
                if (m_Settings.keyframeOnlyMode && !m_UseJpegFallback) {
                    std::fprintf(stderr, "[DXGICapture] WARNING: Keyframe-only with H.264 causes artifacts - forcing normal mode\n");
                    // Don't apply additional reduction
                }

                int bitrate = (std::min)(3000, (std::max)(600, baseBitrate));
                if (!m_H264Encoder.Initialize(outW, outH, m_Settings.fps, bitrate, m_Device))
                    m_UseJpegFallback = true;
            }

            // NEVER skip frames with H.264 - temporal prediction requires all frames
            // Keyframe-only mode only works with JPEG (stateless)
            bool isKeyframe = (frameCount % (std::max)(15, m_Settings.keyframeIntervalFrames) == 0);
            bool canSkipFrames = m_UseJpegFallback;  // JPEG only!
            bool shouldEncodeFrame = !canSkipFrames || !m_Settings.keyframeOnlyMode || isKeyframe;

            std::vector<uint8_t> encoded;
            if (shouldEncodeFrame) {
                // Encode frame; never fall back to JPEG when H264 is active
                // (H264 returns empty on first frame due to pipeline latency — not an error)
                if (!m_UseJpegFallback && m_H264Encoder.IsInitialized()) {
                    encoded = m_H264Encoder.Encode(bgraBuffer.data(), outW, outH);
                } else {
                    encoded = EncodeRGBAtoJPEG(bgraBuffer.data(), outW, outH, m_Settings.quality);
                }
            }

            if (!encoded.empty() && m_OnFrame) {
                const uint8_t codec = (m_UseJpegFallback || !m_H264Encoder.IsInitialized()) ? 0 : 1;
                // Reuse packet buffer: [codec_byte | encoded_data]
                packetBuffer.resize(1 + encoded.size());
                packetBuffer[0] = codec;
                memcpy(packetBuffer.data() + 1, encoded.data(), encoded.size());
                m_OnFrame(packetBuffer, outW, outH, isKeyframe);
            }
            frameCount++;
        }

        // Advance deadline for rate limiting
        nextFrameDeadline += frameInterval;
        auto n = std::chrono::steady_clock::now();
        if (nextFrameDeadline <= n)
            nextFrameDeadline = n;
    }
}

void DXGICapture::GDIFallbackLoop(std::atomic<bool>* externalStop) {
    const int targetFps = (std::max)(1, m_Settings.fps);
    const auto frameInterval = std::chrono::microseconds(1'000'000 / targetFps);
    auto nextFrameDeadline = std::chrono::steady_clock::now();
    int frameCount = 0;

    CLSID jpegClsid;
    if (GetJpegEncoderClsid(&jpegClsid) < 0) return;

    Gdiplus::EncoderParameters params;
    params.Count = 1;
    params.Parameter[0].Guid = Gdiplus::EncoderQuality;
    params.Parameter[0].Type = Gdiplus::EncoderParameterValueTypeLong;
    params.Parameter[0].NumberOfValues = 1;
    ULONG quality = (ULONG)m_Settings.quality;
    params.Parameter[0].Value = &quality;

    std::vector<uint8_t> packetBuffer;

    auto advanceDeadline = [&]() {
        nextFrameDeadline += frameInterval;
        auto n = std::chrono::steady_clock::now();
        if (nextFrameDeadline <= n)
            do { nextFrameDeadline += frameInterval; } while (nextFrameDeadline <= n);
    };

    while ((externalStop ? !externalStop->load() : m_Running.load())) {
        auto now = std::chrono::steady_clock::now();
        if (now < nextFrameDeadline) {
            auto remainMs = std::chrono::duration_cast<std::chrono::milliseconds>(nextFrameDeadline - now).count();
            if (remainMs > 0) Sleep((DWORD)remainMs);
        }

        HDC screenDC = GetDC(nullptr);
        if (!screenDC) { Sleep(100); continue; }
        int screenW = GetSystemMetrics(SM_CXSCREEN);
        int screenH = GetSystemMetrics(SM_CYSCREEN);
        float scale = 1.0f;
        if (screenW > m_Settings.maxWidth || screenH > m_Settings.maxHeight)
            scale = (std::min)((float)m_Settings.maxWidth / screenW, (float)m_Settings.maxHeight / screenH);
        int outW = (std::max)(1, (int)(screenW * scale));
        int outH = (std::max)(1, (int)(screenH * scale));

        HDC memDC = CreateCompatibleDC(screenDC);
        HBITMAP hBmp = CreateCompatibleBitmap(screenDC, outW, outH);
        HBITMAP oldBmp = (HBITMAP)SelectObject(memDC, hBmp);
        SetStretchBltMode(memDC, HALFTONE);
        StretchBlt(memDC, 0, 0, outW, outH, screenDC, 0, 0, screenW, screenH, SRCCOPY);
        CURSORINFO ci = {};
        ci.cbSize = sizeof(ci);
        if (GetCursorInfo(&ci) && ci.hCursor && ci.flags == CURSOR_SHOWING) {
            int cx = (int)(ci.ptScreenPos.x * scale);
            int cy = (int)(ci.ptScreenPos.y * scale);
            if (cx >= 0 && cy >= 0 && cx < outW && cy < outH)
                DrawIconEx(memDC, cx, cy, ci.hCursor, 0, 0, 0, nullptr, DI_NORMAL);
        }
        SelectObject(memDC, oldBmp);

        IStream* pStream = nullptr;
        CreateStreamOnHGlobal(nullptr, TRUE, &pStream);
        if (pStream) {
            Gdiplus::Bitmap bmp(hBmp, nullptr);
            if (bmp.Save(pStream, &jpegClsid, &params) == Gdiplus::Ok) {
                LARGE_INTEGER zero = {};
                pStream->Seek(zero, STREAM_SEEK_SET, nullptr);
                STATSTG stat = {};
                pStream->Stat(&stat, STATFLAG_NONAME);
                ULONG sz = (ULONG)stat.cbSize.QuadPart;
                if (sz > 0) {
                    packetBuffer.resize(1 + sz);
                    packetBuffer[0] = 0;
                    ULONG read = 0;
                    pStream->Read(packetBuffer.data() + 1, sz, &read);
                    if (read > 0 && m_OnFrame) {
                        packetBuffer.resize(1 + read);
                        m_OnFrame(packetBuffer, outW, outH, (frameCount % 30 == 0));
                    }
                }
            }
            pStream->Release();
        }

        DeleteObject(hBmp);
        DeleteDC(memDC);
        ReleaseDC(nullptr, screenDC);
        frameCount++;
        advanceDeadline();
    }
}

void DXGICapture::Cleanup() {
    if (m_StagingTexture) { m_StagingTexture->Release(); m_StagingTexture = nullptr; }
    if (m_Duplication) { m_Duplication->Release(); m_Duplication = nullptr; }
    if (m_Context) { m_Context->Release(); m_Context = nullptr; }
    if (m_Device) { m_Device->Release(); m_Device = nullptr; }
}

} // namespace TalkMe
