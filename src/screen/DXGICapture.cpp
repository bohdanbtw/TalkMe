#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "DXGICapture.h"
#include <d3d11.h>
#include <dxgi1_2.h>
#include <windows.h>
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

namespace TalkMe {

namespace {

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

} // namespace

void DXGICapture::Start(const DXGICaptureSettings& settings, FrameCallback onFrame) {
    if (m_Running.load()) return;
    m_Settings = settings;
    m_OnFrame = std::move(onFrame);
    m_Running.store(true);

    m_Thread = std::thread([this]() {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        Gdiplus::GdiplusStartupInput gdipInput;
        ULONG_PTR gdipToken = 0;
        Gdiplus::GdiplusStartup(&gdipToken, &gdipInput, nullptr);

        if (InitDXGI()) {
            std::fprintf(stderr, "[DXGICapture] DXGI initialized: %dx%d\n", m_CaptureWidth, m_CaptureHeight);
            std::fflush(stderr);
            CaptureLoop();
        } else {
            std::fprintf(stderr, "[DXGICapture] DXGI init failed\n");
            std::fflush(stderr);
        }

        Cleanup();
        Gdiplus::GdiplusShutdown(gdipToken);
        CoUninitialize();
    });
}

void DXGICapture::Stop() {
    m_Running.store(false);
    if (m_Thread.joinable()) m_Thread.join();
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

void DXGICapture::CaptureLoop() {
    int intervalMs = 1000 / (std::max)(1, m_Settings.fps);
    int frameCount = 0;

    while (m_Running.load()) {
        auto start = std::chrono::steady_clock::now();

        IDXGIResource* desktopResource = nullptr;
        DXGI_OUTDUPL_FRAME_INFO frameInfo;
        HRESULT hr = m_Duplication->AcquireNextFrame(100, &frameInfo, &desktopResource);

        if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
            auto elapsed = std::chrono::steady_clock::now() - start;
            int sleepMs = intervalMs - (int)std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
            if (sleepMs > 0) Sleep(sleepMs);
            continue;
        }

        if (hr == DXGI_ERROR_ACCESS_LOST) {
            std::fprintf(stderr, "[DXGICapture] Access lost, reinitializing...\n");
            Cleanup();
            Sleep(500);
            if (!InitDXGI()) { m_Running.store(false); return; }
            continue;
        }

        if (FAILED(hr)) {
            if (frameCount < 3) {
                std::fprintf(stderr, "[DXGICapture] AcquireNextFrame failed: 0x%08lx\n", hr);
                std::fflush(stderr);
            }
            Sleep(50);
            continue;
        }

        ID3D11Texture2D* desktopTexture = nullptr;
        desktopResource->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&desktopTexture);
        desktopResource->Release();

        // Copy to staging texture
        m_Context->CopyResource(m_StagingTexture, desktopTexture);
        desktopTexture->Release();
        m_Duplication->ReleaseFrame();

        // Map staging texture for CPU read
        D3D11_MAPPED_SUBRESOURCE mapped;
        hr = m_Context->Map(m_StagingTexture, 0, D3D11_MAP_READ, 0, &mapped);
        if (SUCCEEDED(hr)) {
            // Copy BGRA data (handle pitch != width*4)
            int outW = m_CaptureWidth;
            int outH = m_CaptureHeight;

            // Scale down if needed
            float scale = 1.0f;
            if (outW > m_Settings.maxWidth || outH > m_Settings.maxHeight) {
                float sx = (float)m_Settings.maxWidth / outW;
                float sy = (float)m_Settings.maxHeight / outH;
                scale = (std::min)(sx, sy);
            }

            std::vector<uint8_t> bgraData(outW * outH * 4);
            for (int y = 0; y < outH; y++) {
                memcpy(bgraData.data() + y * outW * 4,
                    (uint8_t*)mapped.pData + y * mapped.RowPitch,
                    outW * 4);
            }
            m_Context->Unmap(m_StagingTexture, 0);

            // Encode to JPEG (fallback — H.264 MFT can be added later for even better perf)
            auto jpeg = EncodeRGBAtoJPEG(bgraData.data(), outW, outH, m_Settings.quality);

            if (frameCount < 3) {
                std::fprintf(stderr, "[DXGICapture] Frame %d: %dx%d, jpeg=%zu bytes\n",
                    frameCount, outW, outH, jpeg.size());
                std::fflush(stderr);
            }

            if (!jpeg.empty() && m_OnFrame)
                m_OnFrame(jpeg, outW, outH, (frameCount % 30 == 0));

            frameCount++;
        }

        auto elapsed = std::chrono::steady_clock::now() - start;
        int sleepMs = intervalMs - (int)std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
        if (sleepMs > 0) Sleep(sleepMs);
    }
}

void DXGICapture::Cleanup() {
    if (m_StagingTexture) { m_StagingTexture->Release(); m_StagingTexture = nullptr; }
    if (m_Duplication) { m_Duplication->Release(); m_Duplication = nullptr; }
    if (m_Context) { m_Context->Release(); m_Context = nullptr; }
    if (m_Device) { m_Device->Release(); m_Device = nullptr; }
}

} // namespace TalkMe
