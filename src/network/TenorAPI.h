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

class TenorAPI {
public:
    using ResultCallback = std::function<void(const std::vector<GifResult>& results)>;

    TenorAPI() = default;
    ~TenorAPI() = default;

    void Search(const std::string& query, int limit, ResultCallback onResult);
    void Trending(int limit, ResultCallback onResult);

    std::vector<GifResult> GetCachedResults() const;
    bool IsSearching() const { return m_Searching.load(); }

private:
    std::string HttpGet(const std::string& url);

    static constexpr const char* kApiKey = "AIzaSyDvp0SjU7IB2eEsjMahTqErCdYNtfeDWRo";
    static constexpr const char* kBaseUrl = "https://tenor.googleapis.com/v2";

    mutable std::mutex m_ResultsMutex;
    std::vector<GifResult> m_CachedResults;
    std::atomic<bool> m_Searching{ false };
};

} // namespace TalkMe
