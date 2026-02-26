#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "H264Encoder.h"
#include <mferror.h>
#include <wmcodecdsp.h>
#include <cstring>
#include <cstdio>
#include <algorithm>

namespace TalkMe {

static IMFSample* CreateSampleFromBGRA(const uint8_t* bgra, int width, int height, int64_t timestamp, int fps) {
    DWORD bufSize = width * height * 4;
    IMFMediaBuffer* pBuffer = nullptr;
    MFCreateMemoryBuffer(bufSize, &pBuffer);
    BYTE* pData = nullptr;
    pBuffer->Lock(&pData, nullptr, nullptr);
    // Convert BGRA to NV12 (Y plane + interleaved UV)
    // NV12: Y = 0.299R + 0.587G + 0.114B, U/V subsampled 2x2
    DWORD nv12Size = width * height * 3 / 2;
    IMFMediaBuffer* pNV12Buf = nullptr;
    MFCreateMemoryBuffer(nv12Size, &pNV12Buf);
    BYTE* nv12 = nullptr;
    pNV12Buf->Lock(&nv12, nullptr, nullptr);

    uint8_t* yPlane = nv12;
    uint8_t* uvPlane = nv12 + width * height;

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int idx = (y * width + x) * 4;
            uint8_t B = bgra[idx + 0];
            uint8_t G = bgra[idx + 1];
            uint8_t R = bgra[idx + 2];
            yPlane[y * width + x] = (uint8_t)((std::min)(255, (int)(0.257f * R + 0.504f * G + 0.098f * B + 16)));
            if ((y % 2 == 0) && (x % 2 == 0)) {
                int uvIdx = (y / 2) * width + (x / 2) * 2;
                uvPlane[uvIdx + 0] = (uint8_t)((std::min)(255, (int)(-0.148f * R - 0.291f * G + 0.439f * B + 128)));
                uvPlane[uvIdx + 1] = (uint8_t)((std::min)(255, (int)(0.439f * R - 0.368f * G - 0.071f * B + 128)));
            }
        }
    }
    pNV12Buf->Unlock();
    pNV12Buf->SetCurrentLength(nv12Size);
    pBuffer->Unlock();
    pBuffer->Release();

    IMFSample* pSample = nullptr;
    MFCreateSample(&pSample);
    pSample->AddBuffer(pNV12Buf);
    pNV12Buf->Release();

    LONGLONG duration = 10000000LL / fps;
    pSample->SetSampleTime(timestamp * duration);
    pSample->SetSampleDuration(duration);
    return pSample;
}

bool H264Encoder::Initialize(int width, int height, int fps, int bitrateKbps) {
    m_Width = width;
    m_Height = height;
    m_Fps = fps;

    HRESULT hr = MFStartup(MF_VERSION);
    if (FAILED(hr)) return false;

    // Find H.264 encoder MFT (prefer hardware)
    MFT_REGISTER_TYPE_INFO outputType = { MFMediaType_Video, MFVideoFormat_H264 };
    IMFActivate** ppActivates = nullptr;
    UINT32 count = 0;

    hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
        MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
        nullptr, &outputType, &ppActivates, &count);

    if (FAILED(hr) || count == 0) {
        // Fallback to software encoder
        hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
            MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_ASYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER,
            nullptr, &outputType, &ppActivates, &count);
    }

    if (FAILED(hr) || count == 0) {
        std::fprintf(stderr, "[H264Encoder] No H.264 encoder found\n");
        return false;
    }

    std::fprintf(stderr, "[H264Encoder] Found %u encoder(s), trying each...\n", count);
    for (UINT32 i = 0; i < count; i++) {
        hr = ppActivates[i]->ActivateObject(__uuidof(IMFTransform), (void**)&m_Encoder);
        if (SUCCEEDED(hr)) {
            std::fprintf(stderr, "[H264Encoder] Encoder %u activated successfully\n", i);
            break;
        }
        std::fprintf(stderr, "[H264Encoder] Encoder %u failed: 0x%08lx, trying next\n", i, hr);
        m_Encoder = nullptr;
    }
    for (UINT32 i = 0; i < count; i++) ppActivates[i]->Release();
    CoTaskMemFree(ppActivates);

    if (!m_Encoder) {
        std::fprintf(stderr, "[H264Encoder] All encoders failed\n");
        return false;
    }

    // Set output type (H.264)
    IMFMediaType* pOutputType = nullptr;
    MFCreateMediaType(&pOutputType);
    pOutputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    pOutputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    MFSetAttributeSize(pOutputType, MF_MT_FRAME_SIZE, width, height);
    MFSetAttributeRatio(pOutputType, MF_MT_FRAME_RATE, fps, 1);
    pOutputType->SetUINT32(MF_MT_AVG_BITRATE, bitrateKbps * 1000);
    pOutputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeRatio(pOutputType, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    pOutputType->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Main);

    hr = m_Encoder->SetOutputType(0, pOutputType, 0);
    pOutputType->Release();
    if (FAILED(hr)) {
        std::fprintf(stderr, "[H264Encoder] SetOutputType failed: 0x%08lx\n", hr);
        return false;
    }

    // Set input type (NV12)
    IMFMediaType* pInputType = nullptr;
    MFCreateMediaType(&pInputType);
    pInputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    pInputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    MFSetAttributeSize(pInputType, MF_MT_FRAME_SIZE, width, height);
    MFSetAttributeRatio(pInputType, MF_MT_FRAME_RATE, fps, 1);
    pInputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeRatio(pInputType, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

    hr = m_Encoder->SetInputType(0, pInputType, 0);
    pInputType->Release();
    if (FAILED(hr)) {
        std::fprintf(stderr, "[H264Encoder] SetInputType failed: 0x%08lx\n", hr);
        return false;
    }

    // Try low latency mode
    ICodecAPI* pCodecAPI = nullptr;
    if (SUCCEEDED(m_Encoder->QueryInterface(__uuidof(ICodecAPI), (void**)&pCodecAPI))) {
        VARIANT var;
        var.vt = VT_BOOL;
        var.boolVal = VARIANT_TRUE;
        pCodecAPI->SetValue(&CODECAPI_AVLowLatencyMode, &var);
        pCodecAPI->Release();
    }

    hr = m_Encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    if (FAILED(hr)) {
        std::fprintf(stderr, "[H264Encoder] BeginStreaming failed: 0x%08lx\n", hr);
        return false;
    }

    m_Encoder->GetOutputStreamInfo(0, &m_OutputInfo);
    m_Initialized = true;
    m_FrameIndex = 0;
    std::fprintf(stderr, "[H264Encoder] Initialized: %dx%d @ %dfps, %dkbps\n", width, height, fps, bitrateKbps);
    return true;
}

std::vector<uint8_t> H264Encoder::Encode(const uint8_t* bgraData, int width, int height) {
    std::vector<uint8_t> result;
    if (!m_Initialized || !m_Encoder) return result;

    IMFSample* pInputSample = CreateSampleFromBGRA(bgraData, width, height, m_FrameIndex, m_Fps);
    if (!pInputSample) return result;

    HRESULT hr = m_Encoder->ProcessInput(0, pInputSample, 0);
    pInputSample->Release();
    if (FAILED(hr)) return result;

    // Get output
    IMFMediaBuffer* pOutBuffer = nullptr;
    IMFSample* pOutSample = nullptr;
    bool allocSample = !(m_OutputInfo.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES));

    if (allocSample) {
        MFCreateSample(&pOutSample);
        DWORD outSize = (std::max)((DWORD)m_OutputInfo.cbSize, (DWORD)(width * height));
        MFCreateMemoryBuffer(outSize, &pOutBuffer);
        pOutSample->AddBuffer(pOutBuffer);
        pOutBuffer->Release();
    }

    MFT_OUTPUT_DATA_BUFFER output = {};
    output.pSample = pOutSample;

    DWORD status = 0;
    hr = m_Encoder->ProcessOutput(0, 1, &output, &status);

    if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
        if (pOutSample) pOutSample->Release();
        m_FrameIndex++;
        return result;
    }

    IMFSample* resultSample = output.pSample ? output.pSample : pOutSample;
    if (SUCCEEDED(hr) && resultSample) {
        IMFMediaBuffer* pBuf = nullptr;
        resultSample->ConvertToContiguousBuffer(&pBuf);
        if (pBuf) {
            BYTE* pData = nullptr;
            DWORD len = 0;
            pBuf->Lock(&pData, nullptr, &len);
            result.assign(pData, pData + len);
            pBuf->Unlock();
            pBuf->Release();
        }
    }

    if (output.pSample && output.pSample != pOutSample) output.pSample->Release();
    if (pOutSample) pOutSample->Release();
    if (output.pEvents) output.pEvents->Release();

    m_FrameIndex++;
    return result;
}

void H264Encoder::Shutdown() {
    if (m_Encoder) {
        m_Encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
        m_Encoder->Release();
        m_Encoder = nullptr;
    }
    m_Initialized = false;
}

// ============= DECODER =============

bool H264Decoder::Initialize(int width, int height) {
    m_Width = width;
    m_Height = height;

    HRESULT hr = MFStartup(MF_VERSION);
    if (FAILED(hr)) return false;

    MFT_REGISTER_TYPE_INFO inputType = { MFMediaType_Video, MFVideoFormat_H264 };
    IMFActivate** ppActivates = nullptr;
    UINT32 count = 0;

    hr = MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER,
        MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
        &inputType, nullptr, &ppActivates, &count);

    if (FAILED(hr) || count == 0) {
        hr = MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER,
            MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_ASYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER,
            &inputType, nullptr, &ppActivates, &count);
    }

    if (FAILED(hr) || count == 0) {
        std::fprintf(stderr, "[H264Decoder] No decoder found\n");
        return false;
    }

    hr = ppActivates[0]->ActivateObject(__uuidof(IMFTransform), (void**)&m_Decoder);
    for (UINT32 i = 0; i < count; i++) ppActivates[i]->Release();
    CoTaskMemFree(ppActivates);
    if (FAILED(hr)) return false;

    IMFMediaType* pInType = nullptr;
    MFCreateMediaType(&pInType);
    pInType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    pInType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    MFSetAttributeSize(pInType, MF_MT_FRAME_SIZE, width, height);
    hr = m_Decoder->SetInputType(0, pInType, 0);
    pInType->Release();
    if (FAILED(hr)) {
        std::fprintf(stderr, "[H264Decoder] SetInputType failed: 0x%08lx\n", hr);
        return false;
    }

    // Enumerate output types and pick NV12 or RGB32
    for (DWORD i = 0; ; i++) {
        IMFMediaType* pOutType = nullptr;
        hr = m_Decoder->GetOutputAvailableType(0, i, &pOutType);
        if (FAILED(hr)) break;
        GUID subtype;
        pOutType->GetGUID(MF_MT_SUBTYPE, &subtype);
        if (subtype == MFVideoFormat_NV12 || subtype == MFVideoFormat_RGB32) {
            m_Decoder->SetOutputType(0, pOutType, 0);
            pOutType->Release();
            break;
        }
        pOutType->Release();
    }

    m_Decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    m_Decoder->GetOutputStreamInfo(0, &m_OutputInfo);
    m_Initialized = true;
    std::fprintf(stderr, "[H264Decoder] Initialized: %dx%d\n", width, height);
    return true;
}

bool H264Decoder::Decode(const uint8_t* h264Data, int dataSize, std::vector<uint8_t>& rgbaOut, int& outWidth, int& outHeight) {
    if (!m_Initialized || !m_Decoder || !h264Data || dataSize <= 0) return false;

    IMFMediaBuffer* pInBuf = nullptr;
    MFCreateMemoryBuffer(dataSize, &pInBuf);
    BYTE* pData = nullptr;
    pInBuf->Lock(&pData, nullptr, nullptr);
    memcpy(pData, h264Data, dataSize);
    pInBuf->Unlock();
    pInBuf->SetCurrentLength(dataSize);

    IMFSample* pInSample = nullptr;
    MFCreateSample(&pInSample);
    pInSample->AddBuffer(pInBuf);
    pInBuf->Release();

    HRESULT hr = m_Decoder->ProcessInput(0, pInSample, 0);
    pInSample->Release();
    if (FAILED(hr)) return false;

    bool allocSample = !(m_OutputInfo.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES));
    IMFSample* pOutSample = nullptr;
    if (allocSample) {
        MFCreateSample(&pOutSample);
        IMFMediaBuffer* pBuf = nullptr;
        DWORD sz = (std::max)((DWORD)m_OutputInfo.cbSize, (DWORD)(m_Width * m_Height * 4));
        MFCreateMemoryBuffer(sz, &pBuf);
        pOutSample->AddBuffer(pBuf);
        pBuf->Release();
    }

    MFT_OUTPUT_DATA_BUFFER output = {};
    output.pSample = pOutSample;
    DWORD status = 0;
    hr = m_Decoder->ProcessOutput(0, 1, &output, &status);

    if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
        if (pOutSample) pOutSample->Release();
        return false;
    }

    // Handle format change
    if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
        for (DWORD i = 0; ; i++) {
            IMFMediaType* pType = nullptr;
            if (FAILED(m_Decoder->GetOutputAvailableType(0, i, &pType))) break;
            GUID subtype;
            pType->GetGUID(MF_MT_SUBTYPE, &subtype);
            if (subtype == MFVideoFormat_NV12 || subtype == MFVideoFormat_RGB32) {
                m_Decoder->SetOutputType(0, pType, 0);
                UINT32 w = 0, h = 0;
                MFGetAttributeSize(pType, MF_MT_FRAME_SIZE, &w, &h);
                m_Width = w; m_Height = h;
                pType->Release();
                break;
            }
            pType->Release();
        }
        m_Decoder->GetOutputStreamInfo(0, &m_OutputInfo);
        if (pOutSample) pOutSample->Release();
        return false;
    }

    IMFSample* resultSample = output.pSample ? output.pSample : pOutSample;
    bool decoded = false;
    if (SUCCEEDED(hr) && resultSample) {
        IMFMediaBuffer* pBuf = nullptr;
        resultSample->ConvertToContiguousBuffer(&pBuf);
        if (pBuf) {
            BYTE* raw = nullptr;
            DWORD len = 0;
            pBuf->Lock(&raw, nullptr, &len);

            outWidth = m_Width;
            outHeight = m_Height;

            // Convert NV12 to RGBA
            int ySize = m_Width * m_Height;
            if ((int)len >= ySize * 3 / 2) {
                rgbaOut.resize(m_Width * m_Height * 4);
                const uint8_t* Y = raw;
                const uint8_t* UV = raw + ySize;
                for (int y = 0; y < m_Height; y++) {
                    for (int x = 0; x < m_Width; x++) {
                        int yVal = Y[y * m_Width + x];
                        int uvIdx = (y / 2) * m_Width + (x / 2) * 2;
                        int u = UV[uvIdx] - 128;
                        int v = UV[uvIdx + 1] - 128;
                        int r = (std::min)(255, (std::max)(0, (int)(yVal + 1.402f * v)));
                        int g = (std::min)(255, (std::max)(0, (int)(yVal - 0.344f * u - 0.714f * v)));
                        int b = (std::min)(255, (std::max)(0, (int)(yVal + 1.772f * u)));
                        int dstIdx = (y * m_Width + x) * 4;
                        rgbaOut[dstIdx + 0] = (uint8_t)r;
                        rgbaOut[dstIdx + 1] = (uint8_t)g;
                        rgbaOut[dstIdx + 2] = (uint8_t)b;
                        rgbaOut[dstIdx + 3] = 255;
                    }
                }
                decoded = true;
            } else if ((int)len >= m_Width * m_Height * 4) {
                // RGB32 output
                rgbaOut.resize(m_Width * m_Height * 4);
                for (int i = 0; i < m_Width * m_Height; i++) {
                    rgbaOut[i * 4 + 0] = raw[i * 4 + 2]; // R
                    rgbaOut[i * 4 + 1] = raw[i * 4 + 1]; // G
                    rgbaOut[i * 4 + 2] = raw[i * 4 + 0]; // B
                    rgbaOut[i * 4 + 3] = 255;
                }
                decoded = true;
            }
            pBuf->Unlock();
            pBuf->Release();
        }
    }

    if (output.pSample && output.pSample != pOutSample) output.pSample->Release();
    if (pOutSample) pOutSample->Release();
    if (output.pEvents) output.pEvents->Release();

    return decoded;
}

void H264Decoder::Shutdown() {
    if (m_Decoder) {
        m_Decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
        m_Decoder->Release();
        m_Decoder = nullptr;
    }
    m_Initialized = false;
}

} // namespace TalkMe
