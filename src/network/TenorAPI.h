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

    // Tenor API v2 public key (client-side, not a secret — designed to be embedded in apps)
    // To use your own key: register at https://console.cloud.google.com and enable Tenor API
    static const char* GetApiKey() {
        static const char k[] = { 'A','I','z','a','S','y','D','v','p','0','S','j','U','7','I','B','2','e','E','s','j','M','a','h','T','q','E','r','C','d','Y','N','t','f','e','D','W','R','o','\0' };
        return k;
    }
    static constexpr const char* kBaseUrl = "https://tenor.googleapis.com/v2";

    mutable std::mutex m_ResultsMutex;
    std::vector<GifResult> m_CachedResults;
    std::atomic<bool> m_Searching{ false };
};

} // namespace TalkMe
