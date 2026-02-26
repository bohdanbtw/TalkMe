#include "ScreenCapture.h"
#include <chrono>
#include <algorithm>
#include <wingdi.h>

#pragma comment(lib, "gdi32.lib")

namespace TalkMe {

void ScreenCapture::Start(const CaptureSettings& settings, FrameCallback onFrame) {
    if (m_Running.load()) return;
    m_Settings = settings;
    m_OnFrame = std::move(onFrame);
    m_Running.store(true);
    m_Thread = std::thread(&ScreenCapture::CaptureLoop, this);
}

void ScreenCapture::Stop() {
    m_Running.store(false);
    if (m_Thread.joinable()) m_Thread.join();
}

void ScreenCapture::CaptureLoop() {
    int intervalMs = 1000 / std::max(1, m_Settings.fps);
    while (m_Running.load()) {
        auto start = std::chrono::steady_clock::now();

        int w = 0, h = 0;
        auto frame = CaptureFrame(w, h);
        if (!frame.empty() && m_OnFrame) {
            auto compressed = CompressBMP(frame.data(), w, h, m_Settings.quality);
            if (!compressed.empty())
                m_OnFrame(compressed, w, h);
        }

        auto elapsed = std::chrono::steady_clock::now() - start;
        auto sleepMs = intervalMs - (int)std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
        if (sleepMs > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));
    }
}

std::vector<uint8_t> ScreenCapture::CaptureFrame(int& outWidth, int& outHeight) {
    HDC screenDC = GetDC(nullptr);
    if (!screenDC) return {};

    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);

    float scale = 1.0f;
    if (screenW > m_Settings.maxWidth || screenH > m_Settings.maxHeight) {
        float sx = (float)m_Settings.maxWidth / screenW;
        float sy = (float)m_Settings.maxHeight / screenH;
        scale = std::min(sx, sy);
    }

    outWidth = (int)(screenW * scale);
    outHeight = (int)(screenH * scale);

    HDC memDC = CreateCompatibleDC(screenDC);
    HBITMAP hBmp = CreateCompatibleBitmap(screenDC, outWidth, outHeight);
    HBITMAP oldBmp = (HBITMAP)SelectObject(memDC, hBmp);

    SetStretchBltMode(memDC, HALFTONE);
    StretchBlt(memDC, 0, 0, outWidth, outHeight, screenDC, 0, 0, screenW, screenH, SRCCOPY);

    BITMAPINFOHEADER bi = {};
    bi.biSize = sizeof(bi);
    bi.biWidth = outWidth;
    bi.biHeight = -outHeight;
    bi.biPlanes = 1;
    bi.biBitCount = 24;
    bi.biCompression = BI_RGB;

    int rowBytes = ((outWidth * 3 + 3) & ~3);
    int dataSize = rowBytes * outHeight;
    std::vector<uint8_t> pixels(dataSize);

    GetDIBits(memDC, hBmp, 0, outHeight, pixels.data(), (BITMAPINFO*)&bi, DIB_RGB_COLORS);

    SelectObject(memDC, oldBmp);
    DeleteObject(hBmp);
    DeleteDC(memDC);
    ReleaseDC(nullptr, screenDC);

    return pixels;
}

std::vector<uint8_t> ScreenCapture::CompressBMP(const uint8_t* bmpData, int width, int height, int quality) {
    // Simple BMP-to-raw compression: pack RGB tightly and apply basic RLE-like compression
    // For production, this should use libjpeg-turbo or similar. For now, send raw BMP
    // with a size header so the receiver can reconstruct.
    int rowBytes = ((width * 3 + 3) & ~3);
    
    // Build a minimal BMP file in memory
    int pixelDataSize = rowBytes * height;
    int fileSize = 54 + pixelDataSize;
    
    std::vector<uint8_t> bmp(fileSize);
    
    // BMP header
    bmp[0] = 'B'; bmp[1] = 'M';
    memcpy(&bmp[2], &fileSize, 4);
    int offset = 54;
    memcpy(&bmp[10], &offset, 4);
    
    // DIB header
    int headerSize = 40;
    memcpy(&bmp[14], &headerSize, 4);
    memcpy(&bmp[18], &width, 4);
    int negHeight = -height;
    memcpy(&bmp[22], &negHeight, 4);
    short planes = 1, bpp = 24;
    memcpy(&bmp[26], &planes, 2);
    memcpy(&bmp[28], &bpp, 2);
    memcpy(&bmp[34], &pixelDataSize, 4);
    
    memcpy(&bmp[54], bmpData, pixelDataSize);
    
    return bmp;
}

} // namespace TalkMe
