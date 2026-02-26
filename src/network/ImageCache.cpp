#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "ImageCache.h"
#include <windows.h>
#include <winhttp.h>
#include <stb_image.h>
#include <cstdio>
#include <algorithm>

#pragma comment(lib, "winhttp.lib")

namespace TalkMe {

std::string ImageCache::HttpDownload(const std::string& url) {
    std::string result;
    std::wstring wurl(url.begin(), url.end());
    URL_COMPONENTS uc = {};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256] = {}, path[2048] = {};
    uc.lpszHostName = host; uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path; uc.dwUrlPathLength = 2048;
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) return result;

    HINTERNET hSession = WinHttpOpen(L"TalkMe/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, nullptr, nullptr, 0);
    if (!hSession) return result;
    HINTERNET hConnect = WinHttpConnect(hSession, host, uc.nPort, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return result; }
    HINTERNET hReq = WinHttpOpenRequest(hConnect, L"GET", path, nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
        (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0);
    if (!hReq) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return result; }

    if (WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0) &&
        WinHttpReceiveResponse(hReq, nullptr)) {
        DWORD avail = 0;
        while (WinHttpQueryDataAvailable(hReq, &avail) && avail > 0) {
            std::vector<char> buf(avail);
            DWORD read = 0;
            WinHttpReadData(hReq, buf.data(), avail, &read);
            result.append(buf.data(), read);
            if (result.size() > 10 * 1024 * 1024) break; // 10MB max
        }
    }
    WinHttpCloseHandle(hReq);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return result;
}

void ImageCache::RequestImage(const std::string& url) {
    {
        std::lock_guard lock(m_Mutex);
        if (m_Cache.count(url) || m_Loading.count(url)) return;
        m_Loading.insert(url);
    }

    std::thread([this, url]() {
        std::string data;

        // Handle data: URLs (base64 encoded)
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
            data = HttpDownload(url);
        }
        CachedImage img;
        if (!data.empty()) {
            int w = 0, h = 0, ch = 0;
            unsigned char* pixels = stbi_load_from_memory(
                reinterpret_cast<const unsigned char*>(data.data()),
                (int)data.size(), &w, &h, &ch, 4);
            if (pixels) {
                img.data.assign(pixels, pixels + w * h * 4);
                img.width = w;
                img.height = h;
                img.ready = true;
                stbi_image_free(pixels);
            } else {
                img.failed = true;
            }
        } else {
            img.failed = true;
        }

        std::lock_guard lock(m_Mutex);
        m_Cache[url] = std::move(img);
        m_Loading.erase(url);
    }).detach();
}

CachedImage* ImageCache::GetImage(const std::string& url) {
    std::lock_guard lock(m_Mutex);
    auto it = m_Cache.find(url);
    return (it != m_Cache.end()) ? &it->second : nullptr;
}

bool ImageCache::IsLoading(const std::string& url) const {
    std::lock_guard lock(m_Mutex);
    return m_Loading.count(url) > 0;
}

} // namespace TalkMe
