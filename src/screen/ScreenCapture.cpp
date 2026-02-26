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

#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "gdiplus.lib")

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

class MemoryStream : public IStream {
public:
    MemoryStream() : m_Ref(1) {}
    std::vector<uint8_t>& GetData() { return m_Data; }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** ppv) override { *ppv = this; AddRef(); return S_OK; }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_Ref; }
    ULONG STDMETHODCALLTYPE Release() override { if (--m_Ref == 0) { delete this; return 0; } return m_Ref; }
    HRESULT STDMETHODCALLTYPE Read(void* pv, ULONG cb, ULONG* pcbRead) override {
        ULONG avail = (ULONG)(m_Data.size() - m_Pos);
        ULONG toRead = (std::min)(cb, avail);
        memcpy(pv, m_Data.data() + m_Pos, toRead);
        m_Pos += toRead;
        if (pcbRead) *pcbRead = toRead;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Write(const void* pv, ULONG cb, ULONG* pcbWritten) override {
        size_t newEnd = m_Pos + cb;
        if (newEnd > m_Data.size()) m_Data.resize(newEnd);
        memcpy(m_Data.data() + m_Pos, pv, cb);
        m_Pos += cb;
        if (pcbWritten) *pcbWritten = cb;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Seek(LARGE_INTEGER dlibMove, DWORD dwOrigin, ULARGE_INTEGER* plibNewPosition) override {
        LONGLONG newPos = 0;
        if (dwOrigin == STREAM_SEEK_SET) newPos = dlibMove.QuadPart;
        else if (dwOrigin == STREAM_SEEK_CUR) newPos = (LONGLONG)m_Pos + dlibMove.QuadPart;
        else if (dwOrigin == STREAM_SEEK_END) newPos = (LONGLONG)m_Data.size() + dlibMove.QuadPart;
        m_Pos = (size_t)newPos;
        if (plibNewPosition) plibNewPosition->QuadPart = newPos;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetSize(ULARGE_INTEGER libNewSize) override { m_Data.resize((size_t)libNewSize.QuadPart); return S_OK; }
    HRESULT STDMETHODCALLTYPE CopyTo(IStream*, ULARGE_INTEGER, ULARGE_INTEGER*, ULARGE_INTEGER*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Commit(DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Revert() override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE LockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Stat(STATSTG* pstatstg, DWORD) override {
        memset(pstatstg, 0, sizeof(*pstatstg));
        pstatstg->cbSize.QuadPart = m_Data.size();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Clone(IStream**) override { return E_NOTIMPL; }
private:
    std::vector<uint8_t> m_Data;
    size_t m_Pos = 0;
    ULONG m_Ref;
};

} // namespace

void ScreenCapture::Start(const CaptureSettings& settings, FrameCallback onFrame) {
    if (m_Running.load()) return;
    m_Settings = settings;
    m_OnFrame = std::move(onFrame);
    m_Running.store(true);

    m_Thread = std::thread([this]() {
        Gdiplus::GdiplusStartupInput gdipInput;
        ULONG_PTR gdipToken = 0;
        Gdiplus::GdiplusStartup(&gdipToken, &gdipInput, nullptr);

        CaptureLoop();

        Gdiplus::GdiplusShutdown(gdipToken);
    });
}

void ScreenCapture::Stop() {
    m_Running.store(false);
    if (m_Thread.joinable()) m_Thread.join();
}

void ScreenCapture::CaptureLoop() {
    int intervalMs = 1000 / (std::max)(1, m_Settings.fps);

    CLSID jpegClsid;
    GetEncoderClsid(L"image/jpeg", &jpegClsid);

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

        {
            Gdiplus::Bitmap bmp(hBmp, nullptr);
            auto* memStream = new MemoryStream();
            bmp.Save(memStream, &jpegClsid, &encoderParams);

            auto& jpegData = memStream->GetData();
            if (!jpegData.empty() && m_OnFrame)
                m_OnFrame(jpegData, outW, outH);

            memStream->Release();
        }

        SelectObject(memDC, oldBmp);
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
