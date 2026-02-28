#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "KlipyGifProvider.h"
#include <windows.h>
#include <winhttp.h>
#include <nlohmann/json.hpp>
#include <cstdio>
#include <algorithm>
#include <cctype>

#pragma comment(lib, "winhttp.lib")

using json = nlohmann::json;

namespace TalkMe {

namespace {

const wchar_t kUserAgent[] = L"TalkMe/1.0 (Windows; GIF picker)";
const char kBaseUrl[] = "https://api.klipy.com/api/v1";
const char kCustomerId[] = "talkme_desktop";

std::string UrlEncode(const std::string& s) {
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
            out += (char)c;
        else if (c == ' ')
            out += "%20";
        else {
            char hex[4];
            std::snprintf(hex, sizeof(hex), "%%%02X", c);
            out += hex;
        }
    }
    return out;
}

} // namespace

KlipyGifProvider::KlipyGifProvider(const std::string& apiKey) : m_ApiKey(apiKey) {}

std::string KlipyGifProvider::HttpGet(const std::string& url) {
    std::string result;
    std::wstring wurl(url.begin(), url.end());
    URL_COMPONENTS uc = {};
    uc.dwStructSize = sizeof(uc);
    wchar_t hostBuf[256] = {}, pathBuf[2048] = {};
    uc.lpszHostName = hostBuf;
    uc.dwHostNameLength = 256;
    uc.lpszUrlPath = pathBuf;
    uc.dwUrlPathLength = 2048;
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) return result;

    HINTERNET hSession = WinHttpOpen(kUserAgent, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, nullptr, nullptr, 0);
    if (!hSession) return result;
    HINTERNET hConnect = WinHttpConnect(hSession, hostBuf, uc.nPort, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return result; }
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", pathBuf, nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
        (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0);
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

std::vector<GifResult> KlipyGifProvider::ParseResponseJson(const std::string& response) {
    std::vector<GifResult> results;
    if (response.empty()) return results;
    try {
        auto j = json::parse(response);
        if (!j.value("result", false) || !j.contains("data") || !j["data"].contains("data"))
            return results;
        const auto& dataNode = j["data"]["data"];
        if (!dataNode.is_array())
            return results;
        for (const auto& item : dataNode) {
            GifResult gif;
            gif.id = item.value("slug", std::to_string(item.value("id", 0)));
            gif.title = item.value("title", "");
            if (!item.contains("file")) continue;
            const auto& file = item["file"];
            // Prefer md (medium) for full GIF, fallback to hd/sm
            auto pickGif = [&file](const char* size) -> std::string {
                if (!file.contains(size) || !file[size].contains("gif")) return {};
                const auto& g = file[size]["gif"];
                return g.contains("url") ? g["url"].get<std::string>() : std::string();
            };
            gif.gifUrl = pickGif("md");
            if (gif.gifUrl.empty()) gif.gifUrl = pickGif("hd");
            if (gif.gifUrl.empty()) gif.gifUrl = pickGif("sm");
            if (gif.gifUrl.empty()) gif.gifUrl = pickGif("xs");
            gif.previewUrl = pickGif("sm");
            if (gif.previewUrl.empty()) gif.previewUrl = pickGif("xs");
            if (gif.previewUrl.empty()) gif.previewUrl = gif.gifUrl;
            if (file.contains("sm") && file["sm"].contains("gif")) {
                if (file["sm"]["gif"].contains("width")) gif.width = file["sm"]["gif"].value("width", 0);
                if (file["sm"]["gif"].contains("height")) gif.height = file["sm"]["gif"].value("height", 0);
            }
            if (gif.gifUrl.empty()) continue;
            results.push_back(gif);
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[KlipyGifProvider] Parse error: %s\n", e.what());
    }
    return results;
}

void KlipyGifProvider::Search(const std::string& query, int limit, int page, ResultCallback onResult) {
    if (m_ApiKey.empty() || m_Searching.load()) return;
    m_Searching.store(true);
    std::string apiKey = m_ApiKey;
    int pageNum = (std::max)(1, page);
    std::thread([this, query, limit, pageNum, apiKey, onResult]() {
        int perPage = (std::max)(8, (std::min)(50, limit));
        std::string url = std::string(kBaseUrl) + "/" + UrlEncode(apiKey) + "/gifs/search?page=" +
            std::to_string(pageNum) + "&per_page=" + std::to_string(perPage) + "&q=" + UrlEncode(query) +
            "&customer_id=" + kCustomerId + "&locale=us";
        std::string response = HttpGet(url);
        std::vector<GifResult> results = ParseResponseJson(response);
        {
            std::lock_guard lock(m_ResultsMutex);
            if (pageNum == 1)
                m_CachedResults = results;
            else {
                m_CachedResults.insert(m_CachedResults.end(), results.begin(), results.end());
            }
        }
        m_Searching.store(false);
        if (onResult) onResult(results);
    }).detach();
}

void KlipyGifProvider::Trending(int limit, int page, ResultCallback onResult) {
    if (m_ApiKey.empty() || m_Searching.load()) return;
    m_Searching.store(true);
    std::string apiKey = m_ApiKey;
    int pageNum = (std::max)(1, page);
    std::thread([this, limit, pageNum, apiKey, onResult]() {
        int perPage = (std::max)(1, (std::min)(50, limit));
        std::string url = std::string(kBaseUrl) + "/" + UrlEncode(apiKey) + "/gifs/trending?page=" +
            std::to_string(pageNum) + "&per_page=" + std::to_string(perPage) +
            "&customer_id=" + kCustomerId + "&locale=us";
        std::string response = HttpGet(url);
        std::vector<GifResult> results = ParseResponseJson(response);
        {
            std::lock_guard lock(m_ResultsMutex);
            if (pageNum == 1)
                m_CachedResults = results;
            else {
                m_CachedResults.insert(m_CachedResults.end(), results.begin(), results.end());
            }
        }
        m_Searching.store(false);
        if (onResult) onResult(results);
    }).detach();
}

std::vector<GifResult> KlipyGifProvider::GetCachedResults() const {
    std::lock_guard lock(m_ResultsMutex);
    return m_CachedResults;
}

} // namespace TalkMe
