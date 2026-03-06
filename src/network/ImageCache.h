#pragma once
#include <string>
#include <vector>
#include <list>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <thread>
#include <atomic>
#include <functional>
#include <cstdint>

namespace TalkMe {

/// Single decoded GIF frame: composited RGBA canvas + display duration in ms.
using GifFrame = std::pair<std::vector<uint8_t>, int>;

struct CachedImage {
    std::vector<uint8_t> data;   // first frame (or only frame for static images)
    int width  = 0;
    int height = 0;
    bool ready  = false;
    bool failed = false;
    /// Non-empty → animated. Pixel data only needed until textures are uploaded.
    std::vector<GifFrame> animatedFrames;
};

class ImageCache {
public:
    static ImageCache& Get() { static ImageCache instance; return instance; }

    void RequestImage(const std::string& url);
    /// Call from main thread each frame. Decodes queued GIFs (WIC, no compositing) and inserts into cache.
    void ProcessPendingGifDecodes();
    std::optional<CachedImage> GetImageCopy(const std::string& url) const;
    CachedImage* GetImage(const std::string& url);
    bool IsLoading(const std::string& url) const;

    /// Lightweight query — returns per-frame delays (ms) without copying pixel data.
    /// Returns nullopt if URL is not cached/ready or is not animated.
    struct GifTimings {
        std::vector<int> delaysMs;  // one entry per frame
        int width  = 0;
        int height = 0;
    };
    std::optional<GifTimings> GetGifTimings(const std::string& url) const;

    /// Returns {width, height} if the entry is ready, nullopt otherwise.
    /// Does NOT copy pixel data — safe to call every frame.
    struct ImageDims { int width = 0; int height = 0; };
    std::optional<ImageDims> GetReadyDimensions(const std::string& url) const;

    /// After all frames have been uploaded to the GPU you may call this to
    /// free the (potentially large) per-frame pixel vectors.
    void ReleaseGifPixels(const std::string& url);

    /// Remove one URL from the cache and LRU (e.g. when picker closes to free RAM).
    void RemoveEntry(const std::string& url);

    /// If URL has a GIF file on disk, queue it for re-decode (e.g. after texture eviction). Call from main thread. Returns true if queued.
    bool ScheduleRedecodeFromDisk(const std::string& url);

    /// Set URLs that must not be evicted (e.g. visible chat images). Call from main thread each frame.
    /// Eviction will only remove entries not in this set, so chat GIFs keep rendering when the picker is open.
    void SetProtectedUrls(std::unordered_set<std::string> urls);

private:
    ImageCache() = default;
    std::string HttpDownload(const std::string& url);

    mutable std::mutex m_Mutex;
    std::unordered_map<std::string, CachedImage> m_Cache;
    std::unordered_set<std::string> m_Loading;
    /// GIFs from network: written to disk then URL pushed here. Main thread reads from disk and decodes.
    std::vector<std::pair<std::string, std::string>> m_PendingGifDecodes;
    /// URLs of GIFs to decode from disk cache (no raw bytes in RAM until decode).
    std::vector<std::string> m_PendingGifDecodesFromDisk;

    std::list<std::string> m_LruOrder;
    std::unordered_map<std::string, std::list<std::string>::iterator> m_LruPos;
    std::unordered_set<std::string> m_ProtectedUrls;

    void TouchEntry(const std::string& url);
    void EvictIfNeeded();
    /// Returns path for on-disk GIF cache file (URL → stable filename). Used for read/write.
    std::string GetDiskCachePath(const std::string& url) const;
};

} // namespace TalkMe
