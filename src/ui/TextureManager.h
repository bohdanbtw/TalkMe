#pragma once
#include <d3d11.h>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <vector>
#include <list>
#include <cstdint>
#include <mutex>
#include <atomic>
#include "../app/ResourceLimits.h"
#include "../network/ImageCache.h"

namespace TalkMe {

class TextureManager {
public:
    static TextureManager& Get() { static TextureManager instance; return instance; }

    void SetDevice(ID3D11Device* device) { m_Device = device; }

    ID3D11ShaderResourceView* LoadFromBMP(const std::string& id, const uint8_t* bmpData, int dataSize);
    /// If dataSizeBytes > 0, returns nullptr unless dataSizeBytes >= width*height*4 (avoids reading past buffer).
    ID3D11ShaderResourceView* LoadFromRGBA(const std::string& id, const uint8_t* rgba, int width, int height, bool flipY = false, size_t dataSizeBytes = 0);
    /// Uploads to an existing dynamic texture when possible; otherwise recreates as dynamic.
    ID3D11ShaderResourceView* UpsertDynamicFromRGBA(const std::string& id, const uint8_t* rgba, int width, int height, size_t dataSizeBytes = 0);
    /// Uploads BGRA data using DXGI_FORMAT_B8G8R8A8_UNORM.
    ID3D11ShaderResourceView* UpsertDynamicFromBGRA(const std::string& id, const uint8_t* bgra, int width, int height, size_t dataSizeBytes = 0);
    ID3D11ShaderResourceView* LoadFromMemory(const std::string& id, const uint8_t* data, int dataSize, int* outW = nullptr, int* outH = nullptr);
    ID3D11ShaderResourceView* GetTexture(const std::string& id) const;
    void RemoveTexture(const std::string& id);
    void Clear();

    /// Upload all GIF frames at once. Subsequent calls for the same id are no-ops. Returns false on failure.
    bool LoadGifFrames(const std::string& id,
                       const std::vector<GifFrame>& frames,
                       int width, int height);
    /// Returns the SRV for the given frame, or nullptr if not loaded / out of range.
    ID3D11ShaderResourceView* GetGifFrameSRV(const std::string& id, int frameIndex) const;
    /// 0 if never loaded via LoadGifFrames.
    int GetGifFrameCount(const std::string& id) const;

    int GetWidth(const std::string& id) const;
    int GetHeight(const std::string& id) const;

    /// Advance the internal frame counter and run GIF LRU eviction. Call once per frame before rendering.
    void TickFrame();
    /// Current eviction generation. GifAnimState compares against this.
    uint32_t EvictionGeneration() const { return m_EvictionGen.load(std::memory_order_relaxed); }
    /// True if any GIF frame set is currently loaded (for adaptive frame rate).
    bool HasActiveGifSets() const;

    /// Remove all textures whose id starts with prefix and is not in keepIds.
    /// Used to evict off-screen chat/picker images. Bumps eviction gen per removed id.
    void EvictTexturesWithPrefixExcept(const std::string& prefix,
                                       const std::unordered_set<std::string>& keepIds);

    /// Debug: approximate GIF memory usage (chat sets, picker sets, total frames, VRAM bytes).
    struct GifDebugStats {
        int chatSets = 0;
        int pickerSets = 0;
        int totalFrames = 0;
        size_t approxVramBytes = 0;
    };
    GifDebugStats GetGifDebugStats() const;

private:
    TextureManager() = default;
    ~TextureManager() { Clear(); }

    struct TextureEntry {
        ID3D11ShaderResourceView* srv = nullptr;
        ID3D11Texture2D* texture = nullptr;
        int width = 0, height = 0;
        bool dynamic = false;
        DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM;
    };

    ID3D11Device* m_Device = nullptr;
    std::unordered_map<std::string, TextureEntry> m_Textures;
    std::unordered_map<std::string, std::vector<TextureEntry>> m_GifFrames;
    mutable std::mutex m_Mutex;

    uint64_t m_Frame = 0;
    std::list<std::string> m_StaticLru;
    std::unordered_map<std::string, std::list<std::string>::iterator> m_StaticLruPos;
    std::list<std::string> m_GifLru;
    std::unordered_map<std::string, std::list<std::string>::iterator> m_GifLruPos;
    std::atomic<uint32_t> m_EvictionGen{ 0 };

    // COM objects are sometimes still referenced by ImGui draw lists until the end of the frame.
    // We defer Release() calls to the next TickFrame() to avoid freeing SRVs mid-frame.
    std::vector<IUnknown*> m_DeferredReleases;

    void TouchStatic(const std::string& id);
    void TouchGif(const std::string& id);
    void EvictStaticIfNeeded();
    void EvictGifIfNeeded();
    void ReleaseStaticEntry(const std::string& id);
    void ReleaseGifEntry(const std::string& id);

    TextureEntry CreateTexture(const uint8_t* rgba, int width, int height, bool dynamic);
    TextureEntry CreateTextureWithFormat(const uint8_t* data, int width, int height, bool dynamic, DXGI_FORMAT format);

    void DeferRelease(IUnknown* ptr);
};

} // namespace TalkMe
