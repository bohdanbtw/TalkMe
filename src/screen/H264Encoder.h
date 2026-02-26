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

    bool Initialize(int width, int height, int fps, int bitrateKbps);
    std::vector<uint8_t> Encode(const uint8_t* bgraData, int width, int height);
    void Shutdown();

    bool IsInitialized() const { return m_Initialized; }

private:
    bool m_Initialized = false;
    IMFTransform* m_Encoder = nullptr;
    DWORD m_InputStreamID = 0;
    DWORD m_OutputStreamID = 0;
    int m_Width = 0;
    int m_Height = 0;
    int m_Fps = 30;
    int64_t m_FrameIndex = 0;
    MFT_OUTPUT_STREAM_INFO m_OutputInfo = {};
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
