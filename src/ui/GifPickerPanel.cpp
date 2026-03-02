#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "GifPickerPanel.h"
#include "TextureManager.h"
#include "../network/ImageCache.h"
#include <imgui.h>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <mutex>

namespace TalkMe::UI {

namespace {

static inline ImU32 ShimmerColor(double time, int itemIndex) {
    float phase = (float)(time * 1.6 + itemIndex * 0.15);
    float alpha = 0.35f + 0.20f * (0.5f + 0.5f * std::sin(phase * 3.14159f));
    return IM_COL32(80, 82, 90, (int)(alpha * 255));
}

static inline ImU32 SkeletonBg() {
    return IM_COL32(55, 57, 63, 255);
}

static float CellHeight(const TalkMe::GifResult& gif, float cellW) {
    if (gif.width > 0 && gif.height > 0) {
        float aspect = (float)gif.height / (float)gif.width;
        float h = cellW * aspect;
        return std::max(70.0f, std::min(h, 220.0f));
    }
    return 110.0f;  // default skeleton height
}

} // namespace

GifPickerPanel::GifPickerPanel(const std::string& apiKey)
    : m_Provider(apiKey)
{}

void GifPickerPanel::RegisterRenderedTexture(const std::string& texId, const std::string& url) {
    auto& tm = TalkMe::TextureManager::Get();
    auto& imgCache = TalkMe::ImageCache::Get();

    m_TexToUrl[texId] = url;

    bool alreadyTracked = false;
    for (const auto& existing : m_RenderedOrder) {
        if (existing == texId) { alreadyTracked = true; break; }
    }
    if (!alreadyTracked) {
        m_RenderedOrder.push_back(texId);
    }

    while ((int)m_RenderedOrder.size() > kMaxActivePickerGifs) {
        const std::string victim = m_RenderedOrder.front();
        m_RenderedOrder.erase(m_RenderedOrder.begin());

        tm.RemoveTexture(victim);
        m_GifStates.erase(victim);

        auto it = m_TexToUrl.find(victim);
        if (it != m_TexToUrl.end()) {
            // Drop picker cache entry so the next time it appears it starts from skeleton
            // and re-downloads/decodes from the network.
            imgCache.RemoveEntry(it->second);
            m_TexToUrl.erase(it);
        }
    }
}

void GifPickerPanel::OnClosed() {
    m_SearchDebounce = 0.0;
    m_InitialLoad = false;
    m_PendingReady.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lk(m_PendingMutex);
        m_PendingAppend.clear();
    }
    auto& tm = TalkMe::TextureManager::Get();
    auto& imgCache = TalkMe::ImageCache::Get();
    // Nuke all picker textures and decoded cache so RAM is freed. Keep m_DisplayedResults
    // so on reopen we show the same grid as skeletons, then they re-load from disk/network.
    for (auto& [texId, _] : m_GifStates)
        tm.RemoveTexture(texId);
    m_GifStates.clear();
    m_RenderedOrder.clear();
    for (const auto& [_, url] : m_TexToUrl)
        imgCache.RemoveEntry(url);
    m_TexToUrl.clear();
    for (const auto& g : m_DisplayedResults) {
        if (!g.previewUrl.empty()) imgCache.RemoveEntry(g.previewUrl);
        if (!g.gifUrl.empty() && g.gifUrl != g.previewUrl) imgCache.RemoveEntry(g.gifUrl);
    }
    // Do NOT clear m_DisplayedResults: reopen shows same grid as skeletons, then re-renders.
}

std::function<void(float, float)>
GifPickerPanel::MakeRenderCallback(OnGifSelected onSelected) {
    m_OnSelected = std::move(onSelected);
    return [this](float w, float h) { Render(w, h); };
}

void GifPickerPanel::TriggerSearch(const std::string& query, int page) {
    m_LoadingMore = true;
    bool isReset  = (page == 1);
    m_Provider.Search(query, kPerPage, page,
        [this, isReset](const std::vector<TalkMe::GifResult>& results) {
            {
                std::lock_guard<std::mutex> lk(m_PendingMutex);
                m_PendingAppend  = results;
                m_PendingIsReset = isReset;
                m_HasMore        = !results.empty();
            }
            m_PendingReady.store(true, std::memory_order_release);
        });
}

void GifPickerPanel::TriggerTrending(int page) {
    m_LoadingMore = true;
    bool isReset  = (page == 1);
    m_Provider.Trending(kPerPage, page,
        [this, isReset](const std::vector<TalkMe::GifResult>& results) {
            {
                std::lock_guard<std::mutex> lk(m_PendingMutex);
                m_PendingAppend  = results;
                m_PendingIsReset = isReset;
                m_HasMore        = !results.empty();
            }
            m_PendingReady.store(true, std::memory_order_release);
        });
}

void GifPickerPanel::MaybeLoadMore() {
    if (m_LoadingMore || !m_HasMore) return;
    m_CurrentPage++;
    if (m_IsSearchMode)
        TriggerSearch(m_LastQuery, m_CurrentPage);
    else
        TriggerTrending(m_CurrentPage);
}

void GifPickerPanel::Render(float panelW, float panelH) {
    if (m_PendingReady.load(std::memory_order_acquire)) {
        m_PendingReady.store(false, std::memory_order_release);
        m_LoadingMore = false;
        std::vector<TalkMe::GifResult> snap;
        bool doReset = false;
        {
            std::lock_guard<std::mutex> lk(m_PendingMutex);
            snap     = std::move(m_PendingAppend);
            doReset  = m_PendingIsReset;
            m_PendingAppend.clear();
        }
        if (doReset) {
            m_DisplayedResults = std::move(snap);
        } else {
            m_DisplayedResults.insert(
                m_DisplayedResults.end(), snap.begin(), snap.end());
        }
    }

    if (!m_InitialLoad) {
        m_InitialLoad   = true;
        m_CurrentPage   = 1;
        m_IsSearchMode  = false;
        TriggerTrending(1);
    }

    const float minPanelSize = 80.0f;
    if (panelW < minPanelSize || panelH < minPanelSize)
        return;

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(kColGap, kRowGap));
    ImGui::Dummy(ImVec2(0, 2));

    // Search at top (tab already shows "GIFs"; no redundant heading)
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.13f, 0.13f, 0.17f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.17f, 0.17f, 0.22f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.17f, 0.17f, 0.22f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 7.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.0f, 7.0f));
    ImGui::SetNextItemWidth(panelW);
    bool searchEdited = ImGui::InputTextWithHint(
        "##gif_search", "Search GIFs...", m_SearchBuf, sizeof(m_SearchBuf));
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(3);

    if (searchEdited) {
        m_SearchDebounce = ImGui::GetTime() + 0.40;
    }
    double now = ImGui::GetTime();
    if (m_SearchDebounce > 0.0 && now >= m_SearchDebounce) {
        m_SearchDebounce = 0.0;
        std::string q(m_SearchBuf);
        bool trending = q.empty();
        bool sameQuery = (q == m_LastQuery && m_IsSearchMode);
        if (!sameQuery) {
            m_CurrentPage   = 1;
            m_HasMore       = true;
            m_IsSearchMode  = !trending;
            strncpy_s(m_LastQuery, q.c_str(), sizeof(m_LastQuery) - 1);
            m_LastQuery[sizeof(m_LastQuery) - 1] = '\0';
            m_DisplayedResults.clear();
            m_GifStates.clear();
            if (trending)
                TriggerTrending(1);
            else
                TriggerSearch(q, 1);
        }
    }

    ImGui::Dummy(ImVec2(0, 4));

    ImDrawList* bgDl = ImGui::GetWindowDrawList();
    ImVec2 sepMin = ImGui::GetCursorScreenPos();
    bgDl->AddLine(
        ImVec2(sepMin.x, sepMin.y),
        ImVec2(sepMin.x + panelW, sepMin.y),
        IM_COL32(60, 62, 70, 200), 1.0f);
    ImGui::Dummy(ImVec2(0, 4));

    float gridH = panelH - ImGui::GetCursorPosY() - 2.0f;
    if (gridH < 80.0f) gridH = 80.0f;

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.085f, 0.085f, 0.11f, 1.0f));
    ImGui::BeginChild("##gif_grid", ImVec2(panelW, gridH), false,
                      ImGuiWindowFlags_HorizontalScrollbar);

    ImDrawList* dl    = ImGui::GetWindowDrawList();
    float       scrollY = ImGui::GetScrollY();
    float       maxScrollY = ImGui::GetScrollMaxY();

    if (!m_LoadingMore && m_HasMore &&
        !m_DisplayedResults.empty() &&
        (maxScrollY - scrollY) < 300.0f) {
        MaybeLoadMore();
    }

    const float colW  = (panelW - kColGap) * 0.5f;
    const float colX[2] = { 0.0f, colW + kColGap };
    float colY[2] = { 0.0f, 0.0f };

    const int   nResults      = (int)m_DisplayedResults.size();
    const int   skeletonCount = (nResults == 0 && (m_LoadingMore || m_Provider.IsSearching()))
                                ? 20 : 0;
    const int   totalCells    = nResults + skeletonCount;

    const float viewTop    = scrollY;
    const float viewBottom = scrollY + gridH;
    const float overscan   = 150.0f; // pixels; cells within this margin keep their textures
    const float viewTopEx  = viewTop - overscan;
    const float viewBotEx  = viewBottom + overscan;

    std::unordered_set<std::string> visibleTexIds;

    for (int ci = 0; ci < totalCells; ci++) {
        int col  = ci % kColumns;
        float cx = colX[col];
        float cy = colY[col];

        bool isSkeleton = (ci >= nResults);
        const TalkMe::GifResult* gif = isSkeleton ? nullptr : &m_DisplayedResults[ci];

        float cellH = isSkeleton ? kCellH : CellHeight(*gif, colW);

        bool inViewStrict = (cy + cellH >= viewTop && cy <= viewBottom);
        bool inViewOverscan = (cy + cellH >= viewTopEx && cy <= viewBotEx);
        if (!inViewStrict) {
            if (inViewOverscan && !isSkeleton && gif && !gif->id.empty())
                visibleTexIds.insert("gif_" + gif->id);
            colY[col] += cellH + kRowGap;
            continue;
        }
        if (!isSkeleton && gif && !gif->id.empty())
            visibleTexIds.insert("gif_" + gif->id);

        ImVec2 cellMin = ImVec2(ImGui::GetWindowPos().x + cx,
                                ImGui::GetWindowPos().y + cy - scrollY);
        ImVec2 cellMax = ImVec2(cellMin.x + colW, cellMin.y + cellH);

        if (isSkeleton) {
            dl->AddRectFilled(cellMin, cellMax, SkeletonBg(), 6.0f);
            dl->AddRectFilled(cellMin, cellMax, ShimmerColor(now, ci), 6.0f);
        } else {
            try {
            const std::string& url = gif->previewUrl.empty() ? gif->gifUrl : gif->previewUrl;
            const bool canLoad = !gif->id.empty() && !url.empty();
            if (!canLoad) {
                dl->AddRectFilled(cellMin, cellMax, SkeletonBg(), 6.0f);
                dl->AddRectFilled(cellMin, cellMax, ShimmerColor(now, ci), 6.0f);
                ImGui::SetCursorPos(ImVec2(cx, cy));
                ImGui::PushID(ci);
                ImGui::InvisibleButton("##gif_btn", ImVec2(colW, cellH));
                ImGui::PopID();
                colY[col] += cellH + kRowGap;
                continue;
            }
            auto& imgCache = TalkMe::ImageCache::Get();
            auto& tm       = TalkMe::TextureManager::Get();
            const std::string texId = "gif_" + gif->id;

            if (!imgCache.GetImage(url) && !imgCache.IsLoading(url))
                imgCache.RequestImage(url);

            auto& state = m_GifStates[texId];

            if (state.uploaded && tm.EvictionGeneration() != state.uploadedAtGen) {
                bool gifGone    = !state.delaysMs.empty() && tm.GetGifFrameCount(texId) == 0;
                bool staticGone = state.delaysMs.empty()  && tm.GetTexture(texId) == nullptr;
                if (gifGone || staticGone) state = {};
            }

            if (!state.uploaded) {
                auto timingsOpt = imgCache.GetGifTimings(url);
                TalkMe::CachedImage* cached = imgCache.GetImage(url);  // no copy; upload then ReleaseGifPixels

                bool uploadedThisFrame = false;
                if (timingsOpt && cached) {
                    const int w = cached->width;
                    const int h = cached->height;
                    const size_t requiredBytes = (w > 0 && h > 0) ? (size_t)w * (size_t)h * 4u : 0u;
                    bool framesValid = !cached->animatedFrames.empty() && requiredBytes > 0;
                    if (framesValid) {
                        for (const auto& fr : cached->animatedFrames) {
                            if (fr.first.size() < requiredBytes) { framesValid = false; break; }
                        }
                    }
                    if (framesValid && tm.LoadGifFrames(texId, cached->animatedFrames, w, h)) {
                        state.delaysMs      = std::move(timingsOpt->delaysMs);
                        state.totalMs       = 0;
                        for (int d : state.delaysMs) state.totalMs += d;
                        if (state.totalMs <= 0) state.totalMs = 1000;
                        state.uploaded      = true;
                        state.uploadedAtGen = tm.EvictionGeneration();
                        uploadedThisFrame   = true;
                        imgCache.ReleaseGifPixels(url);  // free decoded frames; GPU has the texture
                        RegisterRenderedTexture(texId, url);
                    }
                }

                // Fallback: if we can't upload animated frames, still render the first frame as static.
                if (!uploadedThisFrame && cached) {
                    const size_t requiredBytes = (cached->width > 0 && cached->height > 0)
                        ? (size_t)cached->width * (size_t)cached->height * 4u : 0u;
                    if (cached->ready && requiredBytes > 0 && cached->data.size() >= requiredBytes) {
                        if (tm.LoadFromRGBA(texId, cached->data.data(),
                                             cached->width, cached->height,
                                             false, cached->data.size())) {
                            state = {};
                            state.uploaded      = true;
                            state.uploadedAtGen = tm.EvictionGeneration();
                            imgCache.ReleaseGifPixels(url);
                            RegisterRenderedTexture(texId, url);
                        }
                    }
                }
            }

            ID3D11ShaderResourceView* srv = nullptr;
            if (state.uploaded) {
                if (!state.delaysMs.empty()) {
                    const int frameCount = tm.GetGifFrameCount(texId);
                    if (frameCount > 0) {
                        const double loopMs = std::fmod(now * 1000.0, (double)state.totalMs);
                        int acc = 0;
                        int frameIndex = (int)state.delaysMs.size() - 1;
                        for (int i = 0; i < (int)state.delaysMs.size(); i++) {
                            acc += state.delaysMs[i];
                            if ((int)loopMs < acc) { frameIndex = i; break; }
                        }
                        if (frameIndex < 0) frameIndex = 0;
                        if (frameIndex >= frameCount) frameIndex = frameCount - 1;
                        srv = tm.GetGifFrameSRV(texId, frameIndex);
                    }
                } else {
                    srv = tm.GetTexture(texId);
                }
            }

            if (srv) {
                dl->AddImageRounded((ImTextureID)srv, cellMin, cellMax,
                    ImVec2(0, 0), ImVec2(1, 1),
                    IM_COL32(255, 255, 255, 255), 6.0f);
                dl->AddRect(cellMin, cellMax,
                    IM_COL32(255, 255, 255, 12), 6.0f, 0, 1.0f);
            } else {
                dl->AddRectFilled(cellMin, cellMax, SkeletonBg(), 6.0f);
                dl->AddRectFilled(cellMin, cellMax, ShimmerColor(now, ci), 6.0f);
            }

            ImGui::SetCursorPos(ImVec2(cx, cy));
            ImGui::PushID(ci);
            bool clicked = ImGui::InvisibleButton("##gif_btn", ImVec2(colW, cellH));
            ImGui::PopID();

            if (ImGui::IsItemHovered()) {
                dl->AddRectFilled(cellMin, cellMax,
                    IM_COL32(0, 0, 0, 80), 6.0f);
                if (!gif->title.empty())
                    ImGui::SetTooltip("%s", gif->title.c_str());
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            }

            if (clicked && m_OnSelected) {
                std::string sendUrl = gif->gifUrl.empty() ? gif->previewUrl : gif->gifUrl;
                std::string title = gif->title;
                if (!sendUrl.empty()) {
                    m_OnSelected(sendUrl, title);
                    break;
                }
            }
            } catch (...) {
                dl->AddRectFilled(cellMin, cellMax, SkeletonBg(), 6.0f);
                dl->AddRectFilled(cellMin, cellMax, ShimmerColor(now, ci), 6.0f);
                ImGui::SetCursorPos(ImVec2(cx, cy));
                ImGui::PushID(ci);
                ImGui::InvisibleButton("##gif_btn", ImVec2(colW, cellH));
                ImGui::PopID();
            }
        }

        colY[col] += cellH + kRowGap;
    }

    // Viewport-driven eviction: release textures and free decoded RAM for cells that left the visible area
    auto& tm = TalkMe::TextureManager::Get();
    auto& imgCache = TalkMe::ImageCache::Get();
    std::vector<std::string> toEvict;
    for (const auto& [texId, _] : m_GifStates)
        if (visibleTexIds.count(texId) == 0)
            toEvict.push_back(texId);
    for (const std::string& texId : toEvict) {
        auto urlIt = m_TexToUrl.find(texId);
        if (urlIt != m_TexToUrl.end()) {
            imgCache.RemoveEntry(urlIt->second);  // free decoded frames; next time load from disk
            m_TexToUrl.erase(urlIt);
        }
        tm.RemoveTexture(texId);
        m_GifStates.erase(texId);
        auto it = std::find(m_RenderedOrder.begin(), m_RenderedOrder.end(), texId);
        if (it != m_RenderedOrder.end())
            m_RenderedOrder.erase(it);
    }

    float totalH = std::max(colY[0], colY[1]);
    if (totalH > 0.0f) {
        ImGui::SetCursorPos(ImVec2(0, totalH));
        ImGui::Dummy(ImVec2(1, 1));
    }

    if (m_LoadingMore) {
        ImGui::SetCursorPos(ImVec2((panelW - 100.0f) * 0.5f, totalH + 6.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.5f, 0.55f, 1.0f));
        ImGui::Text("Loading...");
        ImGui::PopStyleColor();
    } else if (!m_HasMore && !m_DisplayedResults.empty()) {
        ImGui::SetCursorPos(ImVec2(0.0f, totalH + 6.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.4f, 0.45f, 1.0f));
        ImGui::Text("No more results");
        ImGui::PopStyleColor();
    }

    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
}

} // namespace TalkMe::UI
