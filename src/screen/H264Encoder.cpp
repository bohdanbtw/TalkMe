#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "H264Encoder.h"
#include <mferror.h>
#include <wmcodecdsp.h>
#include <cstring>
#include <cstdio>
#include <algorithm>
#include <chrono>

namespace TalkMe {

// Include compiled shader bytecode
#include "ColorConversion_bytecode.h"

static inline uint8_t Clamp255(int v) { return (uint8_t)((unsigned)v <= 255 ? v : (v < 0 ? 0 : 255)); }

static IMFSample* CreateSampleFromBGRA(const uint8_t* bgra, int width, int height, int64_t timestamp, int fps) {
    const DWORD nv12Size = width * height * 3 / 2;
    IMFMediaBuffer* pNV12Buf = nullptr;
    MFCreateMemoryBuffer(nv12Size, &pNV12Buf);
    if (!pNV12Buf) return nullptr;
    BYTE* nv12 = nullptr;
    pNV12Buf->Lock(&nv12, nullptr, nullptr);

    uint8_t* yPlane = nv12;
    uint8_t* uvPlane = nv12 + width * height;
    const int stride = width * 4;

    // BT.601 BGRA->NV12 with fixed-point integer arithmetic (8-bit precision shift)
    for (int y = 0; y < height; y++) {
        const uint8_t* row = bgra + y * stride;
        for (int x = 0; x < width; x++) {
            const int B = row[x * 4 + 0];
            const int G = row[x * 4 + 1];
            const int R = row[x * 4 + 2];
            yPlane[y * width + x] = Clamp255((66 * R + 129 * G + 25 * B + 128) >> 8) + 16;
        }
    }
    // UV planes: subsample 2x2 with safe clamping for odd dimensions
    for (int y = 0; y < height; y += 2) {
        const uint8_t* row0 = bgra + y * stride;
        const uint8_t* row1 = (y + 1 < height) ? bgra + (y + 1) * stride : row0;
        uint8_t* uvRow = uvPlane + (y / 2) * width;
        for (int x = 0; x < width; x += 2) {
            const int x1 = (std::min)(x + 1, width - 1);
            const int B = (row0[x*4] + row0[x1*4] + row1[x*4] + row1[x1*4] + 2) >> 2;
            const int G = (row0[x*4+1] + row0[x1*4+1] + row1[x*4+1] + row1[x1*4+1] + 2) >> 2;
            const int R = (row0[x*4+2] + row0[x1*4+2] + row1[x*4+2] + row1[x1*4+2] + 2) >> 2;
            uvRow[x]     = Clamp255(((-38 * R - 74 * G + 112 * B + 128) >> 8) + 128);
            uvRow[x + 1] = Clamp255(((112 * R - 94 * G - 18 * B + 128) >> 8) + 128);
        }
    }
    pNV12Buf->Unlock();
    pNV12Buf->SetCurrentLength(nv12Size);

    IMFSample* pSample = nullptr;
    MFCreateSample(&pSample);
    pSample->AddBuffer(pNV12Buf);
    pNV12Buf->Release();

    const LONGLONG duration = 10'000'000LL / fps;
    pSample->SetSampleTime(timestamp * duration);
    pSample->SetSampleDuration(duration);
    return pSample;
}

// GPU-accelerated BGRA to NV12 conversion (saves 3-5ms per frame!)
bool H264Encoder::InitializeGPUConversion(ID3D11Device* device) {
    if (!device) return false;

    m_D3DDevice = device;
    device->GetImmediateContext(&m_D3DContext);
    if (!m_D3DContext) return false;

    HRESULT hr = S_OK;

    // Create compute shader from compiled bytecode
    hr = device->CreateComputeShader(g_main, sizeof(g_main), nullptr, &m_ColorConversionShader);
    if (FAILED(hr)) {
        std::fprintf(stderr, "[H264Encoder] Failed to create compute shader: 0x%08lx\n", hr);
        return false;
    }

    // Create textures for BGRA input and NV12 output
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = m_Width;
    desc.Height = m_Height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

    // BGRA texture
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    hr = device->CreateTexture2D(&desc, nullptr, &m_BGRATexture);
    if (FAILED(hr)) {
        std::fprintf(stderr, "[H264Encoder] Failed to create BGRA texture\n");
        return false;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;
    hr = device->CreateShaderResourceView(m_BGRATexture, &srvDesc, &m_BGRASRV);
    if (FAILED(hr)) {
        std::fprintf(stderr, "[H264Encoder] Failed to create BGRA SRV\n");
        return false;
    }

    // Y plane texture (same dimensions, R8 format)
    desc.Format = DXGI_FORMAT_R8_UNORM;
    desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    hr = device->CreateTexture2D(&desc, nullptr, &m_YTexture);
    if (FAILED(hr)) {
        std::fprintf(stderr, "[H264Encoder] Failed to create Y texture\n");
        return false;
    }

    D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
    uavDesc.Format = DXGI_FORMAT_R8_UNORM;
    uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
    hr = device->CreateUnorderedAccessView(m_YTexture, &uavDesc, &m_YUAV);
    if (FAILED(hr)) {
        std::fprintf(stderr, "[H264Encoder] Failed to create Y UAV\n");
        return false;
    }

    // UV plane texture (half resolution, RG8 format)
    desc.Width = m_Width / 2;
    desc.Height = m_Height / 2;
    desc.Format = DXGI_FORMAT_R8G8_UNORM;
    hr = device->CreateTexture2D(&desc, nullptr, &m_UVTexture);
    if (FAILED(hr)) {
        std::fprintf(stderr, "[H264Encoder] Failed to create UV texture\n");
        return false;
    }

    uavDesc.Format = DXGI_FORMAT_R8G8_UNORM;
    hr = device->CreateUnorderedAccessView(m_UVTexture, &uavDesc, &m_UVUAV);
    if (FAILED(hr)) {
        std::fprintf(stderr, "[H264Encoder] Failed to create UV UAV\n");
        return false;
    }

    // Create constant buffer for shader parameters
    D3D11_BUFFER_DESC cbDesc = {};
    cbDesc.ByteWidth = sizeof(uint32_t) * 4;  // 2x uint2 (dimensions + padding)
    cbDesc.Usage = D3D11_USAGE_DYNAMIC;
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = device->CreateBuffer(&cbDesc, nullptr, &m_ConversionParamsCB);
    if (FAILED(hr)) {
        std::fprintf(stderr, "[H264Encoder] Failed to create constant buffer\n");
        return false;
    }

    // Pre-create staging textures for readback (avoid per-frame CreateTexture2D)
    D3D11_TEXTURE2D_DESC yStagingDesc = {};
    yStagingDesc.Width = m_Width;
    yStagingDesc.Height = m_Height;
    yStagingDesc.MipLevels = 1;
    yStagingDesc.ArraySize = 1;
    yStagingDesc.Format = DXGI_FORMAT_R8_UNORM;
    yStagingDesc.SampleDesc.Count = 1;
    yStagingDesc.Usage = D3D11_USAGE_STAGING;
    yStagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    hr = device->CreateTexture2D(&yStagingDesc, nullptr, &m_YStaging);
    if (FAILED(hr)) return false;

    D3D11_TEXTURE2D_DESC uvStagingDesc = yStagingDesc;
    uvStagingDesc.Width = m_Width / 2;
    uvStagingDesc.Height = m_Height / 2;
    uvStagingDesc.Format = DXGI_FORMAT_R8G8_UNORM;
    hr = device->CreateTexture2D(&uvStagingDesc, nullptr, &m_UVStaging);
    if (FAILED(hr)) return false;

    std::fprintf(stderr, "[H264Encoder] GPU conversion initialized successfully\n");
    m_GPUConversionReady = true;
    return true;
}

bool H264Encoder::ConvertBGRAviaGPU(const uint8_t* bgraData, int width, int height, std::vector<uint8_t>& outNV12) {
    if (!m_GPUConversionReady || !m_D3DContext) return false;

    // Upload BGRA via UpdateSubresource (DEFAULT texture can't be mapped)
    D3D11_BOX srcBox = { 0, 0, 0, (UINT)width, (UINT)height, 1 };
    m_D3DContext->UpdateSubresource(m_BGRATexture, 0, &srcBox, bgraData, width * 4, 0);

    // Update constant buffer
    D3D11_MAPPED_SUBRESOURCE cbMapped = {};
    if (SUCCEEDED(m_D3DContext->Map(m_ConversionParamsCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &cbMapped))) {
        uint32_t* pCB = (uint32_t*)cbMapped.pData;
        pCB[0] = width;
        pCB[1] = height;
        m_D3DContext->Unmap(m_ConversionParamsCB, 0);
    }

    // Dispatch compute shader
    ID3D11UnorderedAccessView* uavs[] = { m_YUAV, m_UVUAV };
    m_D3DContext->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
    m_D3DContext->CSSetShaderResources(0, 1, &m_BGRASRV);
    m_D3DContext->CSSetConstantBuffers(0, 1, &m_ConversionParamsCB);
    m_D3DContext->CSSetShader(m_ColorConversionShader, nullptr, 0);
    m_D3DContext->Dispatch((width + 15) / 16, (height + 15) / 16, 1);

    ID3D11UnorderedAccessView* nullUAVs[] = { nullptr, nullptr };
    m_D3DContext->CSSetUnorderedAccessViews(0, 2, nullUAVs, nullptr);

    // Read back NV12 using cached staging textures
    const size_t outNV12Size = static_cast<size_t>(width) * static_cast<size_t>(height) * 3 / 2;
    outNV12.resize(outNV12Size);

    // Y plane readback
    m_D3DContext->CopyResource(m_YStaging, m_YTexture);
    D3D11_MAPPED_SUBRESOURCE mappedY = {};
    if (SUCCEEDED(m_D3DContext->Map(m_YStaging, 0, D3D11_MAP_READ, 0, &mappedY))) {
        for (int y = 0; y < height; y++)
            memcpy(outNV12.data() + static_cast<size_t>(y) * width, (uint8_t*)mappedY.pData + static_cast<size_t>(y) * mappedY.RowPitch, width);
        m_D3DContext->Unmap(m_YStaging, 0);
    } else {
        return false;
    }

    // UV plane readback
    m_D3DContext->CopyResource(m_UVStaging, m_UVTexture);
    D3D11_MAPPED_SUBRESOURCE mappedUV = {};
    if (SUCCEEDED(m_D3DContext->Map(m_UVStaging, 0, D3D11_MAP_READ, 0, &mappedUV))) {
        uint8_t* pUV = outNV12.data() + static_cast<size_t>(width) * height;
        for (int y = 0; y < height / 2; y++)
            memcpy(pUV + static_cast<size_t>(y) * width, (uint8_t*)mappedUV.pData + static_cast<size_t>(y) * mappedUV.RowPitch, width);
        m_D3DContext->Unmap(m_UVStaging, 0);
    } else {
        return false;
    }

    return true;
}

void H264Encoder::ShutdownGPUResources() {
    if (m_YStaging) { m_YStaging->Release(); m_YStaging = nullptr; }
    if (m_UVStaging) { m_UVStaging->Release(); m_UVStaging = nullptr; }
    if (m_ColorConversionShader) { m_ColorConversionShader->Release(); m_ColorConversionShader = nullptr; }
    if (m_ConversionParamsCB) { m_ConversionParamsCB->Release(); m_ConversionParamsCB = nullptr; }
    if (m_BGRASRV) { m_BGRASRV->Release(); m_BGRASRV = nullptr; }
    if (m_BGRATexture) { m_BGRATexture->Release(); m_BGRATexture = nullptr; }
    if (m_YUAV) { m_YUAV->Release(); m_YUAV = nullptr; }
    if (m_YTexture) { m_YTexture->Release(); m_YTexture = nullptr; }
    if (m_UVUAV) { m_UVUAV->Release(); m_UVUAV = nullptr; }
    if (m_UVTexture) { m_UVTexture->Release(); m_UVTexture = nullptr; }
    if (m_D3DContext) { m_D3DContext->Release(); m_D3DContext = nullptr; }
    m_GPUConversionReady = false;
}

bool H264Encoder::Initialize(int width, int height, int fps, int bitrateKbps, ID3D11Device* d3dDevice) {
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
    // Baseline profile for minimal encoding latency (no B-frames, no CABAC)
    pOutputType->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Base);

    hr = m_Encoder->SetOutputType(0, pOutputType, 0);
    if (FAILED(hr)) {
        // Fallback to Main profile if Baseline rejected
        pOutputType->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Main);
        hr = m_Encoder->SetOutputType(0, pOutputType, 0);
    }
    pOutputType->Release();
    if (FAILED(hr)) {
        std::fprintf(stderr, "[H264Encoder] SetOutputType failed: 0x%08lx\n", hr);
        return false;
    }

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

    // Real-time encoding hints for minimum latency
    ICodecAPI* pCodecAPI = nullptr;
    if (SUCCEEDED(m_Encoder->QueryInterface(__uuidof(ICodecAPI), (void**)&pCodecAPI))) {
        VARIANT var;
        var.vt = VT_BOOL;
        var.boolVal = VARIANT_TRUE;
        pCodecAPI->SetValue(&CODECAPI_AVLowLatencyMode, &var);
        var.vt = VT_UI4;
        var.ulVal = 0;
        pCodecAPI->SetValue(&CODECAPI_AVEncMPVDefaultBPictureCount, &var);
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

    // Try to initialize GPU conversion (optional, falls back to CPU if fails)
    if (d3dDevice) {
        if (!InitializeGPUConversion(d3dDevice)) {
            std::fprintf(stderr, "[H264Encoder] GPU conversion init failed, falling back to CPU\n");
        } else {
            std::fprintf(stderr, "[H264Encoder] Using GPU-accelerated BGRA→NV12 conversion\n");
        }
    }

    std::fprintf(stderr, "[H264Encoder] Initialized: %dx%d @ %dfps, %dkbps\n", width, height, fps, bitrateKbps);
    return true;
}

std::vector<uint8_t> H264Encoder::Encode(const uint8_t* bgraData, int width, int height) {
    std::vector<uint8_t> result;
    if (!m_Initialized || !m_Encoder) return result;

    // Try GPU conversion if available, fall back to CPU
    IMFSample* pInputSample = nullptr;
    if (m_GPUConversionReady) {
        std::vector<uint8_t> nv12Data;
        if (ConvertBGRAviaGPU(bgraData, width, height, nv12Data) && !nv12Data.empty()) {
            // Create MFSample from GPU-converted NV12
            IMFMediaBuffer* pBuf = nullptr;
            MFCreateMemoryBuffer(static_cast<DWORD>(nv12Data.size()), &pBuf);
            if (pBuf) {
                BYTE* pData = nullptr;
                pBuf->Lock(&pData, nullptr, nullptr);
                memcpy(pData, nv12Data.data(), nv12Data.size());
                pBuf->Unlock();
                pBuf->SetCurrentLength(static_cast<DWORD>(nv12Data.size()));

                MFCreateSample(&pInputSample);
                pInputSample->AddBuffer(pBuf);
                pBuf->Release();

                const LONGLONG duration = 10'000'000LL / m_Fps;
                pInputSample->SetSampleTime(m_FrameIndex * duration);
                pInputSample->SetSampleDuration(duration);
            }
        }
    }

    // Fallback to CPU conversion if GPU failed
    if (!pInputSample) {
        pInputSample = CreateSampleFromBGRA(bgraData, width, height, m_FrameIndex, m_Fps);
    }

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

    // Measure ProcessOutput latency for frame skipping decision (Solution C)
    auto processStart = std::chrono::high_resolution_clock::now();
    hr = m_Encoder->ProcessOutput(0, 1, &output, &status);
    auto processEnd = std::chrono::high_resolution_clock::now();
    m_LastProcessOutputTimeMs = std::chrono::duration<float, std::milli>(processEnd - processStart).count();

    if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
        m_NeedMoreInputCount++;
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
    ShutdownGPUResources();
    if (m_Encoder) {
        m_Encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
        m_Encoder->Release();
        m_Encoder = nullptr;
    }
    m_Initialized = false;
    MFShutdown();
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

            // NV12 to RGBA using fixed-point integer math (BT.601)
            int ySize = m_Width * m_Height;
            if ((int)len >= ySize * 3 / 2) {
                rgbaOut.resize(ySize * 4);
                const uint8_t* Y = raw;
                const uint8_t* UV = raw + ySize;
                uint8_t* dst = rgbaOut.data();
                for (int y = 0; y < m_Height; y++) {
                    const uint8_t* yRow = Y + y * m_Width;
                    const uint8_t* uvRow = UV + (y / 2) * m_Width;
                    uint8_t* dstRow = dst + y * m_Width * 4;
                    for (int x = 0; x < m_Width; x++) {
                        int C = (int)yRow[x] - 16;
                        int uvBase = (x & ~1);
                        int D = (int)uvRow[uvBase] - 128;
                        int E = (int)uvRow[uvBase + 1] - 128;
                        int r = (298 * C + 409 * E + 128) >> 8;
                        int g = (298 * C - 100 * D - 208 * E + 128) >> 8;
                        int b = (298 * C + 516 * D + 128) >> 8;
                        dstRow[x * 4 + 0] = (uint8_t)((unsigned)r <= 255 ? r : (r < 0 ? 0 : 255));
                        dstRow[x * 4 + 1] = (uint8_t)((unsigned)g <= 255 ? g : (g < 0 ? 0 : 255));
                        dstRow[x * 4 + 2] = (uint8_t)((unsigned)b <= 255 ? b : (b < 0 ? 0 : 255));
                        dstRow[x * 4 + 3] = 255;
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
    MFShutdown();
}

} // namespace TalkMe
