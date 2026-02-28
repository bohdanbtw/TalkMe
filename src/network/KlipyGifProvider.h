#pragma once
#include <string>
#include <vector>
#include <functional>
#include <thread>
#include <mutex>
#include <atomic>

namespace TalkMe {

struct GifResult {
    std::string id;
    std::string title;
    std::string gifUrl;
    std::string previewUrl;
    int width = 0;
    int height = 0;
};

// GIF provider using Klipy.com API. Requires API key (from secret/secrets).
class KlipyGifProvider {
public:
    using ResultCallback = std::function<void(const std::vector<GifResult>& results)>;

    explicit KlipyGifProvider(const std::string& apiKey);
    ~KlipyGifProvider() = default;

    void Search(const std::string& query, int limit, int page, ResultCallback onResult);
    void Trending(int limit, int page, ResultCallback onResult);
    std::vector<GifResult> GetCachedResults() const;
    bool IsSearching() const { return m_Searching.load(); }
    bool HasApiKey() const { return !m_ApiKey.empty(); }

private:
    std::string HttpGet(const std::string& url);
    std::vector<GifResult> ParseResponseJson(const std::string& json);

    std::string m_ApiKey;
    mutable std::mutex m_ResultsMutex;
    std::vector<GifResult> m_CachedResults;
    std::atomic<bool> m_Searching{ false };
};

} // namespace TalkMe
