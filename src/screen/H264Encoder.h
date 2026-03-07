#pragma once
#include <d3d11.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mftransform.h>
#include <codecapi.h>
#include <vector>
#include <cstdint>

#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "mfreadwrite.lib")

namespace TalkMe {

class H264Encoder {
public:
    H264Encoder() = default;
    ~H264Encoder() { Shutdown(); }

    bool Initialize(int width, int height, int fps, int bitrateKbps, ID3D11Device* d3dDevice = nullptr);
    std::vector<uint8_t> Encode(const uint8_t* bgraData, int width, int height);
    void Shutdown();

    bool IsInitialized() const { return m_Initialized; }
    bool IsGPUConversionAvailable() const { return m_GPUConversionReady; }

    // Encoder health monitoring (for frame skipping decision)
    float GetLastProcessOutputTimeMs() const { return m_LastProcessOutputTimeMs; }
    bool IsEncoderLagging() const { return m_LastProcessOutputTimeMs > 8.5f; }
    int GetNeedMoreInputCount() const { return m_NeedMoreInputCount; }

private:
    bool m_Initialized = false;
    bool m_GPUConversionReady = false;
    IMFTransform* m_Encoder = nullptr;
    DWORD m_InputStreamID = 0;
    DWORD m_OutputStreamID = 0;
    int m_Width = 0;
    int m_Height = 0;
    int m_Fps = 30;
    int64_t m_FrameIndex = 0;
    MFT_OUTPUT_STREAM_INFO m_OutputInfo = {};

    // GPU capability tracking (for adaptive bitrate)
    bool m_HasGPUEncoder = false;  // Set if hardware encoder detected

    // Encoder health telemetry (for Solution C: Frame Skipping)
    float m_LastProcessOutputTimeMs = 0.0f;
    int m_NeedMoreInputCount = 0;
    int m_FrameSkipCount = 0;

    // GPU resources for BGRA->NV12 conversion
    ID3D11Device* m_D3DDevice = nullptr;
    ID3D11DeviceContext* m_D3DContext = nullptr;
    ID3D11ComputeShader* m_ColorConversionShader = nullptr;
    ID3D11Buffer* m_ConversionParamsCB = nullptr;
    ID3D11Texture2D* m_BGRATexture = nullptr;
    ID3D11ShaderResourceView* m_BGRASRV = nullptr;
    ID3D11Texture2D* m_YTexture = nullptr;
    ID3D11UnorderedAccessView* m_YUAV = nullptr;
    ID3D11Texture2D* m_UVTexture = nullptr;
    ID3D11UnorderedAccessView* m_UVUAV = nullptr;

    bool InitializeGPUConversion(ID3D11Device* device);
    bool ConvertBGRAviaGPU(const uint8_t* bgraData, int width, int height, 
                           uint8_t*& outNV12, DWORD& outNV12Size);
    void ShutdownGPUResources();
};

class H264Decoder {
public:
    H264Decoder() = default;
    ~H264Decoder() { Shutdown(); }

    bool Initialize(int width, int height);
    bool Decode(const uint8_t* h264Data, int dataSize, std::vector<uint8_t>& rgbaOut, int& outWidth, int& outHeight);
    void Shutdown();

    bool IsInitialized() const { return m_Initialized; }

private:
    bool m_Initialized = false;
    IMFTransform* m_Decoder = nullptr;
    DWORD m_InputStreamID = 0;
    DWORD m_OutputStreamID = 0;
    int m_Width = 0;
    int m_Height = 0;
    MFT_OUTPUT_STREAM_INFO m_OutputInfo = {};
};

} // namespace TalkMe
