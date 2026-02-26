#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <thread>
#include <atomic>
#include <functional>
#include <cstdint>

namespace TalkMe {

struct CachedImage {
    std::vector<uint8_t> data;
    int width = 0;
    int height = 0;
    bool ready = false;
    bool failed = false;
};

class ImageCache {
public:
    static ImageCache& Get() { static ImageCache instance; return instance; }

    void RequestImage(const std::string& url);
    CachedImage* GetImage(const std::string& url);
    bool IsLoading(const std::string& url) const;

private:
    ImageCache() = default;
    std::string HttpDownload(const std::string& url);

    mutable std::mutex m_Mutex;
    std::unordered_map<std::string, CachedImage> m_Cache;
    std::unordered_set<std::string> m_Loading;
};

} // namespace TalkMe
