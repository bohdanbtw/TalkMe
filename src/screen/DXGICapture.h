#pragma once
#include <d3d11.h>
#include <dxgi1_2.h>
#include <vector>
#include <cstdint>
#include <atomic>
#include <thread>
#include <functional>
#include <mutex>

namespace TalkMe {

struct DXGICaptureSettings {
    int fps = 30;
    int quality = 70;       // 0-100
    int maxWidth = 1920;
    int maxHeight = 1080;
};

class DXGICapture {
public:
    using FrameCallback = std::function<void(const std::vector<uint8_t>& encodedData, int width, int height, bool isKeyFrame)>;

    DXGICapture() = default;
    ~DXGICapture() { Stop(); }

    void Start(const DXGICaptureSettings& settings, FrameCallback onFrame);
    void Stop();
    bool IsRunning() const { return m_Running.load(); }

private:
    void CaptureLoop();
    bool InitDXGI();
    bool InitEncoder(int width, int height);
    std::vector<uint8_t> EncodeFrame(ID3D11Texture2D* texture);
    void Cleanup();

    std::atomic<bool> m_Running{ false };
    std::thread m_Thread;
    DXGICaptureSettings m_Settings;
    FrameCallback m_OnFrame;

    // DXGI
    ID3D11Device* m_Device = nullptr;
    ID3D11DeviceContext* m_Context = nullptr;
    IDXGIOutputDuplication* m_Duplication = nullptr;
    ID3D11Texture2D* m_StagingTexture = nullptr;
    int m_CaptureWidth = 0;
    int m_CaptureHeight = 0;

    // Fallback: if MF encoder fails, use JPEG
    bool m_UseJpegFallback = false;
};

} // namespace TalkMe
