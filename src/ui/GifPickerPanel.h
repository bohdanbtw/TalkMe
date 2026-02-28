#pragma once
#include <string>
#include <vector>
#include <functional>
#include <atomic>
#include <memory>
#include "../network/KlipyGifProvider.h"

namespace TalkMe::UI {

// ---------------------------------------------------------------------------
//  GifPickerPanel
//
//  Drop-in, stateful GIF picker that renders a polished panel docked to the
//  right edge of the chat window.  Pass the result of MakeRenderCallback()
//  to RenderChannelView's renderGifPanel argument (or call from Emotions tab).
//
//  Usage:
//      GifPickerPanel panel(apiKey);
//      renderGifPanel = panel.MakeRenderCallback(
//          [](const std::string& url, const std::string& title) { /* send */ });
// ---------------------------------------------------------------------------
class GifPickerPanel {
public:
    using OnGifSelected = std::function<void(const std::string& gifUrl,
                                             const std::string& title)>;

    explicit GifPickerPanel(const std::string& apiKey);
    ~GifPickerPanel() = default;

    std::function<void(float w, float h)> MakeRenderCallback(OnGifSelected onSelected);
    void OnClosed();
    bool HasApiKey() const { return m_Provider.HasApiKey(); }

private:
    void Render(float w, float h);
    void TriggerSearch(const std::string& query, int page);
    void TriggerTrending(int page);
    void MaybeLoadMore();

    TalkMe::KlipyGifProvider m_Provider;
    OnGifSelected             m_OnSelected;
    mutable std::mutex        m_PendingMutex;

    char   m_SearchBuf[256]  = {};
    char   m_LastQuery[256]  = {};
    int    m_CurrentPage     = 1;
    bool   m_IsSearchMode    = false;
    bool   m_HasMore         = true;
    bool   m_LoadingMore     = false;
    bool   m_InitialLoad     = false;
    double m_SearchDebounce  = 0.0;

    std::vector<TalkMe::GifResult> m_DisplayedResults;
    std::vector<TalkMe::GifResult> m_PendingAppend;
    std::atomic<bool>              m_PendingReady{ false };
    bool                           m_PendingIsReset{ false };

    static constexpr float kCellW    = 148.0f;
    static constexpr float kCellH    = 110.0f;
    static constexpr float kColGap   = 12.0f;
    static constexpr float kRowGap   = 12.0f;
    static constexpr int   kPerPage  = 50;
    static constexpr int   kColumns  = 2;
};

} // namespace TalkMe::UI
