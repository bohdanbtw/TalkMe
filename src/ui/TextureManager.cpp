#define STB_IMAGE_IMPLEMENTATION
#include "TextureManager.h"
#include <stb_image.h>
#include <cstring>
#include <algorithm>

namespace TalkMe {

TextureManager::TextureEntry TextureManager::CreateTexture(const uint8_t* rgba, int width, int height) {
    TextureEntry entry;
    entry.width = width;
    entry.height = height;

    if (!m_Device || !rgba || width <= 0 || height <= 0) return entry;

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA initData = {};
    initData.pSysMem = rgba;
    initData.SysMemPitch = width * 4;

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

ID3D11ShaderResourceView* TextureManager::LoadFromRGBA(const std::string& id, const uint8_t* rgba, int width, int height) {
    std::lock_guard lock(m_Mutex);
    auto it = m_Textures.find(id);
    if (it != m_Textures.end()) {
        if (it->second.srv) it->second.srv->Release();
        if (it->second.texture) it->second.texture->Release();
    }

    auto entry = CreateTexture(rgba, width, height);
    m_Textures[id] = entry;
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

    return LoadFromRGBA(id, rgba.data(), width, height);
}

ID3D11ShaderResourceView* TextureManager::GetTexture(const std::string& id) {
    std::lock_guard lock(m_Mutex);
    auto it = m_Textures.find(id);
    return (it != m_Textures.end()) ? it->second.srv : nullptr;
}

void TextureManager::RemoveTexture(const std::string& id) {
    std::lock_guard lock(m_Mutex);
    auto it = m_Textures.find(id);
    if (it != m_Textures.end()) {
        if (it->second.srv) it->second.srv->Release();
        if (it->second.texture) it->second.texture->Release();
        m_Textures.erase(it);
    }
}

void TextureManager::Clear() {
    std::lock_guard lock(m_Mutex);
    for (auto& [_, entry] : m_Textures) {
        if (entry.srv) entry.srv->Release();
        if (entry.texture) entry.texture->Release();
    }
    m_Textures.clear();
}

ID3D11ShaderResourceView* TextureManager::LoadFromMemory(const std::string& id, const uint8_t* data, int dataSize, int* outW, int* outH) {
    if (!data || dataSize <= 0) return nullptr;
    int w = 0, h = 0, channels = 0;
    unsigned char* rgba = stbi_load_from_memory(data, dataSize, &w, &h, &channels, 4);
    if (!rgba) return nullptr;
    if (outW) *outW = w;
    if (outH) *outH = h;
    auto* srv = LoadFromRGBA(id, rgba, w, h);
    stbi_image_free(rgba);
    return srv;
}

int TextureManager::GetWidth(const std::string& id) const {
    auto it = m_Textures.find(id);
    return (it != m_Textures.end()) ? it->second.width : 0;
}

int TextureManager::GetHeight(const std::string& id) const {
    auto it = m_Textures.find(id);
    return (it != m_Textures.end()) ? it->second.height : 0;
}

} // namespace TalkMe
