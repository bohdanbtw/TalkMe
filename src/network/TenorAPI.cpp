#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "TenorAPI.h"
#include <windows.h>
#include <winhttp.h>
#include <nlohmann/json.hpp>
#include <cstdio>
#include <sstream>

#pragma comment(lib, "winhttp.lib")

using json = nlohmann::json;

namespace TalkMe {

std::string TenorAPI::HttpGet(const std::string& url) {
    std::string result;

    // Parse URL
    std::wstring wurl(url.begin(), url.end());
    URL_COMPONENTS urlComp = {};
    urlComp.dwStructSize = sizeof(urlComp);
    wchar_t hostBuf[256] = {}, pathBuf[2048] = {};
    urlComp.lpszHostName = hostBuf;
    urlComp.dwHostNameLength = 256;
    urlComp.lpszUrlPath = pathBuf;
    urlComp.dwUrlPathLength = 2048;
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &urlComp)) return result;

    HINTERNET hSession = WinHttpOpen(L"TalkMe/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, nullptr, nullptr, 0);
    if (!hSession) return result;

    HINTERNET hConnect = WinHttpConnect(hSession, hostBuf, urlComp.nPort, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return result; }

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", pathBuf,
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
        (urlComp.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0);
    if (!hRequest) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return result; }

    if (WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0) &&
        WinHttpReceiveResponse(hRequest, nullptr)) {
        DWORD bytesAvailable = 0;
        while (WinHttpQueryDataAvailable(hRequest, &bytesAvailable) && bytesAvailable > 0) {
            std::vector<char> buf(bytesAvailable);
            DWORD bytesRead = 0;
            WinHttpReadData(hRequest, buf.data(), bytesAvailable, &bytesRead);
            result.append(buf.data(), bytesRead);
        }
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return result;
}

void TenorAPI::Search(const std::string& query, int limit, ResultCallback onResult) {
    if (m_Searching.load()) return;
    m_Searching.store(true);

    std::thread([this, query, limit, onResult]() {
        // URL-encode the query
        std::string encoded;
        for (char c : query) {
            if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
                encoded += c;
            else {
                char hex[4];
                snprintf(hex, sizeof(hex), "%%%02X", (unsigned char)c);
                encoded += hex;
            }
        }

        std::string url = std::string(kBaseUrl) + "/search?q=" + encoded +
            "&key=" + kApiKey + "&limit=" + std::to_string(limit) +
            "&media_filter=gif,tinygif";

        std::string response = HttpGet(url);
        std::vector<GifResult> results;

        if (!response.empty()) {
            try {
                auto j = json::parse(response);
                if (j.contains("results")) {
                    for (const auto& item : j["results"]) {
                        GifResult gif;
                        gif.id = item.value("id", "");
                        gif.title = item.value("content_description", "");

                        if (item.contains("media_formats")) {
                            auto& media = item["media_formats"];
                            if (media.contains("gif") && media["gif"].contains("url"))
                                gif.gifUrl = media["gif"]["url"];
                            if (media.contains("tinygif") && media["tinygif"].contains("url"))
                                gif.previewUrl = media["tinygif"]["url"];
                            if (media.contains("gif") && media["gif"].contains("dims")) {
                                auto& dims = media["gif"]["dims"];
                                if (dims.size() >= 2) {
                                    gif.width = dims[0];
                                    gif.height = dims[1];
                                }
                            }
                        }

                        if (!gif.gifUrl.empty())
                            results.push_back(gif);
                    }
                }
            }
            catch (const std::exception& e) {
                std::fprintf(stderr, "[TenorAPI] Parse error: %s\n", e.what());
            }
        }

        {
            std::lock_guard lock(m_ResultsMutex);
            m_CachedResults = results;
        }

        m_Searching.store(false);
        if (onResult) onResult(results);
    }).detach();
}

void TenorAPI::Trending(int limit, ResultCallback onResult) {
    if (m_Searching.load()) return;
    m_Searching.store(true);

    std::thread([this, limit, onResult]() {
        std::string url = std::string(kBaseUrl) + "/featured?key=" + kApiKey +
            "&limit=" + std::to_string(limit) + "&media_filter=gif,tinygif";

        std::string response = HttpGet(url);
        std::vector<GifResult> results;

        if (!response.empty()) {
            try {
                auto j = json::parse(response);
                if (j.contains("results")) {
                    for (const auto& item : j["results"]) {
                        GifResult gif;
                        gif.id = item.value("id", "");
                        gif.title = item.value("content_description", "");
                        if (item.contains("media_formats")) {
                            auto& media = item["media_formats"];
                            if (media.contains("gif") && media["gif"].contains("url"))
                                gif.gifUrl = media["gif"]["url"];
                            if (media.contains("tinygif") && media["tinygif"].contains("url"))
                                gif.previewUrl = media["tinygif"]["url"];
                        }
                        if (!gif.gifUrl.empty()) results.push_back(gif);
                    }
                }
            }
            catch (...) {}
        }

        {
            std::lock_guard lock(m_ResultsMutex);
            m_CachedResults = results;
        }
        m_Searching.store(false);
        if (onResult) onResult(results);
    }).detach();
}

std::vector<GifResult> TenorAPI::GetCachedResults() const {
    std::lock_guard lock(m_ResultsMutex);
    return m_CachedResults;
}

} // namespace TalkMe
