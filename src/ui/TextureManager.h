#pragma once
#include <d3d11.h>
#include <unordered_map>
#include <string>
#include <vector>
#include <cstdint>
#include <mutex>

namespace TalkMe {

class TextureManager {
public:
    static TextureManager& Get() { static TextureManager instance; return instance; }

    void SetDevice(ID3D11Device* device) { m_Device = device; }

    ID3D11ShaderResourceView* LoadFromBMP(const std::string& id, const uint8_t* bmpData, int dataSize);
    ID3D11ShaderResourceView* LoadFromRGBA(const std::string& id, const uint8_t* rgba, int width, int height);
    ID3D11ShaderResourceView* GetTexture(const std::string& id);
    void RemoveTexture(const std::string& id);
    void Clear();

    int GetWidth(const std::string& id) const;
    int GetHeight(const std::string& id) const;

private:
    TextureManager() = default;
    ~TextureManager() { Clear(); }

    struct TextureEntry {
        ID3D11ShaderResourceView* srv = nullptr;
        ID3D11Texture2D* texture = nullptr;
        int width = 0, height = 0;
    };

    ID3D11Device* m_Device = nullptr;
    std::unordered_map<std::string, TextureEntry> m_Textures;
    std::mutex m_Mutex;

    TextureEntry CreateTexture(const uint8_t* rgba, int width, int height);
};

} // namespace TalkMe
