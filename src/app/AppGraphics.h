#pragma once

#include <windows.h>
#include <d3d11.h>
#include <cstdint>

struct ImDrawData;

namespace TalkMe {

// D3D11 device, swap chain, render target, and ImGui platform/renderer init.
// Owns all DX and ImGui backend state.
// Call Init(HWND) then InitImGui(); call Shutdown() before the window is destroyed.
class AppGraphics {
public:
    AppGraphics()  = default;
    ~AppGraphics();

    AppGraphics(const AppGraphics&)            = delete;
    AppGraphics& operator=(const AppGraphics&) = delete;

    // Create device and swap chain for the given window.
    bool Init(HWND hwnd);

    // Init ImGui context, fonts, style, and Win32/DX11 backends. Call after Init().
    bool InitImGui();

    // Release ImGui backends and D3D resources. Safe to call multiple times.
    void Shutdown();

    void OnResize(uint32_t width, uint32_t height);

    // True if device, swap chain, and RTV are non-null (check before rendering).
    bool IsValid() const;

    // Returns the device-removed reason; FAILED(hr) signals device loss.
    HRESULT GetDeviceRemovedReason() const;

    ID3D11Device*        GetDevice()    const { return m_pd3dDevice; }
    ID3D11DeviceContext* GetContext()   const { return m_pd3dDeviceContext; }
    IDXGISwapChain*      GetSwapChain() const { return m_pSwapChain; }

    // Call once per frame before ImGui::NewFrame().
    void ImGuiNewFrame();

    // Clear back buffer, render ImGui draw data, and Present.
    // Returns false if Present fails (device lost/reset).
    bool ClearAndPresent(const float clearColor[4], ImDrawData* drawData);

private:
    void CreateRenderTarget();
    void CleanupRenderTarget();

    HWND                     m_Hwnd                  = nullptr;
    ID3D11Device*            m_pd3dDevice            = nullptr;
    ID3D11DeviceContext*     m_pd3dDeviceContext      = nullptr;
    IDXGISwapChain*          m_pSwapChain            = nullptr;
    ID3D11RenderTargetView*  m_mainRenderTargetView  = nullptr;
    uint32_t                 m_LastResizeWidth       = 0;
    uint32_t                 m_LastResizeHeight      = 0;
    bool                     m_TearingSupported      = false;
};

} // namespace TalkMe