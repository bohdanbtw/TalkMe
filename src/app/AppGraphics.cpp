#include "AppGraphics.h"
#include <dxgi1_5.h>
#pragma comment(lib, "dxgi.lib")
#include "../core/ConfigManager.h"
#include "../core/Logger.h"
#include "../ui/Theme.h"
#include "../ui/Styles.h"
#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx11.h>
#include <cstdio>

namespace TalkMe {

namespace {

// Returns true if the DXGI adapter supports variable-refresh / tearing.
static bool QueryTearingSupport() noexcept {
    IDXGIFactory5* factory5 = nullptr;
    if (FAILED(::CreateDXGIFactory1(__uuidof(IDXGIFactory5),
                                    reinterpret_cast<void**>(&factory5))))
        return false;
    BOOL tearing = FALSE;
    HRESULT hr = factory5->CheckFeatureSupport(
        DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof(tearing));
    factory5->Release();
    return SUCCEEDED(hr) && tearing;
}

} // namespace

bool AppGraphics::Init(HWND hwnd) {
    m_Hwnd            = hwnd;
    m_TearingSupported = QueryTearingSupport();

    const UINT scFlags = m_TearingSupported ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0u;

    DXGI_SWAP_CHAIN_DESC sd        = {};
    sd.BufferCount                 = 2;
    sd.BufferDesc.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage                 = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow                = hwnd;
    sd.SampleDesc.Count            = 1;
    sd.Windowed                    = TRUE;
    sd.SwapEffect                  = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.Flags                       = scFlags;

    D3D_FEATURE_LEVEL fl = {};
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION,
        &sd, &m_pSwapChain, &m_pd3dDevice, &fl, &m_pd3dDeviceContext);
    if (FAILED(hr)) return false;

    CreateRenderTarget();

    RECT r = {};
    if (::GetClientRect(m_Hwnd, &r)) {
        m_LastResizeWidth  = static_cast<uint32_t>(r.right  - r.left);
        m_LastResizeHeight = static_cast<uint32_t>(r.bottom - r.top);
    }
    return true;
}

void AppGraphics::CreateRenderTarget() {
    if (!m_pSwapChain || !m_pd3dDevice) return;
    ID3D11Texture2D* pBackBuffer = nullptr;
    if (FAILED(m_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer))) || !pBackBuffer)
        return;
    HRESULT hr = m_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr,
                                                       &m_mainRenderTargetView);
    pBackBuffer->Release();
    if (FAILED(hr)) {
        if (m_mainRenderTargetView) {
            m_mainRenderTargetView->Release();
            m_mainRenderTargetView = nullptr;
        }
    }
}

void AppGraphics::CleanupRenderTarget() {
    if (m_mainRenderTargetView) {
        m_mainRenderTargetView->Release();
        m_mainRenderTargetView = nullptr;
    }
}

void AppGraphics::OnResize(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0)                              return;
    if (width == m_LastResizeWidth && height == m_LastResizeHeight) return;
    m_LastResizeWidth  = width;
    m_LastResizeHeight = height;
    if (!m_pSwapChain || !m_pd3dDevice) return;

    CleanupRenderTarget();
    const UINT scFlags = m_TearingSupported ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0u;
    m_pSwapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, scFlags);
    CreateRenderTarget();
}

bool AppGraphics::InitImGui() {
    if (!m_Hwnd || !m_pd3dDevice || !m_pd3dDeviceContext) return false;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    // Periodically free unused vertex/index buffers (e.g. after 60 s idle). Standard practice.
    io.ConfigMemoryCompactTimer = 60.0f;

    const std::string configDir = ConfigManager::GetConfigDirectory();
    ::CreateDirectoryA(configDir.c_str(), nullptr);

    static std::string s_iniPath;
    s_iniPath       = configDir + "\\imgui.ini";
    io.IniFilename  = s_iniPath.c_str();

    // Font atlas is built once here (and by the backend when creating the font texture).
    // Rebuild only on font/size change if you add settings for that; do not rebuild every frame.
    static constexpr const char* kFontPaths[] = {
        "c:\\Windows\\Fonts\\segoeui.ttf",
        "c:\\Windows\\Fonts\\Arial.ttf",
    };
    bool fontLoaded = false;
    for (const char* path : kFontPaths) {
        if (io.Fonts->AddFontFromFileTTF(path, 20.0f)) { fontLoaded = true; break; }
    }
    if (!fontLoaded) io.Fonts->AddFontDefault();

    UI::ApplyAppStyle();
    ImGui_ImplWin32_Init(m_Hwnd);
    ImGui_ImplDX11_Init(m_pd3dDevice, m_pd3dDeviceContext);
    return true;
}

void AppGraphics::Shutdown() {
    if (ImGui::GetCurrentContext()) {
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
    }
    CleanupRenderTarget();
    if (m_pSwapChain)         { m_pSwapChain->Release();         m_pSwapChain         = nullptr; }
    if (m_pd3dDeviceContext)  { m_pd3dDeviceContext->ClearState();
                                m_pd3dDeviceContext->Release();  m_pd3dDeviceContext  = nullptr; }
    if (m_pd3dDevice)         { m_pd3dDevice->Release();         m_pd3dDevice         = nullptr; }
    m_Hwnd = nullptr;
}

bool AppGraphics::IsValid() const {
    return m_pd3dDevice && m_pSwapChain && m_mainRenderTargetView;
}

HRESULT AppGraphics::GetDeviceRemovedReason() const {
    return m_pd3dDevice ? m_pd3dDevice->GetDeviceRemovedReason() : E_FAIL;
}

void AppGraphics::ImGuiNewFrame() {
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
}

bool AppGraphics::ClearAndPresent(const float clearColor[4], ImDrawData* drawData) {
    if (!m_pd3dDeviceContext || !m_mainRenderTargetView) return false;

    // Set viewport to match the back buffer so the UI scales with the window during resize
    // (without this, the pipeline can use a stale viewport and the image stretches instead of reflowing).
    D3D11_VIEWPORT vp = {};
    vp.TopLeftX = 0;
    vp.TopLeftY = 0;
    vp.Width  = static_cast<float>(m_LastResizeWidth);
    vp.Height = static_cast<float>(m_LastResizeHeight);
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    m_pd3dDeviceContext->RSSetViewports(1, &vp);

    m_pd3dDeviceContext->OMSetRenderTargets(1, &m_mainRenderTargetView, nullptr);
    m_pd3dDeviceContext->ClearRenderTargetView(m_mainRenderTargetView, clearColor);
    if (drawData) ImGui_ImplDX11_RenderDrawData(drawData);
    if (!m_pSwapChain) return false;

    // syncInterval = 0: present immediately (no vsync).
    // DXGI_PRESENT_ALLOW_TEARING: required for variable-refresh-rate monitors
    // when the swap chain was created with DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING.
    const UINT presentFlags = m_TearingSupported ? DXGI_PRESENT_ALLOW_TEARING : 0u;
    HRESULT hr = m_pSwapChain->Present(0, presentFlags);

    if (hr == DXGI_ERROR_DEVICE_REMOVED) { LOG_ERROR("Device removed during Present()"); return false; }
    if (hr == DXGI_ERROR_DEVICE_RESET)   { LOG_ERROR("Device reset during Present()");   return false; }
    if (FAILED(hr)) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "Present() failed: 0x%08lX", static_cast<unsigned long>(hr));
        LOG_ERROR(buf);
        return false;
    }
    return true;
}

AppGraphics::~AppGraphics() {
    Shutdown();
}

} // namespace TalkMe