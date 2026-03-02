#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "ImageCache.h"
#include "../app/ResourceLimits.h"
#include "../core/ConfigManager.h"
#include <windows.h>
#include <fstream>
#include <sstream>
#include <winhttp.h>
#include <wincodec.h>
#include <propidl.h>
#include <stb_image.h>
#include <cstdio>
#include <algorithm>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "windowscodecs.lib")

namespace TalkMe {

namespace {
    constexpr int kMaxRedirects = 5;
    constexpr size_t kMaxDownloadBytes = 10 * 1024 * 1024;

    std::string ReadResponseBody(HINTERNET hReq) {
        std::string result;
        DWORD avail = 0;
        while (WinHttpQueryDataAvailable(hReq, &avail) && avail > 0) {
            std::vector<char> buf(avail);
            DWORD read = 0;
            WinHttpReadData(hReq, buf.data(), avail, &read);
            result.append(buf.data(), read);
            if (result.size() > kMaxDownloadBytes) break;
        }
        return result;
    }

    int GetStatusCode(HINTERNET hReq) {
        DWORD statusCode = 0;
        DWORD size = sizeof(statusCode);
        if (WinHttpQueryHeaders(hReq, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &size, WINHTTP_NO_HEADER_INDEX))
            return static_cast<int>(statusCode);
        return 0;
    }

    std::string GetLocationHeader(HINTERNET hReq) {
        wchar_t buf[2048] = {};
        DWORD size = sizeof(buf);
        if (!WinHttpQueryHeaders(hReq, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX, buf, &size, WINHTTP_NO_HEADER_INDEX))
            return {};
        std::string out;
        for (int i = 0; buf[i]; ++i)
            out += static_cast<char>(buf[i] <= 127 ? buf[i] : '?');
        return out;
    }

    std::string ResolveRedirectUrl(const std::string& baseUrl, const std::string& location) {
        if (location.empty()) return {};
        if (location.find("http://") == 0 || location.find("https://") == 0)
            return location;
        std::wstring wbase(baseUrl.begin(), baseUrl.end());
        URL_COMPONENTS uc = {};
        uc.dwStructSize = sizeof(uc);
        wchar_t host[256] = {}, path[2048] = {};
        uc.lpszHostName = host; uc.dwHostNameLength = 256;
        uc.lpszUrlPath = path; uc.dwUrlPathLength = 2048;
        if (!WinHttpCrackUrl(wbase.c_str(), 0, 0, &uc)) return location;
        std::string scheme = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? "https://" : "http://";
        std::string hostStr;
        for (int i = 0; host[i]; ++i) hostStr += static_cast<char>(host[i] <= 127 ? host[i] : '?');
        std::string pathStr;
        if (location[0] == '/') {
            pathStr = location;
        } else {
            std::string basePath;
            for (int i = 0; path[i]; ++i) basePath += static_cast<char>(path[i] <= 127 ? path[i] : '?');
            size_t lastSlash = basePath.rfind('/');
            pathStr = (lastSlash != std::string::npos) ? basePath.substr(0, lastSlash + 1) + location : "/" + location;
        }
        int port = uc.nPort;
        if ((uc.nScheme == INTERNET_SCHEME_HTTPS && port == 443) || (uc.nScheme == INTERNET_SCHEME_HTTP && port == 80))
            return scheme + hostStr + pathStr;
        return scheme + hostStr + ":" + std::to_string(port) + pathStr;
    }

    bool IsGifMagic(const void* data, size_t size) {
        if (size < 6) return false;
        const char* p = static_cast<const char*>(data);
        return (p[0] == 'G' && p[1] == 'I' && p[2] == 'F' && p[3] == '8' && (p[4] == '7' || p[4] == '9') && p[5] == 'a');
    }

    using namespace TalkMe::Limits;

    /// Box-filter resize RGBA image to fit within maxDim on longest edge. Returns new buffer and dimensions.
    std::pair<std::vector<uint8_t>, std::pair<int, int>>
    ResizeRgba(const uint8_t* rgba, int w, int h, int maxDim) {
        if (w <= 0 || h <= 0 || maxDim <= 0) return { {}, { 0, 0 } };
        int scaleW = w, scaleH = h;
        if (w > maxDim || h > maxDim) {
            if (w >= h) {
                scaleW = maxDim;
                scaleH = (int)((double)h * maxDim / (double)w);
            } else {
                scaleH = maxDim;
                scaleW = (int)((double)w * maxDim / (double)h);
            }
            if (scaleW < 1) scaleW = 1;
            if (scaleH < 1) scaleH = 1;
        }
        std::vector<uint8_t> out((size_t)scaleW * (size_t)scaleH * 4);
        for (int y = 0; y < scaleH; y++) {
            for (int x = 0; x < scaleW; x++) {
                int srcX = (int)((double)x * (double)w / (double)scaleW);
                int srcY = (int)((double)y * (double)h / (double)scaleH);
                if (srcX >= w) srcX = w - 1;
                if (srcY >= h) srcY = h - 1;
                size_t dstIdx = ((size_t)y * (size_t)scaleW + (size_t)x) * 4;
                size_t srcIdx = ((size_t)srcY * (size_t)w + (size_t)srcX) * 4;
                out[dstIdx + 0] = rgba[srcIdx + 0];
                out[dstIdx + 1] = rgba[srcIdx + 1];
                out[dstIdx + 2] = rgba[srcIdx + 2];
                out[dstIdx + 3] = rgba[srcIdx + 3];
            }
        }
        return { std::move(out), { scaleW, scaleH } };
    }

    static int ReadMetaUI2(IWICMetadataQueryReader* r, const wchar_t* path, int defaultVal) {
        if (!r) return defaultVal;
        PROPVARIANT pv = {};
        if (SUCCEEDED(r->GetMetadataByName(path, &pv)) && pv.vt == VT_UI2) {
            int v = (int)pv.uiVal;
            PropVariantClear(&pv);
            return v;
        }
        PropVariantClear(&pv);
        return defaultVal;
    }

    static int ReadMetaUI1(IWICMetadataQueryReader* r, const wchar_t* path, int defaultVal) {
        if (!r) return defaultVal;
        PROPVARIANT pv = {};
        if (SUCCEEDED(r->GetMetadataByName(path, &pv)) && pv.vt == VT_UI1) {
            int v = (int)pv.bVal;
            PropVariantClear(&pv);
            return v;
        }
        PropVariantClear(&pv);
        return defaultVal;
    }

    static bool ReadMetaBool(IWICMetadataQueryReader* r, const wchar_t* path) {
        if (!r) return false;
        PROPVARIANT pv = {};
        bool v = false;
        if (SUCCEEDED(r->GetMetadataByName(path, &pv)))
            v = (pv.vt == VT_BOOL && pv.boolVal != VARIANT_FALSE)
                || (pv.vt == VT_UI1 && pv.bVal != 0);
        PropVariantClear(&pv);
        return v;
    }

    /// Decode animated GIF with proper canvas compositing and disposal handling.
    std::tuple<std::vector<GifFrame>, int, int>
    DecodeAnimatedGif(const void* data, size_t size) {
        std::vector<GifFrame> frames;
        int width = 0, height = 0;
        if (!data || size == 0) return { frames, 0, 0 };

        HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (hr != S_OK && hr != S_FALSE) return { frames, 0, 0 };
        const bool weInitCom = (hr == S_OK);

        IWICImagingFactory* factory = nullptr;
        hr = CoCreateInstance(CLSID_WICImagingFactory2, nullptr,
                              CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
        if (FAILED(hr))
            hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                  CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
        if (FAILED(hr) || !factory) {
            if (weInitCom) CoUninitialize();
            return { frames, 0, 0 };
        }

        HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, size);
        if (!hMem) { factory->Release(); if (weInitCom) CoUninitialize(); return { frames, 0, 0 }; }
        void* pMem = GlobalLock(hMem);
        if (!pMem) { GlobalFree(hMem); factory->Release(); if (weInitCom) CoUninitialize(); return { frames, 0, 0 }; }
        memcpy(pMem, data, size);
        GlobalUnlock(hMem);

        IStream* stream = nullptr;
        hr = CreateStreamOnHGlobal(hMem, TRUE, &stream);
        if (FAILED(hr) || !stream) {
            GlobalFree(hMem); factory->Release();
            if (weInitCom) CoUninitialize();
            return { frames, 0, 0 };
        }

        IWICBitmapDecoder* decoder = nullptr;
        hr = factory->CreateDecoderFromStream(stream, nullptr,
                                              WICDecodeMetadataCacheOnLoad, &decoder);
        stream->Release();
        if (FAILED(hr) || !decoder) {
            factory->Release();
            if (weInitCom) CoUninitialize();
            return { frames, 0, 0 };
        }

        {
            IWICMetadataQueryReader* contMeta = nullptr;
            if (SUCCEEDED(decoder->GetMetadataQueryReader(&contMeta)) && contMeta) {
                width  = ReadMetaUI2(contMeta, L"/logscrdesc/Width",  0);
                height = ReadMetaUI2(contMeta, L"/logscrdesc/Height", 0);
                contMeta->Release();
            }
        }

        UINT frameCount = 0;
        hr = decoder->GetFrameCount(&frameCount);
        if (FAILED(hr) || frameCount == 0) {
            decoder->Release(); factory->Release();
            if (weInitCom) CoUninitialize();
            return { frames, 0, 0 };
        }

        if (width <= 0 || height <= 0) {
            IWICBitmapFrameDecode* f0 = nullptr;
            if (SUCCEEDED(decoder->GetFrame(0, &f0)) && f0) {
                UINT fw = 0, fh = 0;
                f0->GetSize(&fw, &fh);
                width  = (int)fw;
                height = (int)fh;
                f0->Release();
            }
        }
        if (width <= 0 || height <= 0) {
            decoder->Release(); factory->Release();
            if (weInitCom) CoUninitialize();
            return { frames, 0, 0 };
        }

        const size_t canvasBytes = (size_t)width * (size_t)height * 4;
        std::vector<uint8_t> canvas(canvasBytes, 0);
        std::vector<uint8_t> prevCanvas(canvasBytes, 0);

        for (UINT fi = 0; fi < frameCount; fi++) {
            IWICBitmapFrameDecode* frameDecode = nullptr;
            hr = decoder->GetFrame(fi, &frameDecode);
            if (FAILED(hr) || !frameDecode) continue;

            UINT fw = 0, fh = 0;
            frameDecode->GetSize(&fw, &fh);
            if (fw == 0 || fh == 0) { frameDecode->Release(); continue; }

            int delayMs    = 100;
            int disposal   = 0;
            int frameLeft  = 0;
            int frameTop   = 0;
            bool hasTransparency   = false;
            int  transparentIndex  = -1;

            IWICMetadataQueryReader* meta = nullptr;
            if (SUCCEEDED(frameDecode->GetMetadataQueryReader(&meta)) && meta) {
                int rawDelay = ReadMetaUI2(meta, L"/grctlext/Delay", 10);
                delayMs = rawDelay * 10;
                if (delayMs < 20) delayMs = 100;
                disposal          = ReadMetaUI1(meta, L"/grctlext/Disposal",            0);
                hasTransparency   = ReadMetaBool(meta, L"/grctlext/TransparencyFlag");
                transparentIndex  = ReadMetaUI1(meta,  L"/grctlext/TransparentColorIndex", -1);
                frameLeft         = ReadMetaUI2(meta, L"/imgdesc/Left", 0);
                frameTop          = ReadMetaUI2(meta, L"/imgdesc/Top",  0);
                meta->Release();
            }

            IWICFormatConverter* conv = nullptr;
            hr = factory->CreateFormatConverter(&conv);
            if (FAILED(hr) || !conv) { frameDecode->Release(); continue; }
            hr = conv->Initialize(frameDecode,
                                  GUID_WICPixelFormat32bppRGBA,
                                  WICBitmapDitherTypeNone,
                                  nullptr, 0.f,
                                  WICBitmapPaletteTypeCustom);
            frameDecode->Release();
            if (FAILED(hr)) { conv->Release(); continue; }

            const UINT fStride  = fw * 4;
            const UINT fBufSize = fStride * fh;
            std::vector<uint8_t> framePixels(fBufSize);
            hr = conv->CopyPixels(nullptr, fStride, fBufSize, framePixels.data());
            conv->Release();
            if (FAILED(hr)) continue;

            if (disposal == 3)
                prevCanvas = canvas;

            const int clampW = std::min((int)fw, width  - frameLeft);
            const int clampH = std::min((int)fh, height - frameTop);
            for (int y = 0; y < clampH; y++) {
                const uint8_t* src = framePixels.data() + (size_t)y * fStride;
                uint8_t*       dst = canvas.data()
                                   + (size_t)(frameTop + y) * (size_t)width * 4
                                   + (size_t)frameLeft * 4;
                for (int x = 0; x < clampW; x++, src += 4, dst += 4) {
                    if (src[3] == 0) continue;
                    dst[0] = src[0];
                    dst[1] = src[1];
                    dst[2] = src[2];
                    dst[3] = src[3];
                }
            }

            if ((int)frames.size() < kMaxGifFramesPerSet)
                frames.push_back({ canvas, delayMs });

            switch (disposal) {
            case 2:
                for (int y = 0; y < clampH; y++) {
                    uint8_t* dst = canvas.data()
                                 + (size_t)(frameTop + y) * (size_t)width * 4
                                 + (size_t)frameLeft * 4;
                    memset(dst, 0, (size_t)clampW * 4);
                }
                break;
            case 3:
                canvas = prevCanvas;
                break;
            default:
                break;
            }
        }

        decoder->Release();
        factory->Release();

        // Downscale if over limit to save RAM/VRAM
        if ((std::max)(width, height) > kMaxGifDimension && !frames.empty()) {
            std::vector<GifFrame> resized;
            resized.reserve(frames.size());
            int newW = width, newH = height;
            for (size_t i = 0; i < frames.size(); i++) {
                auto [outBuf, dims] = ResizeRgba(frames[i].first.data(), width, height, kMaxGifDimension);
                if (i == 0) { newW = dims.first; newH = dims.second; }
                if (!outBuf.empty())
                    resized.push_back({ std::move(outBuf), frames[i].second });
            }
            if (!resized.empty()) {
                frames = std::move(resized);
                width = newW;
                height = newH;
            }
        }

        if (weInitCom) CoUninitialize();
        return { std::move(frames), width, height };
    }
}

static std::string UrlToCacheFilename(const std::string& url) {
    size_t h = std::hash<std::string>{}(url);
    static const char hex[] = "0123456789abcdef";
    std::string out(16, '0');
    for (int i = 0; i < 16; ++i) out[15 - i] = hex[(h >> (i * 4)) & 0xF];
    return out + ".gif";
}

std::string ImageCache::GetDiskCachePath(const std::string& url) const {
    return ConfigManager::GetConfigDirectory() + "\\gif_cache\\" + UrlToCacheFilename(url);
}

void ImageCache::TouchEntry(const std::string& url) {
    auto it = m_LruPos.find(url);
    if (it != m_LruPos.end()) {
        m_LruOrder.splice(m_LruOrder.end(), m_LruOrder, it->second);
    } else {
        m_LruOrder.push_back(url);
        m_LruPos[url] = std::prev(m_LruOrder.end());
    }
}

void ImageCache::EvictIfNeeded() {
    using namespace TalkMe::Limits;
    const size_t maxAttempts = m_LruOrder.size();
    size_t attempts = 0;

    while ((int)m_Cache.size() > kMaxImageCacheEntries
           && !m_LruOrder.empty()
           && attempts < maxAttempts) {
        ++attempts;
        std::string victim = m_LruOrder.front();

        bool busy = (m_Loading.count(victim) > 0);
        if (!busy) {
            for (const auto& [u, _] : m_PendingGifDecodes)
                if (u == victim) { busy = true; break; }
            for (const std::string& u : m_PendingGifDecodesFromDisk)
                if (u == victim) { busy = true; break; }
        }
        if (busy) {
            m_LruOrder.splice(m_LruOrder.end(), m_LruOrder, m_LruOrder.begin());
            continue;
        }
        if (m_ProtectedUrls.count(victim) > 0) {
            m_LruOrder.splice(m_LruOrder.end(), m_LruOrder, m_LruOrder.begin());
            continue;
        }

        m_LruPos.erase(victim);
        m_LruOrder.pop_front();
        m_Cache.erase(victim);
        attempts = 0;
    }
}

std::string ImageCache::HttpDownload(const std::string& url) {
    std::string currentUrl = url;
    for (int redirects = 0; redirects < kMaxRedirects; ++redirects) {
        std::wstring wurl(currentUrl.begin(), currentUrl.end());
        URL_COMPONENTS uc = {};
        uc.dwStructSize = sizeof(uc);
        wchar_t host[256] = {}, path[2048] = {};
        uc.lpszHostName = host; uc.dwHostNameLength = 256;
        uc.lpszUrlPath = path; uc.dwUrlPathLength = 2048;
        if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) return {};

        HINTERNET hSession = WinHttpOpen(L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) TalkMe/1.0",
            WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, nullptr, nullptr, 0);
        if (!hSession) return {};
        HINTERNET hConnect = WinHttpConnect(hSession, host, uc.nPort, 0);
        if (!hConnect) { WinHttpCloseHandle(hSession); return {}; }
        HINTERNET hReq = WinHttpOpenRequest(hConnect, L"GET", path, nullptr,
            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
            (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0);
        if (!hReq) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return {}; }

        // Many image CDNs (Tenor, GIPHY, etc.) expect browser-like headers to serve images
        WinHttpAddRequestHeaders(hReq, L"Accept: image/*, */*\r\n", (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);
        if (currentUrl.find("tenor.com") != std::string::npos || currentUrl.find("giphy.com") != std::string::npos)
            WinHttpAddRequestHeaders(hReq, L"Referer: https://tenor.com/\r\n", (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);

        std::string result;
        if (WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0) &&
            WinHttpReceiveResponse(hReq, nullptr)) {
            int status = GetStatusCode(hReq);
            if (status == 301 || status == 302) {
                std::string location = GetLocationHeader(hReq);
                WinHttpCloseHandle(hReq);
                WinHttpCloseHandle(hConnect);
                WinHttpCloseHandle(hSession);
                if (location.empty()) return {};
                currentUrl = ResolveRedirectUrl(currentUrl, location);
                continue;
            }
            if (status >= 200 && status < 300)
                result = ReadResponseBody(hReq);
        }
        WinHttpCloseHandle(hReq);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return result;
    }
    return {};
}

void ImageCache::RequestImage(const std::string& url) {
    {
        std::lock_guard lock(m_Mutex);
        if (m_Cache.count(url) || m_Loading.count(url)) return;
        m_Loading.insert(url);
    }

    std::thread([this, url]() {
        std::string data;

        // Handle data: URLs (base64 encoded) — no disk cache
        if (url.find("data:image/") == 0) {
            size_t commaPos = url.find(',');
            if (commaPos != std::string::npos) {
                std::string b64 = url.substr(commaPos + 1);
                static const std::string chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
                int val = 0, bits = -8;
                for (char c : b64) {
                    size_t p = chars.find(c);
                    if (p == std::string::npos) continue;
                    val = (val << 6) + (int)p;
                    bits += 6;
                    if (bits >= 0) { data += (char)((val >> bits) & 0xFF); bits -= 8; }
                }
            }
        } else {
            // Check disk cache first (GIFs only) to avoid re-download and keep RAM low
            std::string diskPath = GetDiskCachePath(url);
            if (GetFileAttributesA(diskPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
                std::lock_guard lock(m_Mutex);
                m_PendingGifDecodesFromDisk.push_back(url);
                return;
            }
            data = HttpDownload(url);
        }

        CachedImage img;
        bool gifQueued = false;
        if (!data.empty()) {
            if (IsGifMagic(data.data(), data.size())) {
                // Write GIF to disk, then queue URL for decode from disk so we don't hold bytes in RAM
                std::string diskPath = GetDiskCachePath(url);
                std::string cacheDir = ConfigManager::GetConfigDirectory() + "\\gif_cache";
                CreateDirectoryA(cacheDir.c_str(), nullptr);
                std::ofstream of(diskPath, std::ios::binary | std::ios::trunc);
                if (of.is_open())
                    of.write(data.data(), (std::streamsize)data.size());
                data.clear();
                data.shrink_to_fit();
                std::lock_guard lock(m_Mutex);
                m_PendingGifDecodesFromDisk.push_back(url);
                gifQueued = true;
            }
            if (!gifQueued) {
                int w = 0, h = 0, ch = 0;
                unsigned char* pixels = stbi_load_from_memory(
                    reinterpret_cast<const unsigned char*>(data.data()),
                    (int)data.size(), &w, &h, &ch, 4);
                if (pixels) {
                    img.data.assign(pixels, pixels + (size_t)w * (size_t)h * 4);
                    img.width = w;
                    img.height = h;
                    img.ready = true;
                    stbi_image_free(pixels);
                } else {
                    img.failed = true;
                }
                std::lock_guard lock(m_Mutex);
                m_Cache[url] = std::move(img);
                m_Loading.erase(url);
                TouchEntry(url);
                EvictIfNeeded();
            }
        } else {
            img.failed = true;
            std::lock_guard lock(m_Mutex);
            m_Cache[url] = std::move(img);
            m_Loading.erase(url);
            TouchEntry(url);
            EvictIfNeeded();
        }
    }).detach();
}

void ImageCache::ProcessPendingGifDecodes() {
    std::vector<std::string> fromDisk;
    std::vector<std::pair<std::string, std::string>> pending;
    {
        std::lock_guard lock(m_Mutex);
        fromDisk = std::move(m_PendingGifDecodesFromDisk);
        m_PendingGifDecodesFromDisk.clear();
        pending = std::move(m_PendingGifDecodes);
        m_PendingGifDecodes.clear();
    }

    // Decode GIFs from disk (no long-lived raw bytes in RAM)
    for (const std::string& url : fromDisk) {
        std::string data;
        std::string path = GetDiskCachePath(url);
        std::ifstream f(path, std::ios::binary);
        if (f.is_open()) {
            data.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            f.close();
        }
        CachedImage img;
        if (!data.empty()) {
            auto [gifFrames, w, h] = DecodeAnimatedGif(data.data(), data.size());
            data.clear();
            data.shrink_to_fit();
            if (!gifFrames.empty() && w > 0 && h > 0) {
                img.animatedFrames = std::move(gifFrames);
                img.data = img.animatedFrames[0].first;
                img.width = w;
                img.height = h;
                img.ready = true;
            } else {
                // Fallback: try static decode (re-read from disk if needed)
                std::ifstream f2(path, std::ios::binary);
                if (f2.is_open()) {
                    std::string buf((std::istreambuf_iterator<char>(f2)), std::istreambuf_iterator<char>());
                    int sw = 0, sh = 0, ch = 0;
                    unsigned char* pixels = stbi_load_from_memory(
                        reinterpret_cast<const unsigned char*>(buf.data()), (int)buf.size(), &sw, &sh, &ch, 4);
                    if (pixels) {
                        img.data.assign(pixels, pixels + (size_t)sw * (size_t)sh * 4);
                        img.width = sw;
                        img.height = sh;
                        img.ready = true;
                        stbi_image_free(pixels);
                    } else
                        img.failed = true;
                } else
                    img.failed = true;
            }
        } else {
            img.failed = true;
        }
        std::lock_guard lock(m_Mutex);
        m_Cache[url] = std::move(img);
        m_Loading.erase(url);
        TouchEntry(url);
        EvictIfNeeded();
    }

    // Legacy path: GIFs passed in memory (e.g. from tests or data: URLs decoded on main thread)
    for (auto& [url, data] : pending) {
        auto [gifFrames, w, h] = DecodeAnimatedGif(data.data(), data.size());
        CachedImage img;
        if (!gifFrames.empty() && w > 0 && h > 0) {
            img.animatedFrames = std::move(gifFrames);
            img.data = img.animatedFrames[0].first;
            img.width = w;
            img.height = h;
            img.ready = true;
        } else {
            int sw = 0, sh = 0, ch = 0;
            unsigned char* pixels = stbi_load_from_memory(
                reinterpret_cast<const unsigned char*>(data.data()),
                (int)data.size(), &sw, &sh, &ch, 4);
            if (pixels) {
                img.data.assign(pixels, pixels + (size_t)sw * (size_t)sh * 4);
                img.width = sw;
                img.height = sh;
                img.ready = true;
                stbi_image_free(pixels);
            } else {
                img.failed = true;
            }
        }
        std::lock_guard lock(m_Mutex);
        m_Cache[url] = std::move(img);
        m_Loading.erase(url);
        TouchEntry(url);
        EvictIfNeeded();
    }
}

std::optional<CachedImage> ImageCache::GetImageCopy(const std::string& url) const {
    std::lock_guard lock(m_Mutex);
    auto it = m_Cache.find(url);
    if (it == m_Cache.end()) return std::nullopt;
    const_cast<ImageCache*>(this)->TouchEntry(url);
    return it->second;
}

CachedImage* ImageCache::GetImage(const std::string& url) {
    std::lock_guard lock(m_Mutex);
    auto it = m_Cache.find(url);
    if (it == m_Cache.end()) return nullptr;
    TouchEntry(url);
    return &it->second;
}

bool ImageCache::IsLoading(const std::string& url) const {
    std::lock_guard lock(m_Mutex);
    return m_Loading.count(url) > 0;
}

std::optional<ImageCache::ImageDims>
ImageCache::GetReadyDimensions(const std::string& url) const {
    std::lock_guard lock(m_Mutex);
    auto it = m_Cache.find(url);
    if (it == m_Cache.end()) return std::nullopt;
    const auto& img = it->second;
    if (!img.ready || img.width <= 0 || img.height <= 0) return std::nullopt;
    const_cast<ImageCache*>(this)->TouchEntry(url);
    return ImageDims{ img.width, img.height };
}

std::optional<ImageCache::GifTimings>
ImageCache::GetGifTimings(const std::string& url) const {
    std::lock_guard lock(m_Mutex);
    auto it = m_Cache.find(url);
    if (it == m_Cache.end()) return std::nullopt;
    const CachedImage& img = it->second;
    if (!img.ready || img.animatedFrames.size() < 2) return std::nullopt;
    const_cast<ImageCache*>(this)->TouchEntry(url);

    GifTimings t;
    t.width  = img.width;
    t.height = img.height;
    t.delaysMs.reserve(img.animatedFrames.size());
    for (const auto& f : img.animatedFrames)
        t.delaysMs.push_back(f.second);
    return t;
}

void ImageCache::ReleaseGifPixels(const std::string& url) {
    std::lock_guard lock(m_Mutex);
    auto it = m_Cache.find(url);
    if (it == m_Cache.end()) return;
    for (auto& f : it->second.animatedFrames)
        std::vector<uint8_t>().swap(f.first);
}

void ImageCache::RemoveEntry(const std::string& url) {
    std::lock_guard lock(m_Mutex);
    auto itPos = m_LruPos.find(url);
    if (itPos != m_LruPos.end()) {
        m_LruOrder.erase(itPos->second);
        m_LruPos.erase(itPos);
    }
    m_Cache.erase(url);
    // Disk cache is left in place so re-requesting this URL loads from disk instead of re-downloading
}

void ImageCache::SetProtectedUrls(std::unordered_set<std::string> urls) {
    std::lock_guard lock(m_Mutex);
    m_ProtectedUrls = std::move(urls);
}

} // namespace TalkMe
