#define STB_IMAGE_IMPLEMENTATION
#include "TextureManager.h"
#include <stb_image.h>
#include <cstring>
#include <algorithm>

namespace TalkMe {

using namespace Limits;

// ── LRU helpers ─────────────────────────────────────────────────────────────

void TextureManager::DeferRelease(IUnknown* ptr) {
    if (ptr) m_DeferredReleases.push_back(ptr);
}

void TextureManager::TouchStatic(const std::string& id) {
    auto it = m_StaticLruPos.find(id);
    if (it != m_StaticLruPos.end()) {
        m_StaticLru.splice(m_StaticLru.end(), m_StaticLru, it->second);
    } else {
        m_StaticLru.push_back(id);
        m_StaticLruPos[id] = std::prev(m_StaticLru.end());
    }
}

void TextureManager::TouchGif(const std::string& id) {
    auto it = m_GifLruPos.find(id);
    if (it != m_GifLruPos.end()) {
        m_GifLru.splice(m_GifLru.end(), m_GifLru, it->second);
    } else {
        m_GifLru.push_back(id);
        m_GifLruPos[id] = std::prev(m_GifLru.end());
    }
}

void TextureManager::ReleaseStaticEntry(const std::string& id) {
    auto it = m_Textures.find(id);
    if (it != m_Textures.end()) {
        DeferRelease(it->second.srv);
        DeferRelease(it->second.texture);
        m_Textures.erase(it);
    }
    auto lp = m_StaticLruPos.find(id);
    if (lp != m_StaticLruPos.end()) {
        m_StaticLru.erase(lp->second);
        m_StaticLruPos.erase(lp);
    }
}

void TextureManager::ReleaseGifEntry(const std::string& id) {
    auto it = m_GifFrames.find(id);
    if (it != m_GifFrames.end()) {
        for (auto& e : it->second) {
            DeferRelease(e.srv);
            DeferRelease(e.texture);
        }
        m_GifFrames.erase(it);
    }
    auto lp = m_GifLruPos.find(id);
    if (lp != m_GifLruPos.end()) {
        m_GifLru.erase(lp->second);
        m_GifLruPos.erase(lp);
    }
}

static bool IsChatStaticId(const std::string& id) {
    if (id.size() >= 4 && id.compare(0, 4, "att_") == 0) return true;
    if (id.size() >= 4 && id.compare(0, 4, "img_") == 0) return true;
    return false;
}

void TextureManager::EvictStaticIfNeeded() {
    const size_t maxAttempts = m_StaticLru.size();
    size_t attempts = 0;
    while ((int)m_Textures.size() > kMaxStaticTextures && !m_StaticLru.empty() && attempts < maxAttempts) {
        ++attempts;
        const std::string victim = m_StaticLru.front();
        // Chat images (att_/img_) are evicted only by EvictTexturesWithPrefixExcept when off-screen.
        if (IsChatStaticId(victim)) {
            m_StaticLru.splice(m_StaticLru.end(), m_StaticLru, m_StaticLru.begin());
            continue;
        }
        ReleaseStaticEntry(victim);
        m_EvictionGen.fetch_add(1, std::memory_order_relaxed);
        attempts = 0;
    }
}

static bool IsPickerGifId(const std::string& id) {
    constexpr const char* p = "gif_";
    return id.size() >= 4 && id.compare(0, 4, p) == 0;
}
static bool IsChatGifId(const std::string& id) {
    if (id.size() >= 4 && id.compare(0, 4, "att_") == 0) return true;
    if (id.size() >= 4 && id.compare(0, 4, "img_") == 0) return true;
    if (id.size() >= 3 && id.compare(0, 3, "yt_") == 0) return true;
    return false;
}

void TextureManager::EvictGifIfNeeded() {

    int pickerCount = 0, chatCount = 0;
    for (const auto& [id, _] : m_GifFrames) {
        if (IsPickerGifId(id)) ++pickerCount;
        else if (IsChatGifId(id)) ++chatCount;
    }

    const size_t maxAttempts = m_GifLru.size();
    size_t attempts = 0;
    while ((pickerCount > kMaxPickerGifSetsAnimating || chatCount > kMaxChatGifSetsAnimating) && !m_GifLru.empty() && attempts < maxAttempts) {
        ++attempts;
        const std::string victim = m_GifLru.front();
        const bool victimPicker = IsPickerGifId(victim);
        const bool victimChat = IsChatGifId(victim);
        bool evict = (victimPicker && pickerCount > kMaxPickerGifSetsAnimating) ||
                     (victimChat && chatCount > kMaxChatGifSetsAnimating);
        if (!evict) {
            m_GifLru.splice(m_GifLru.end(), m_GifLru, m_GifLru.begin());
            continue;
        }
        ReleaseGifEntry(victim);
        m_EvictionGen.fetch_add(1, std::memory_order_relaxed);
        if (victimPicker) --pickerCount;
        else if (victimChat) --chatCount;
        attempts = 0;
    }

    // Global cap: if total still over kMaxGifSets, evict by LRU regardless of category
    while ((int)m_GifFrames.size() > kMaxGifSets && !m_GifLru.empty()) {
        const std::string victim = m_GifLru.front();
        ReleaseGifEntry(victim);
        m_EvictionGen.fetch_add(1, std::memory_order_relaxed);
    }
}

void TextureManager::TickFrame() {
    std::vector<IUnknown*> toRelease;
    {
        std::lock_guard lock(m_Mutex);
        toRelease.swap(m_DeferredReleases);
        EvictStaticIfNeeded();
        EvictGifIfNeeded();
        if (!m_DeferredReleases.empty()) {
            toRelease.insert(toRelease.end(), m_DeferredReleases.begin(), m_DeferredReleases.end());
            m_DeferredReleases.clear();
        }
        ++m_Frame;
    }
    for (IUnknown* p : toRelease) {
        if (p) p->Release();
    }
}

bool TextureManager::HasActiveGifSets() const {
    std::lock_guard lock(m_Mutex);
    return !m_GifFrames.empty();
}

TextureManager::GifDebugStats TextureManager::GetGifDebugStats() const {
    std::lock_guard lock(m_Mutex);
    GifDebugStats s;
    for (const auto& [id, entries] : m_GifFrames) {
        if (IsPickerGifId(id)) ++s.pickerSets;
        else if (IsChatGifId(id)) ++s.chatSets;
        s.totalFrames += (int)entries.size();
        if (!entries.empty() && entries[0].width > 0 && entries[0].height > 0)
            s.approxVramBytes += (size_t)entries[0].width * (size_t)entries[0].height * 4u * entries.size();
    }
    return s;
}

void TextureManager::EvictTexturesWithPrefixExcept(const std::string& prefix,
                                                   const std::unordered_set<std::string>& keepIds) {
    std::lock_guard lock(m_Mutex);
    std::vector<std::string> toRemove;
    const size_t pl = prefix.size();
    for (const auto& [id, _] : m_Textures)
        if (id.size() >= pl && id.compare(0, pl, prefix) == 0 && keepIds.count(id) == 0)
            toRemove.push_back(id);
    for (const auto& [id, _] : m_GifFrames)
        if (id.size() >= pl && id.compare(0, pl, prefix) == 0 && keepIds.count(id) == 0)
            toRemove.push_back(id);
    for (const std::string& id : toRemove) {
        ReleaseStaticEntry(id);
        ReleaseGifEntry(id);
        m_EvictionGen.fetch_add(1, std::memory_order_relaxed);
    }
}

TextureManager::TextureEntry TextureManager::CreateTexture(const uint8_t* rgba, int width, int height, bool dynamic) {
    TextureEntry entry;
    entry.width = width;
    entry.height = height;
    entry.dynamic = dynamic;

    if (!m_Device || !rgba || width <= 0 || height <= 0) return entry;
    if (width > kMaxTextureDimension || height > kMaxTextureDimension) return entry;

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = dynamic ? D3D11_USAGE_DEFAULT : D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.CPUAccessFlags = 0;

    D3D11_SUBRESOURCE_DATA initData = {};
    initData.pSysMem = rgba;
    initData.SysMemPitch = width * 4;
    initData.SysMemSlicePitch = 0;

    if (FAILED(m_Device->CreateTexture2D(&desc, &initData, &entry.texture))) return entry;

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = desc.Format;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;

    if (FAILED(m_Device->CreateShaderResourceView(entry.texture, &srvDesc, &entry.srv))) {
        entry.texture->Release();
        entry.texture = nullptr;
        return entry;
    }

    return entry;
}

ID3D11ShaderResourceView* TextureManager::LoadFromRGBA(const std::string& id, const uint8_t* rgba, int width, int height, bool flipY, size_t dataSizeBytes) {
    std::lock_guard lock(m_Mutex);
    if (width <= 0 || height <= 0 || width > kMaxTextureDimension || height > kMaxTextureDimension || !rgba)
        return nullptr;
    const size_t requiredBytes = static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
    if (dataSizeBytes < requiredBytes) return nullptr;  // must have at least width*height*4 bytes to avoid reading past buffer

    ReleaseStaticEntry(id);

    std::vector<uint8_t> buffer(requiredBytes);
    const uint8_t* src = rgba;

    if (flipY) {
        int rowBytes = width * 4;
        for (int y = height - 1; y >= 0; --y)
            std::memcpy(buffer.data() + (height - 1 - y) * rowBytes, rgba + y * rowBytes, rowBytes);
        src = buffer.data();
    } else {
        std::memcpy(buffer.data(), rgba, requiredBytes);
        src = buffer.data();
    }

    auto entry = CreateTexture(src, width, height, false);
    if (!entry.srv) return nullptr;  // Do not store failed entries so we can retry when device is valid
    m_Textures[id] = entry;
    TouchStatic(id);
    return entry.srv;
}

ID3D11ShaderResourceView* TextureManager::UpsertDynamicFromRGBA(const std::string& id, const uint8_t* rgba, int width, int height, size_t dataSizeBytes) {
    std::lock_guard lock(m_Mutex);
    if (width <= 0 || height <= 0 || width > kMaxTextureDimension || height > kMaxTextureDimension || !rgba)
        return nullptr;
    const size_t requiredBytes = static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
    if (dataSizeBytes < requiredBytes) return nullptr;
    if (!m_Device) return nullptr;

    auto it = m_Textures.find(id);
    if (it != m_Textures.end() &&
        it->second.texture &&
        it->second.srv &&
        it->second.dynamic &&
        it->second.width == width &&
        it->second.height == height) {
        ID3D11DeviceContext* ctx = nullptr;
        m_Device->GetImmediateContext(&ctx);
        if (!ctx) return nullptr;
        ctx->UpdateSubresource(it->second.texture, 0, nullptr, rgba, width * 4, 0);
        ctx->Release();
        TouchStatic(id);
        return it->second.srv;
    }

    ReleaseStaticEntry(id);
    auto entry = CreateTexture(rgba, width, height, true);
    if (!entry.srv) return nullptr;
    m_Textures[id] = entry;
    TouchStatic(id);
    return entry.srv;
}

ID3D11ShaderResourceView* TextureManager::LoadFromBMP(const std::string& id, const uint8_t* bmpData, int dataSize) {
    if (dataSize < 54 || bmpData[0] != 'B' || bmpData[1] != 'M') return nullptr;

    int offset = *(int*)&bmpData[10];
    int width = *(int*)&bmpData[18];
    int height = *(int*)&bmpData[22];
    short bpp = *(short*)&bmpData[28];
    bool topDown = (height < 0);
    if (height < 0) height = -height;
    if (width <= 0 || height <= 0 || width > 8192 || height > 8192) return nullptr;

    std::vector<uint8_t> rgba(width * height * 4);

    if (bpp == 24) {
        int rowBytes = ((width * 3 + 3) & ~3);
        for (int y = 0; y < height; y++) {
            int srcY = topDown ? y : (height - 1 - y);
            const uint8_t* row = bmpData + offset + srcY * rowBytes;
            for (int x = 0; x < width; x++) {
                int dstIdx = (y * width + x) * 4;
                rgba[dstIdx + 0] = row[x * 3 + 2]; // R
                rgba[dstIdx + 1] = row[x * 3 + 1]; // G
                rgba[dstIdx + 2] = row[x * 3 + 0]; // B
                rgba[dstIdx + 3] = 255;             // A
            }
        }
    } else if (bpp == 32) {
        int rowBytes = width * 4;
        for (int y = 0; y < height; y++) {
            int srcY = topDown ? y : (height - 1 - y);
            const uint8_t* row = bmpData + offset + srcY * rowBytes;
            for (int x = 0; x < width; x++) {
                int dstIdx = (y * width + x) * 4;
                rgba[dstIdx + 0] = row[x * 4 + 2];
                rgba[dstIdx + 1] = row[x * 4 + 1];
                rgba[dstIdx + 2] = row[x * 4 + 0];
                rgba[dstIdx + 3] = row[x * 4 + 3];
            }
        }
    } else {
        return nullptr;
    }

    return LoadFromRGBA(id, rgba.data(), width, height, false, static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
}

ID3D11ShaderResourceView* TextureManager::GetTexture(const std::string& id) const {
    std::lock_guard lock(m_Mutex);
    auto it = m_Textures.find(id);
    if (it == m_Textures.end()) return nullptr;
    const_cast<TextureManager*>(this)->TouchStatic(id);
    return it->second.srv;
}

void TextureManager::RemoveTexture(const std::string& id) {
    std::lock_guard lock(m_Mutex);
    ReleaseStaticEntry(id);
    ReleaseGifEntry(id);
}

void TextureManager::Clear() {
    std::vector<IUnknown*> toRelease;
    {
        std::lock_guard lock(m_Mutex);
        toRelease.swap(m_DeferredReleases);
        for (auto& [_, entry] : m_Textures) {
            if (entry.srv) toRelease.push_back(entry.srv);
            if (entry.texture) toRelease.push_back(entry.texture);
        }
        for (auto& [_, entries] : m_GifFrames) {
            for (auto& entry : entries) {
                if (entry.srv) toRelease.push_back(entry.srv);
                if (entry.texture) toRelease.push_back(entry.texture);
            }
        }
        m_Textures.clear();
        m_StaticLru.clear();
        m_StaticLruPos.clear();
        m_GifFrames.clear();
        m_GifLru.clear();
        m_GifLruPos.clear();
    }
    for (IUnknown* p : toRelease) {
        if (p) p->Release();
    }
}

bool TextureManager::LoadGifFrames(const std::string& id,
                                   const std::vector<GifFrame>& frames,
                                   int width, int height) {
    std::lock_guard lock(m_Mutex);
    if (width <= 0 || height <= 0 || width > kMaxTextureDimension || height > kMaxTextureDimension)
        return false;
    if (frames.empty()) return false;

    if (m_GifFrames.count(id)) {
        TouchGif(id);
        return true;
    }

    const size_t requiredBytes = (size_t)width * (size_t)height * 4;
    const size_t frameCap = frames.size() <= (size_t)kMaxGifFramesPerSet
        ? frames.size() : (size_t)kMaxGifFramesPerSet;
    std::vector<TextureEntry> entries;
    entries.reserve(frameCap);

    for (size_t i = 0; i < frameCap; i++) {
        const auto& [pixels, delayMs] = frames[i];
        (void)delayMs;
        if (pixels.size() < requiredBytes) {
            for (auto& e : entries) { if (e.srv) e.srv->Release(); if (e.texture) e.texture->Release(); }
            return false;
        }
        auto entry = CreateTexture(pixels.data(), width, height, false);
        if (!entry.srv) {
            for (auto& e : entries) { if (e.srv) e.srv->Release(); if (e.texture) e.texture->Release(); }
            if (entry.srv) entry.srv->Release();
            if (entry.texture) entry.texture->Release();
            return false;
        }
        entries.push_back(entry);
    }

    m_GifFrames[id] = std::move(entries);
    TouchGif(id);
    return true;
}

ID3D11ShaderResourceView*
TextureManager::GetGifFrameSRV(const std::string& id, int frameIndex) const {
    std::lock_guard lock(m_Mutex);
    auto it = m_GifFrames.find(id);
    if (it == m_GifFrames.end()) return nullptr;
    if (frameIndex < 0 || frameIndex >= (int)it->second.size()) return nullptr;
    const_cast<TextureManager*>(this)->TouchGif(id);
    return it->second[frameIndex].srv;
}

int TextureManager::GetGifFrameCount(const std::string& id) const {
    std::lock_guard lock(m_Mutex);
    auto it = m_GifFrames.find(id);
    return (it != m_GifFrames.end()) ? (int)it->second.size() : 0;
}

ID3D11ShaderResourceView* TextureManager::LoadFromMemory(const std::string& id, const uint8_t* data, int dataSize, int* outW, int* outH) {
    if (!data || dataSize <= 0) return nullptr;
    int w = 0, h = 0, channels = 0;
    unsigned char* rgba = stbi_load_from_memory(data, dataSize, &w, &h, &channels, 4);
    if (!rgba) return nullptr;
    if (outW) *outW = w;
    if (outH) *outH = h;
    auto* srv = LoadFromRGBA(id, rgba, w, h, false, static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
    stbi_image_free(rgba);
    return srv;
}

int TextureManager::GetWidth(const std::string& id) const {
    std::lock_guard lock(m_Mutex);
    auto it = m_Textures.find(id);
    return (it != m_Textures.end()) ? it->second.width : 0;
}

int TextureManager::GetHeight(const std::string& id) const {
    std::lock_guard lock(m_Mutex);
    auto it = m_Textures.find(id);
    return (it != m_Textures.end()) ? it->second.height : 0;
}

} // namespace TalkMe
