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
