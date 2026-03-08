#include "ChatView.h"
#include "../Components.h"
#include "../Theme.h"
#include "../Styles.h"
#include "../TextureManager.h"
#include "../../app/ResourceLimits.h"
#include "../../shared/PacketHandler.h"
#include "../../network/ImageCache.h"
#include "../../screen/WebcamCapture.h"
#include <string>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <cstdint>
#include <algorithm>
#include <cmath>
#include <shellapi.h>
#include <windows.h>
#include <dwmapi.h>
#include <cstdio>

#pragma comment(lib, "dwmapi.lib")

#ifndef NOMINMAX
#define NOMINMAX
#endif
#undef min
#undef max

namespace TalkMe::UI::Views {

#ifndef PW_RENDERFULLCONTENT
#define PW_RENDERFULLCONTENT 0x00000002
#endif

    namespace {
        using namespace TalkMe::Limits;

        struct GifAnimState {
            std::vector<int> delaysMs;
            int totalMs = 0;
            int width = 0;
            int height = 0;
            bool uploaded = false;
            uint32_t uploadedAtGen = 0;
            // Preserved across eviction so placeholders stay correctly sized
            int cachedW = 0;
            int cachedH = 0;
        };
        std::unordered_map<std::string, GifAnimState> s_ChatGifStates;
        static int s_lastChannelForGifState = -1;

        std::unordered_map<std::string, float> s_MsgHeightCache;
        std::string s_HeightCacheChannel;

        void InvalidateMsgHeights(int channelId) {
            std::string ch = std::to_string(channelId);
            if (s_HeightCacheChannel != ch) {
                s_MsgHeightCache.clear();
                s_HeightCacheChannel = ch;
            }
        }

        float GetEstimatedMsgHeight(int msgId) {
            auto it = s_MsgHeightCache.find(std::to_string(msgId));
            return (it != s_MsgHeightCache.end()) ? it->second : kDefaultMsgHeightPx;
        }

        void RecordMsgHeight(int msgId, float h) {
            if (h > 0.f) s_MsgHeightCache[std::to_string(msgId)] = h;
        }

        constexpr float kSkeletonDefaultW = 280.f;
        constexpr float kSkeletonDefaultH = 180.f;
        constexpr float kSkeletonRounding = 6.f;

        void DrawImageSkeleton(float w, float h) {
            if (w <= 0.f || h <= 0.f) return;
            ImVec2 minP = ImGui::GetCursorScreenPos();
            ImVec2 maxP = ImVec2(minP.x + w, minP.y + h);
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImVec4 base = TalkMe::UI::Styles::C.frameBg;
            float t = (float)ImGui::GetTime();
            float pulse = 0.78f + 0.1f * sinf(t * 2.2f);
            ImU32 col = ImGui::ColorConvertFloat4ToU32(ImVec4(base.x * pulse, base.y * pulse, base.z * pulse, 0.88f));
            dl->AddRectFilled(minP, maxP, col, kSkeletonRounding);
            ImGui::Dummy(ImVec2(w, h));
        }

        struct ContentSegment {
            bool isUrl = false;
            bool isImage = false;
            std::string text;
        };

        std::vector<ContentSegment> ParseMessageContent(const std::string& content) {
            std::vector<ContentSegment> out;
            size_t pos = 0;
            while (pos < content.size()) {
                size_t urlStart = std::string::npos;
                for (const char* prefix : { "https://", "http://", "data:image/" }) {
                    size_t f = content.find(prefix, pos);
                    if (f != std::string::npos && (urlStart == std::string::npos || f < urlStart))
                        urlStart = f;
                }
                if (urlStart == std::string::npos) {
                    if (pos < content.size())
                        out.push_back({ false, false, content.substr(pos) });
                    break;
                }
                if (urlStart > pos)
                    out.push_back({ false, false, content.substr(pos, urlStart - pos) });
                size_t urlEnd = content.find_first_of(" \t\n\r", urlStart);
                if (urlEnd == std::string::npos) urlEnd = content.size();
                std::string url = content.substr(urlStart, urlEnd - urlStart);
                std::string lower = url;
                for (auto& c : lower) c = (char)std::tolower((unsigned char)c);
                bool isImage = (lower.find("data:image/") == 0);
                for (const char* ext : { ".png", ".jpg", ".jpeg", ".gif", ".webp", ".bmp" })
                    if (lower.find(ext) != std::string::npos) isImage = true;
                if (lower.find("tenor.com") != std::string::npos || lower.find("giphy.com") != std::string::npos ||
                    lower.find("imgur.com") != std::string::npos || lower.find("i.redd.it") != std::string::npos)
                    isImage = true;
                out.push_back({ true, isImage, std::move(url) });
                pos = urlEnd;
            }
            return out;
        }

        ID3D11ShaderResourceView* GetAnimatedSrv(
            const std::string& url,
            const std::string& texId,
            double nowSec,
            std::function<void(const std::string&)>* onAttachmentEvicted = nullptr)
        {
            auto& imgCache = TalkMe::ImageCache::Get();
            auto& tm = TalkMe::TextureManager::Get();
            auto& state = s_ChatGifStates[texId];

            // If we think we're uploaded but the texture is gone (e.g. evicted when scrolled away), reset so we re-upload.
            if (state.uploaded) {
                bool gifGone = !state.delaysMs.empty() && tm.GetGifFrameCount(texId) == 0;
                bool staticGone = state.delaysMs.empty() && tm.GetTexture(texId) == nullptr;
                if (gifGone || staticGone) {
                    // Preserve known dimensions so placeholder Dummies stay correctly sized
                    const int prevW = state.width > 0 ? state.width : state.cachedW;
                    const int prevH = state.height > 0 ? state.height : state.cachedH;
                    const bool isAttachment = (texId.size() >= 4u && texId.compare(0, 4, "att_") == 0);
                    if (isAttachment && onAttachmentEvicted && *onAttachmentEvicted) {
                        (*onAttachmentEvicted)(texId.substr(4));
                        state = {};
                        state.cachedW = prevW;
                        state.cachedH = prevH;
                        // Also re-request from ImageCache so when we scroll back the image can re-appear.
                        if (gifGone) {
                            if (!imgCache.ScheduleRedecodeFromDisk(url))
                                imgCache.RequestImage(url);
                        } else {
                            imgCache.RequestImage(url);
                        }
                        return nullptr;
                    }
                    if (gifGone) {
                        state = {};
                        state.cachedW = prevW;
                        state.cachedH = prevH;
                        if (!imgCache.ScheduleRedecodeFromDisk(url)) {
                            imgCache.RemoveEntry(url);
                            imgCache.RequestImage(url);
                        }
                    }
                    else if (staticGone) {
                        state = {};
                        state.cachedW = prevW;
                        state.cachedH = prevH;
                        imgCache.RemoveEntry(url);
                        imgCache.RequestImage(url);
                    }
                }
            }

            if (state.uploaded) {
                if (!state.delaysMs.empty()) {
                    const int frameCount = tm.GetGifFrameCount(texId);
                    if (frameCount > 0) {
                        const double loopMs = std::fmod(nowSec * 1000.0, (double)state.totalMs);
                        int acc = 0, frameIndex = (int)state.delaysMs.size() - 1;
                        for (int i = 0; i < (int)state.delaysMs.size(); i++) {
                            acc += state.delaysMs[i];
                            if ((int)loopMs < acc) { frameIndex = i; break; }
                        }
                        if (frameIndex < 0) frameIndex = 0;
                        if (frameIndex >= frameCount) frameIndex = frameCount - 1;
                        return tm.GetGifFrameSRV(texId, frameIndex);
                    }
                }
                else {
                    return tm.GetTexture(texId);
                }
            }

            if (auto timings = imgCache.GetGifTimings(url)) {
                TalkMe::CachedImage* cached = imgCache.GetImage(url);  // no copy; upload then release
                if (cached) {
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
                        state.delaysMs = std::move(timings->delaysMs);
                        state.totalMs = 0;
                        for (int d : state.delaysMs) state.totalMs += d;
                        if (state.totalMs <= 0) state.totalMs = 1000;
                        state.width = timings->width;
                        state.height = timings->height;
                        state.cachedW = timings->width;
                        state.cachedH = timings->height;
                        state.uploaded = true;
                        state.uploadedAtGen = tm.EvictionGeneration();
                        imgCache.ReleaseGifPixels(url);
                        const int fc = tm.GetGifFrameCount(texId);
                        return (fc > 0) ? tm.GetGifFrameSRV(texId, 0) : nullptr;
                    }
                }
            }
            else if (auto dims = imgCache.GetReadyDimensions(url)) {
                TalkMe::CachedImage* cached = imgCache.GetImage(url);
                if (cached) {
                    const size_t requiredBytes = (cached->width > 0 && cached->height > 0)
                        ? (size_t)cached->width * (size_t)cached->height * 4u : 0u;
                    if (requiredBytes > 0 && cached->data.size() >= requiredBytes) {
                        auto* srv = tm.LoadFromRGBA(texId, cached->data.data(),
                            cached->width, cached->height,
                            false, cached->data.size());
                        if (srv) {
                            state.width = cached->width;
                            state.height = cached->height;
                            state.cachedW = cached->width;
                            state.cachedH = cached->height;
                            state.uploaded = true;
                            state.uploadedAtGen = tm.EvictionGeneration();
                            imgCache.ReleaseGifPixels(url);  // free GIF frames if entry was animated
                            return srv;
                        }
                    }
                }
            }
            // Fallback: upload from a copy in case of race or previous LoadFromRGBA failure (e.g. device was null).
            {
                auto cachedOpt = imgCache.GetImageCopy(url);
                if (cachedOpt && cachedOpt->ready && cachedOpt->width > 0 && cachedOpt->height > 0) {
                    const size_t requiredBytes = (size_t)cachedOpt->width * (size_t)cachedOpt->height * 4u;
                    if (cachedOpt->data.size() >= requiredBytes) {
                        auto* srv = tm.LoadFromRGBA(texId, cachedOpt->data.data(),
                            cachedOpt->width, cachedOpt->height,
                            false, cachedOpt->data.size());
                        if (srv) {
                            state.width = cachedOpt->width;
                            state.height = cachedOpt->height;
                            state.cachedW = cachedOpt->width;
                            state.cachedH = cachedOpt->height;
                            state.uploaded = true;
                            state.uploadedAtGen = tm.EvictionGeneration();
                            return srv;
                        }
                    }
                }
            }

            return nullptr;
        }
    }

    void RenderChannelView(NetworkClient& netClient, UserSession& currentUser, const Server& currentServer,
        std::vector<ChatMessage>& messages, int& selectedChannelId, int& activeVoiceChannelId,
        std::vector<std::string>& voiceMembers, std::map<std::string, float>& speakingTimers,
        std::map<std::string, float>& userVolumes, std::function<void(const std::string&, float)> setUserVolume,
        char* chatInputBuf, bool selfMuted, bool selfDeafened,
        const std::map<std::string, UserVoiceState>* userMuteStates,
        const std::map<std::string, float>* typingUsers,
        std::function<void()> onUserTyping,
        int* replyingToMessageId,
        const std::vector<std::pair<std::string, bool>>* serverMembers,
        bool* showMemberList,
        char* searchBuf,
        bool* showSearch,
        std::function<void(int fps, int quality, int maxW, int maxH, void* hwnd, int sourceType, const std::string& sourceId)> onStartScreenShare,
        std::function<void()> onStopScreenShare,
        bool isScreenSharing,
        bool someoneIsSharing,
        void* screenShareTexture,
        int screenShareW,
        int screenShareH,
        const std::vector<std::string>* activeStreamers,
        std::string* viewingStream,
        bool* streamMaximized,
        bool* showGifPicker,
        std::function<void(float w, float h)> renderGifPanel,
        const std::string* mediaBaseUrl,
        std::function<void(std::vector<uint8_t>, std::string)>* onImageUpload,
        std::function<void(const std::string&)>* requestAttachment,
        std::function<const TalkMe::AttachmentDisplay* (const std::string&)>* getAttachmentDisplay,
        std::function<void(const std::string&)>* onAttachmentClick,
        std::function<void(const std::string&)>* onAttachmentTextureEvicted,
        std::function<void()> requestOlderMessages,
        const bool* loadingOlder,
        std::function<void(int channelId, int fullyVisibleMid)> onReadAnchorAdvanced,
        std::string* attachedGifUrl,
        std::string* attachedImageFilename,
        std::function<void()> onClearAttachedImage,
        std::function<void(std::vector<uint8_t>, std::string)>* onAttachImage,
        std::function<void(const std::string&, int)>* onSendWithAttachedImage,
        const bool* gameMode,
        const bool* isDraggingFilesOver,
        int screenShareTargetFps,
        float screenShareStreamFps,
        float screenSharePreviewFps)
    {
        const bool gameModeOn = (gameMode && *gameMode);
        const bool showDragOverlay = (isDraggingFilesOver && *isDraggingFilesOver);

        float winH = ImGui::GetWindowHeight();
        float winW = ImGui::GetWindowWidth();
        float left = Styles::MainContentLeftOffset;
        float contentW = winW - left;
        // When GIF panel is open, it takes 30% of content area on the right; chat uses the rest
        float gifPanelW = 0.0f;
        if (!gameModeOn && showGifPicker && *showGifPicker) {
            gifPanelW = contentW * 0.30f;
            if (gifPanelW < 200.0f) gifPanelW = 200.0f;
        }
        float areaW = contentW - gifPanelW;

        ImGui::SetCursorPos(ImVec2(left, 0));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, Styles::BgChat());
        ImGui::BeginChild("ChatArea", ImVec2(areaW, winH), false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

        if (selectedChannelId != -1) {
            std::string chName = "Unknown";
            std::string chDesc;
            ChannelType cType = ChannelType::Text;
            for (const auto& ch : currentServer.channels)
                if (ch.id == selectedChannelId) { chName = ch.name; cType = ch.type; chDesc = ch.description; break; }

            // ==================== VOICE VIEW ====================
            if (cType == ChannelType::Voice) {

                ImGui::Dummy(ImVec2(0, 28));
                ImGui::Indent(40);
                ImGui::PushStyleColor(ImGuiCol_Text, Styles::Accent());
                ImGui::SetWindowFontScale(1.4f);
                ImGui::Text("%s", chName.c_str());
                ImGui::SetWindowFontScale(1.0f);
                ImGui::PopStyleColor();

                ImGui::PushStyleColor(ImGuiCol_Text, Styles::TextMuted());
                ImGui::Text("%d connected", (int)voiceMembers.size());
                ImGui::PopStyleColor();
                ImGui::Unindent(40);

                ImGui::Dummy(ImVec2(0, 12));
                ImGui::PushStyleColor(ImGuiCol_Separator, Styles::Separator());
                ImGui::Separator();
                ImGui::PopStyleColor();
                ImGui::Dummy(ImVec2(0, 16));

                float leaveBarH = 120.0f;
                float gridTop = ImGui::GetCursorPosY();
                float gridH = winH - gridTop - leaveBarH;
                if (gridH < 100.0f) gridH = 100.0f;

                // When screen sharing: split into screen viewport (top) + user strip (bottom) (disabled in game mode)
                bool showingScreenShare = !gameModeOn && (someoneIsSharing || isScreenSharing);
                float screenViewH = showingScreenShare ? (gridH * 0.7f) : 0.0f;
                float userStripH = showingScreenShare ? (gridH - screenViewH) : gridH;

                if (showingScreenShare) {
                    bool isMaximized = streamMaximized && *streamMaximized;
                    bool canMaximize = (streamMaximized != nullptr);

                    // When maximized: take the ENTIRE voice area (grid + user strip + button bar)
                    float fullH = winH - ImGui::GetCursorPosY();
                    float actualViewH = isMaximized ? fullH : screenViewH;

                    // Stream switcher tabs (only when not maximized or when multiple streams)
                    if (!isMaximized && activeStreamers && activeStreamers->size() > 1 && viewingStream) {
                        for (size_t i = 0; i < activeStreamers->size(); i++) {
                            const auto& streamer = (*activeStreamers)[i];
                            std::string disp = streamer;
                            size_t hp = disp.find('#');
                            if (hp != std::string::npos) disp = disp.substr(0, hp);
                            bool selected = (*viewingStream == streamer);
                            if (i > 0) ImGui::SameLine();
                            if (selected) ImGui::PushStyleColor(ImGuiCol_Button, Styles::Accent());
                            if (ImGui::SmallButton((disp + "##stream").c_str()))
                                *viewingStream = streamer;
                            if (selected) ImGui::PopStyleColor();
                        }
                        actualViewH -= ImGui::GetTextLineHeightWithSpacing() + 4;
                    }

                    // Pre-compute stream image layout before entering child so the
                    // Fullscreen button can be rendered AFTER EndChild (correct Z-order).
                    float fitW = 0.0f, fitH = 0.0f, padX = 0.0f, padY = 0.0f;
                    bool hasStream = (screenShareTexture && screenShareW > 0 && screenShareH > 0);
                    if (hasStream) {
                        float viewW = areaW - 10.0f;
                        float viewH = actualViewH - 10.0f;
                        float aspect = (float)screenShareW / (float)screenShareH;
                        fitW = viewW;
                        fitH = fitW / aspect;
                        if (fitH > viewH) { fitH = viewH; fitW = fitH * aspect; }
                        padX = (areaW - fitW) * 0.5f;
                        padY = (actualViewH - fitH) * 0.5f;
                    }

                    // Track viewport top in ChatArea coords for the button overlay below.
                    float viewportTopY = ImGui::GetCursorPosY();

                    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.05f, 0.05f, 0.07f, 1.0f));
                    ImGui::BeginChild("ScreenViewport", ImVec2(areaW, actualViewH), false,
                        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

                    if (hasStream) {
                        ImGui::SetCursorPos(ImVec2(padX, padY));
                        ImGui::Image((ImTextureID)screenShareTexture, ImVec2(fitW, fitH));

                        // FPS stats overlay pinned to the top-left corner of the stream image.
                        ImGui::SetCursorPos(ImVec2(padX + 10.0f, padY + 10.0f));
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 0.9f));
                        {
                            const float uiFps = ImGui::GetIO().Framerate;
                            const bool hasStream_ = (screenShareStreamFps >= 1.0f);
                            const bool hasPreview  = (screenSharePreviewFps >= 1.0f);
                            if (!hasStream_ && !hasPreview)
                                ImGui::Text("Stream: -- | Preview: -- (target %d) | UI: %.0f fps",
                                    screenShareTargetFps, uiFps);
                            else if (!hasStream_)
                                ImGui::Text("Stream: -- | Preview: %.0f fps (target %d) | UI: %.0f fps",
                                    screenSharePreviewFps, screenShareTargetFps, uiFps);
                            else if (!hasPreview)
                                ImGui::Text("Stream: %.0f fps | Preview: -- (target %d) | UI: %.0f fps",
                                    screenShareStreamFps, screenShareTargetFps, uiFps);
                            else
                                ImGui::Text("Stream: %.0f fps | Preview: %.0f fps (target %d) | UI: %.0f fps",
                                    screenShareStreamFps, screenSharePreviewFps, screenShareTargetFps, uiFps);
                        }
                        ImGui::PopStyleColor();
                    }
                    else {
                        float phH = actualViewH * 0.4f;
                        ImGui::Dummy(ImVec2(0, (actualViewH - phH) * 0.5f - 20));
                        float placeholderW = areaW * 0.5f;
                        ImGui::SetCursorPosX((areaW - placeholderW) * 0.5f);
                        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.08f, 0.08f, 0.1f, 1.0f));
                        ImGui::BeginChild("##placeholder", ImVec2(placeholderW, phH), true);
                        ImGui::Dummy(ImVec2(0, phH * 0.3f));
                        std::string sharerName = isScreenSharing ? "You" : "Someone";
                        std::string msg2 = sharerName + " is sharing their screen...";
                        ImVec2 sz2 = ImGui::CalcTextSize(msg2.c_str());
                        ImGui::SetCursorPosX((placeholderW - sz2.x) * 0.5f);
                        ImGui::TextColored(ImVec4(0.5f, 0.7f, 1.0f, 1.0f), "%s", msg2.c_str());
                        ImGui::SetCursorPosX((placeholderW - 140) * 0.5f);
                        ImGui::TextDisabled("Waiting for video stream...");
                        ImGui::EndChild();
                        ImGui::PopStyleColor();
                    }

                    ImGui::EndChild();
                    ImGui::PopStyleColor();

                    // Render the Fullscreen/Exit button AFTER EndChild so it is drawn on top
                    // of the viewport contents (correct Z-order, no clipping by child rect).
                    if (canMaximize && hasStream) {
                        ImVec2 savedCursor = ImGui::GetCursorPos();
                        float btnX = padX + fitW - 80.0f;
                        float btnY = viewportTopY + padY + fitH - 30.0f;
                        ImGui::SetCursorPos(ImVec2(btnX, btnY));
                        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.0f, 0.0f, 0.0f, 0.65f));
                        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.2f, 0.2f, 0.2f, 0.85f));
                        ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(1.0f, 1.0f, 1.0f, 0.95f));
                        if (ImGui::Button(isMaximized ? "Exit" : "Fullscreen", ImVec2(80, 26)))
                            *streamMaximized = !*streamMaximized;
                        ImGui::PopStyleColor(3);
                        ImGui::SetCursorPos(savedCursor);
                    }

                    // When maximized: skip everything below (users, action bar).
                    if (isMaximized) {
                        ImGui::EndChild(); // ChatArea
                        ImGui::PopStyleColor();
                        return;
                    }
                }

                // User strip (hidden when stream maximized)
                bool hideUsers = (streamMaximized && *streamMaximized && showingScreenShare);
                float actualUserH = hideUsers ? 0.0f : userStripH;
                if (!hideUsers) {
                    ImGui::BeginChild("VoiceGrid", ImVec2(areaW, actualUserH), false, ImGuiWindowFlags_None);
                    {

                        float avatarR = Styles::AvatarRadius;
                        float itemW = Styles::VoiceItemWidth;
                        float itemH = Styles::VoiceItemHeight;
                        float gap = Styles::VoiceItemSpacing;
                        float cardR = Styles::VoiceCardRounding;

                        float usableW = areaW - 60.0f;
                        int cols = (std::max)(1, (int)(usableW / (itemW + gap)));
                        float totalW = cols * itemW + (cols - 1) * gap;
                        float padX = (areaW - totalW) * 0.5f;
                        if (padX < 20.0f) padX = 20.0f;

                        static std::string s_volPopupMember;
                        bool wantPopup = false;
                        int col = 0;

                        for (size_t mi = 0; mi < voiceMembers.size(); mi++) {
                            const auto& member = voiceMembers[mi];

                            if (col == 0) {
                                ImGui::Dummy(ImVec2(padX - 10.0f, 0));
                                ImGui::SameLine();
                            }

                            ImGui::PushID((int)mi);
                            ImGui::BeginGroup();

                            ImVec2 pos = ImGui::GetCursorScreenPos();
                            ImGui::InvisibleButton("##card", ImVec2(itemW, itemH));
                            if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(1) && setUserVolume) {
                                s_volPopupMember = member;
                                wantPopup = true;
                            }

                            ImDrawList* dl = ImGui::GetWindowDrawList();
                            bool speaking = speakingTimers.count(member) && ((float)ImGui::GetTime() - speakingTimers[member] < 0.5f);
                            bool muted = userVolumes.count(member) && userVolumes[member] <= 0.0f;

                            // Card background
                            ImVec2 cardEnd = ImVec2(pos.x + itemW, pos.y + itemH);
                            if (speaking) {
                                // Glow behind card
                                dl->AddRectFilled(
                                    ImVec2(pos.x - 3, pos.y - 3),
                                    ImVec2(cardEnd.x + 3, cardEnd.y + 3),
                                    Styles::ColSpeakingGlow(), cardR + 3);
                            }
                            dl->AddRectFilled(pos, cardEnd, Styles::ColBgCard(), cardR);

                            if (speaking)
                                dl->AddRect(pos, cardEnd, Styles::ColSpeakingRing(), cardR, 0, 2.0f);

                            // Avatar circle centered in card
                            ImVec2 ctr = ImVec2(pos.x + itemW * 0.5f, pos.y + 12.0f + avatarR);

                            // Check for avatar texture
                            auto* avatarSrv = TalkMe::TextureManager::Get().GetTexture("avatar_" + member);
                            if (avatarSrv) {
                                // Draw circular avatar using a square image (ImGui clips to card)
                                float avSz = avatarR * 2.0f;
                                ImVec2 avMin(ctr.x - avatarR, ctr.y - avatarR);
                                ImVec2 avMax(ctr.x + avatarR, ctr.y + avatarR);
                                dl->AddCircleFilled(ctr, avatarR, Styles::ColBgAvatar());
                                dl->AddImageRounded((ImTextureID)avatarSrv, avMin, avMax,
                                    ImVec2(0, 0), ImVec2(1, 1), IM_COL32(255, 255, 255, 255), avatarR);
                            }
                            else {
                                dl->AddCircleFilled(ctr, avatarR, Styles::ColBgAvatar());
                                // Initials fallback
                                std::string init = member.substr(0, (std::min)((size_t)2, member.size()));
                                ImVec2 tsz = ImGui::GetFont()->CalcTextSizeA(Styles::VoiceAvatarFontSize, FLT_MAX, 0, init.c_str());
                                dl->AddText(ImGui::GetFont(), Styles::VoiceAvatarFontSize,
                                    ImVec2(ctr.x - tsz.x * 0.5f, ctr.y - tsz.y * 0.5f),
                                    Styles::ColTextOnAvatar(), init.c_str());
                            }

                            // Name
                            std::string disp = member;
                            size_t hp = member.find('#');
                            if (hp != std::string::npos) disp = member.substr(0, hp);
                            if (disp.size() > 12) disp = disp.substr(0, 11) + "..";

                            ImVec2 nsz = ImGui::CalcTextSize(disp.c_str());
                            float nameY = pos.y + avatarR * 2.0f + 22.0f;
                            ImU32 nCol = speaking ? Styles::ColSpeakingRing() :
                                (muted ? Styles::ColMutedText() : Styles::ColTextName());
                            dl->AddText(ImVec2(pos.x + (itemW - nsz.x) * 0.5f, nameY), nCol, disp.c_str());

                            // Status icons below name
                            bool isMe = (member == currentUser.username);
                            bool micOff = false;
                            bool spkOff = false;
                            if (isMe) {
                                micOff = selfMuted;
                                spkOff = selfDeafened;
                            }
                            else if (userMuteStates) {
                                auto sit = userMuteStates->find(member);
                                if (sit != userMuteStates->end()) {
                                    micOff = sit->second.muted;
                                    spkOff = sit->second.deafened;
                                }
                            }

                            if (muted || micOff || spkOff) {
                                float iconY = nameY + 18.0f;
                                std::string status;
                                if (muted && !isMe) {
                                    status = "MUTED";
                                }
                                else {
                                    if (micOff) status += "MIC X";
                                    if (spkOff) { if (!status.empty()) status += "  "; status += "SPK X"; }
                                }
                                if (!status.empty()) {
                                    ImVec2 ms = ImGui::CalcTextSize(status.c_str());
                                    dl->AddText(ImVec2(pos.x + (itemW - ms.x) * 0.5f, iconY), Styles::ColMutedLabel(), status.c_str());
                                }
                            }

                            ImGui::EndGroup();

                            // Admin right-click context menu on member cards
                            if (!isMe && ImGui::BeginPopupContextItem(("admin_" + member).c_str())) {
                                std::string dName = member;
                                size_t hp2 = dName.find('#');
                                if (hp2 != std::string::npos) dName = dName.substr(0, hp2);
                                ImGui::TextDisabled("%s", dName.c_str());
                                ImGui::Separator();

                                if (ImGui::Selectable("Mute User")) {
                                    nlohmann::json aj; aj["sid"] = currentServer.id; aj["u"] = member; aj["state"] = true;
                                    netClient.Send(PacketType::Admin_Force_Mute, aj.dump());
                                }
                                if (ImGui::Selectable("Deafen User")) {
                                    nlohmann::json aj; aj["sid"] = currentServer.id; aj["u"] = member; aj["state"] = true;
                                    netClient.Send(PacketType::Admin_Force_Deafen, aj.dump());
                                }
                                if (ImGui::Selectable("Disconnect from Voice")) {
                                    nlohmann::json aj; aj["sid"] = currentServer.id; aj["u"] = member;
                                    netClient.Send(PacketType::Admin_Disconnect_User, aj.dump());
                                }
                                ImGui::Separator();

                                // Move to another voice channel
                                if (ImGui::BeginMenu("Move to Channel")) {
                                    for (const auto& ch : currentServer.channels) {
                                        if (ch.type != ChannelType::Voice || ch.id == activeVoiceChannelId) continue;
                                        if (ImGui::MenuItem(ch.name.c_str())) {
                                            nlohmann::json aj; aj["sid"] = currentServer.id; aj["u"] = member; aj["cid"] = ch.id;
                                            netClient.Send(PacketType::Admin_Move_User, aj.dump());
                                        }
                                    }
                                    ImGui::EndMenu();
                                }

                                ImGui::Separator();
                                if (ImGui::Selectable("Chat Mute (10 min)")) {
                                    nlohmann::json aj; aj["sid"] = currentServer.id; aj["u"] = member;
                                    aj["type"] = "chat_mute"; aj["reason"] = "Admin action"; aj["duration_minutes"] = 10;
                                    netClient.Send(PacketType::Admin_Sanction_User, aj.dump());
                                }
                                if (ImGui::Selectable("Chat Mute (1 hour)")) {
                                    nlohmann::json aj; aj["sid"] = currentServer.id; aj["u"] = member;
                                    aj["type"] = "chat_mute"; aj["reason"] = "Admin action"; aj["duration_minutes"] = 60;
                                    netClient.Send(PacketType::Admin_Sanction_User, aj.dump());
                                }

                                ImGui::Separator();
                                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.3f, 0.7f, 1.0f, 1.0f));
                                if (ImGui::Selectable("Grant Admin")) {
                                    nlohmann::json aj; aj["sid"] = currentServer.id; aj["u"] = member;
                                    aj["perms"] = 0xF; // All permissions
                                    netClient.Send(PacketType::Set_Member_Role, aj.dump());
                                }
                                ImGui::PopStyleColor();

                                ImGui::EndPopup();
                            }

                            ImGui::PopID();

                            col++;
                            if (col < cols) {
                                ImGui::SameLine(0, gap);
                            }
                            else {
                                col = 0;
                            }
                        }

                        // Volume popup
                        if (wantPopup)
                            ImGui::OpenPopup("UserVolumePopup");

                        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Styles::PopupPadding, Styles::PopupPadding));
                        ImGui::PushStyleColor(ImGuiCol_PopupBg, Styles::BgPopup());
                        if (ImGui::BeginPopup("UserVolumePopup")) {
                            if (!s_volPopupMember.empty() && setUserVolume) {
                                const std::string& m = s_volPopupMember;
                                std::string d = m;
                                size_t hp2 = m.find('#');
                                if (hp2 != std::string::npos) d = m.substr(0, hp2);

                                ImGui::Text("%s", d.c_str());
                                ImGui::Separator();
                                ImGui::Dummy(ImVec2(0, 4));

                                bool isSelf = (m == currentUser.username);
                                if (!isSelf) {
                                    float vol = userVolumes.count(m) ? userVolumes[m] : 1.0f;
                                    if (vol <= 0.0f) {
                                        if (UI::AccentButton("Unmute", ImVec2(220, 30))) {
                                            setUserVolume(m, 1.0f);
                                            ImGui::CloseCurrentPopup();
                                        }
                                    }
                                    else {
                                        ImGui::PushStyleColor(ImGuiCol_Button, Styles::ButtonDanger());
                                        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Styles::ButtonDangerHover());
                                        if (ImGui::Button("Mute", ImVec2(220, 30))) {
                                            setUserVolume(m, 0.0f);
                                            ImGui::CloseCurrentPopup();
                                        }
                                        ImGui::PopStyleColor(2);
                                    }

                                    ImGui::Dummy(ImVec2(0, 6));
                                    float pct = vol * 100.0f;
                                    ImGui::SetNextItemWidth(220);
                                    if (ImGui::SliderFloat("##Vol", &pct, 0, 200, "%.0f%%"))
                                        setUserVolume(m, pct / 100.0f);
                                }
                                else {
                                    ImGui::PushStyleColor(ImGuiCol_Text, Styles::TextMuted());
                                    ImGui::TextDisabled("Use MIC / SPK in the sidebar to mute or deafen yourself.");
                                    ImGui::PopStyleColor();
                                }
                            }
                            ImGui::EndPopup();
                        }
                        ImGui::PopStyleColor();
                        ImGui::PopStyleVar();

                    } // close user grid scope
                    ImGui::EndChild(); // VoiceGrid
                } // close !hideUsers

                const bool inThisVoiceCall = (activeVoiceChannelId == selectedChannelId);

                // ====== Bottom action bar ======
                ImGui::Dummy(ImVec2(0, 8));
                float btnBarW = inThisVoiceCall ? 380.0f : 160.0f;
                float btnBarX = (areaW - btnBarW) * 0.5f;
                if (btnBarX < 10.0f) btnBarX = 10.0f;
                ImGui::SetCursorPosX(btnBarX);

                float actionBtnH = 38.0f;
                ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f);

                if (!inThisVoiceCall) {
                    // Not connected: show Join button only.
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.55f, 0.25f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.22f, 0.65f, 0.3f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
                    if (ImGui::Button("Join call", ImVec2(160, actionBtnH))) {
                        activeVoiceChannelId = selectedChannelId;
                        netClient.Send(PacketType::Join_Voice_Channel, PacketHandler::JoinVoiceChannelPayload(selectedChannelId));
                    }
                    ImGui::PopStyleColor(3);
                }
                else {
                    // Connected: show Leave / Screen Share / Games.

                    // Button 1: Leave (red)
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.7f, 0.12f, 0.12f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.85f, 0.18f, 0.18f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
                    if (ImGui::Button("Leave", ImVec2(100, actionBtnH))) {
                        activeVoiceChannelId = -1;
                        netClient.Send(PacketType::Join_Voice_Channel, PacketHandler::JoinVoiceChannelPayload(-1));
                    }
                    ImGui::PopStyleColor(3);

                    // Button 2: Screen Share (blue) or Stop (if sharing) — hidden in game mode
                    if (!gameModeOn) {
                        ImGui::SameLine(0, 12);
                        if (isScreenSharing) {
                            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.6f, 0.3f, 0.1f, 1.0f));
                            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.7f, 0.35f, 0.12f, 1.0f));
                            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
                            if (ImGui::Button("Stop Share", ImVec2(130, actionBtnH))) {
                                if (onStopScreenShare) onStopScreenShare();
                            }
                            ImGui::PopStyleColor(3);
                        }
                        else {
                            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.35f, 0.65f, 1.0f));
                            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.22f, 0.42f, 0.75f, 1.0f));
                            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
                            if (ImGui::Button("Screen Share", ImVec2(130, actionBtnH))) {
                                ImGui::OpenPopup("ScreenShareSetup");
                            }
                            ImGui::PopStyleColor(3);
                        }
                    }

                    // Button 3: Games (green)
                    ImGui::SameLine(0, 12);
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.55f, 0.25f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.2f, 0.65f, 0.3f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
                    if (ImGui::Button("Games", ImVec2(100, actionBtnH))) {
                        ImGui::OpenPopup("GamesPicker");
                    }
                    ImGui::PopStyleColor(3);
                }

                ImGui::PopStyleVar();

                // ====== Screen Share — Discord-style modal with 3 tabs ======
                {
                    struct WinEntry { HWND hwnd; std::string title; };
                    struct CamEntry { std::string name; std::string symLink; };
                    static std::vector<WinEntry> s_windows;
                    static std::vector<CamEntry> s_cameras;
                    static int s_tab = 0;        // 0=Applications, 1=Screens, 2=Cameras
                    static int s_selApp = -1;
                    static int s_selCam = -1;
                    static int s_shareFps = 1;
                    static int s_shareQuality = 1;
                    static int s_shareRes = 1;
                    static bool s_refresh = true;
                    static bool s_popupWasOpen = false;

                    auto updateSharePickerThumb = [](const std::string& texId, HWND srcWindow, bool captureDesktop, int outW, int outH) {
                        if (outW <= 0 || outH <= 0) return;
                        HDC memDC = ::CreateCompatibleDC(nullptr);
                        if (!memDC) return;

                        BITMAPINFO bmi = {};
                        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                        bmi.bmiHeader.biWidth = outW;
                        bmi.bmiHeader.biHeight = -outH;
                        bmi.bmiHeader.biPlanes = 1;
                        bmi.bmiHeader.biBitCount = 32;
                        bmi.bmiHeader.biCompression = BI_RGB;

                        void* bits = nullptr;
                        HBITMAP dib = ::CreateDIBSection(memDC, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
                        if (!dib || !bits) {
                            if (dib) ::DeleteObject(dib);
                            ::DeleteDC(memDC);
                            ::ReleaseDC(nullptr, srcDC);
                            return;
                        }
                        HGDIOBJ old = ::SelectObject(memDC, dib);
                        ::SetStretchBltMode(memDC, HALFTONE);

                        bool copied = false;
                        if (!captureDesktop && srcWindow && ::IsWindow(srcWindow)) {
                            // Capture independent window content even if obscured by our popup.
                            if (::PrintWindow(srcWindow, memDC, PW_RENDERFULLCONTENT) != 0) {
                                copied = true;
                            } else {
                                RECT wr = {};
                                if (::GetWindowRect(srcWindow, &wr)) {
                                    HDC wndDC = ::GetWindowDC(srcWindow);
                                    if (wndDC) {
                                        int srcW = (std::max)(1, wr.right - wr.left);
                                        int srcH = (std::max)(1, wr.bottom - wr.top);
                                        ::StretchBlt(memDC, 0, 0, outW, outH, wndDC, 0, 0, srcW, srcH, SRCCOPY);
                                        ::ReleaseDC(srcWindow, wndDC);
                                        copied = true;
                                    }
                                }
                            }
                        }
                        if (!copied) {
                            HDC srcDC = ::GetDC(nullptr);
                            if (!srcDC) {
                                ::SelectObject(memDC, old);
                                ::DeleteObject(dib);
                                ::DeleteDC(memDC);
                                return;
                            }
                            RECT src = { 0, 0, ::GetSystemMetrics(SM_CXSCREEN), ::GetSystemMetrics(SM_CYSCREEN) };
                            int srcW = (std::max)(1, src.right - src.left);
                            int srcH = (std::max)(1, src.bottom - src.top);
                            ::StretchBlt(memDC, 0, 0, outW, outH, srcDC, src.left, src.top, srcW, srcH, SRCCOPY);
                            ::ReleaseDC(nullptr, srcDC);
                        }
                        std::vector<uint8_t> rgba(static_cast<size_t>(outW) * static_cast<size_t>(outH) * 4);
                        const uint8_t* bgra = reinterpret_cast<const uint8_t*>(bits);
                        for (int i = 0; i < outW * outH; i++) {
                            rgba[i * 4 + 0] = bgra[i * 4 + 2];
                            rgba[i * 4 + 1] = bgra[i * 4 + 1];
                            rgba[i * 4 + 2] = bgra[i * 4 + 0];
                            rgba[i * 4 + 3] = 255;
                        }
                        TalkMe::TextureManager::Get().LoadFromRGBA(texId, rgba.data(), outW, outH, false, rgba.size());

                        ::SelectObject(memDC, old);
                        ::DeleteObject(dib);
                        ::DeleteDC(memDC);
                    };

                    if (s_refresh) {
                        TalkMe::TextureManager::Get().EvictTexturesWithPrefixExcept("sharepick_", std::unordered_set<std::string>{});
                        s_windows.clear();
                        s_cameras.clear();
                        s_selApp = -1;
                        s_selCam = -1;
                        ::EnumWindows([](HWND h, LPARAM lp) -> BOOL {
                            if (!::IsWindowVisible(h)) return TRUE;
                            if (::GetWindowLongW(h, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) return TRUE;
                            if (::GetWindow(h, GW_OWNER)) return TRUE;
                            DWORD pid = 0; ::GetWindowThreadProcessId(h, &pid);
                            if (pid == ::GetCurrentProcessId()) return TRUE; // Hide TalkMe itself.
                            BOOL cloaked = FALSE;
                            if (SUCCEEDED(::DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked)
                                return TRUE;
                            wchar_t buf[256] = {};
                            if (::GetWindowTextW(h, buf, 256) <= 0) return TRUE;
                            char u8[512] = {};
                            ::WideCharToMultiByte(CP_UTF8, 0, buf, -1, u8, 512, nullptr, nullptr);
                            std::string t(u8);
                            if (t.empty() || t == "Program Manager") return TRUE;
                            reinterpret_cast<std::vector<WinEntry>*>(lp)->push_back({h, std::move(t)});
                            return TRUE;
                        }, (LPARAM)&s_windows);
                        // Enumerate cameras
                        auto devs = TalkMe::WebcamCapture::EnumerateDevices();
                        for (auto& d : devs)
                            s_cameras.push_back({std::move(d.name), std::move(d.symbolicLink)});
                        if (s_selApp < 0 && !s_windows.empty()) s_selApp = 0;
                        if (s_selCam < 0 && !s_cameras.empty()) s_selCam = 0;
                        // One-shot thumbnails on open/refresh (not continuous updates).
                        const int thumbW = 320;
                        const int thumbH = 180;
                        for (size_t i = 0; i < s_windows.size() && i < 8; ++i) {
                            const std::string texId = "sharepick_app_" + std::to_string(reinterpret_cast<uintptr_t>(s_windows[i].hwnd));
                            updateSharePickerThumb(texId, s_windows[i].hwnd, false, thumbW, thumbH);
                        }
                        updateSharePickerThumb("sharepick_screen", nullptr, true, thumbW, thumbH);
                        s_refresh = false;
                    }

                    const float popW = 940.0f, popH = 640.0f;
                    ImGui::SetNextWindowSize(ImVec2(popW, popH), ImGuiCond_Always);
                    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
                    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
                    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 12.0f);
                    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0.11f, 0.11f, 0.13f, 1.0f));

                    if (ImGui::BeginPopup("ScreenShareSetup")) {
                        s_popupWasOpen = true;
                        const float pad = 24.0f;
                        const float cW = popW - pad * 2;
                        const ImVec4 accent(0.40f, 0.52f, 0.96f, 1.0f);
                        const ImVec4 accentH(0.48f, 0.60f, 1.0f, 1.0f);
                        const ImVec4 tabSel(0.18f, 0.18f, 0.22f, 1.0f);
                        const ImVec4 tabDef(0.13f, 0.13f, 0.16f, 1.0f);
                        const ImVec4 cardBg(0.14f, 0.14f, 0.17f, 1.0f);
                        const ImVec4 cardHov(0.17f, 0.17f, 0.21f, 1.0f);
                        const ImVec4 cardSel(0.20f, 0.26f, 0.48f, 1.0f);
                        const ImVec4 border(0.40f, 0.52f, 0.96f, 0.7f);
                        const ImVec4 dim(0.50f, 0.50f, 0.55f, 1.0f);
                        const ImVec4 pill(0.16f, 0.16f, 0.20f, 1.0f);
                        const ImVec4 pillH(0.20f, 0.20f, 0.25f, 1.0f);
                        const ImVec4 pillA(0.22f, 0.32f, 0.56f, 1.0f);

                        // ── Tab bar (full width, dark background) ──
                        ImGui::PushStyleColor(ImGuiCol_ChildBg, tabDef);
                        ImGui::BeginChild("##tabbar", ImVec2(popW, 48), false);
                        {
                            const char* tabs[] = {"Applications", "Screens", "Cameras"};
                            float tabW = popW / 3.0f;
                            for (int i = 0; i < 3; i++) {
                                bool sel = (s_tab == i);
                                ImGui::PushStyleColor(ImGuiCol_Button, sel ? tabSel : tabDef);
                                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, tabSel);
                                ImGui::PushStyleColor(ImGuiCol_Text, sel ? ImVec4(1,1,1,1) : dim);
                                ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0);
                                if (ImGui::Button(tabs[i], ImVec2(tabW, 48))) s_tab = i;
                                ImGui::PopStyleVar();
                                ImGui::PopStyleColor(3);
                                // Accent underline for active tab
                                if (sel) {
                                    ImVec2 p = ImGui::GetItemRectMin();
                                    ImVec2 sz = ImGui::GetItemRectSize();
                                    ImGui::GetWindowDrawList()->AddRectFilled(
                                        ImVec2(p.x, p.y + sz.y - 3), ImVec2(p.x + sz.x, p.y + sz.y),
                                        ImGui::ColorConvertFloat4ToU32(accent));
                                }
                                if (i < 2) ImGui::SameLine(0, 0);
                            }
                        }
                        ImGui::EndChild();
                        ImGui::PopStyleColor();

                        // ── Content area ──
                        ImGui::SetCursorPos(ImVec2(pad, 48 + 12));
                        const float contentH = popH - 48 - 12 - 90;
                        const int cols = 2;
                        const float gap = 12.0f;
                        const float cardW = (cW - gap * (cols - 1)) / cols;
                        const float cardH = 120.0f;
                        const float thumbH = 70.0f;

                        // Draws a selection card at fixed grid position
                        auto drawCard = [&](int idx, int total, bool selected, const char* title, const char* subtitle, float cw, float ch, float th, const char* previewTexId) -> bool {
                            int col = idx % cols;
                            int row = idx / cols;
                            float x = col * (cw + gap);
                            float y = row * (ch + gap);
                            ImGui::SetCursorPos(ImVec2(x, y));

                            ImGui::PushID(idx);
                            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f);
                            ImGui::PushStyleColor(ImGuiCol_Button, selected ? cardSel : cardBg);
                            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, selected ? cardSel : cardHov);
                            ImVec2 screenPos = ImGui::GetCursorScreenPos();
                            bool clicked = ImGui::Button("##card", ImVec2(cw, ch));
                            ImGui::PopStyleColor(2);
                            ImGui::PopStyleVar();

                            if (selected) {
                                ImGui::GetWindowDrawList()->AddRect(screenPos, ImVec2(screenPos.x + cw, screenPos.y + ch),
                                    ImGui::ColorConvertFloat4ToU32(border), 8.0f, 0, 2.0f);
                            }

                            // Thumbnail placeholder
                            ImGui::GetWindowDrawList()->AddRectFilled(
                                ImVec2(screenPos.x + 8, screenPos.y + 8),
                                ImVec2(screenPos.x + cw - 8, screenPos.y + 8 + th),
                                IM_COL32(18, 18, 22, 255), 6.0f);

                            if (previewTexId && previewTexId[0]) {
                                if (auto* thumbSrv = TalkMe::TextureManager::Get().GetTexture(previewTexId)) {
                                    ImGui::GetWindowDrawList()->AddImageRounded(
                                        (ImTextureID)thumbSrv,
                                        ImVec2(screenPos.x + 8, screenPos.y + 8),
                                        ImVec2(screenPos.x + cw - 8, screenPos.y + 8 + th),
                                        ImVec2(0, 0), ImVec2(1, 1), IM_COL32(255, 255, 255, 255), 6.0f);
                                }
                            }

                            if (subtitle && subtitle[0]) {
                                ImGui::GetWindowDrawList()->AddText(
                                    ImVec2(screenPos.x + 14, screenPos.y + th * 0.35f),
                                    IM_COL32(120, 120, 130, 255), subtitle);
                            }

                            // Title below thumbnail
                            std::string disp = title;
                            if (disp.length() > 36) disp = disp.substr(0, 33) + "...";
                            ImGui::GetWindowDrawList()->AddText(
                                ImVec2(screenPos.x + 8, screenPos.y + th + 16),
                                IM_COL32(210, 210, 220, 255), disp.c_str());

                            ImGui::PopID();
                            return clicked;
                        };

                        if (s_tab == 0) {
                            ImGui::BeginChild("##apps", ImVec2(cW, contentH), false);
                            for (int i = 0; i < (int)s_windows.size(); i++) {
                                RECT wr = {};
                                char sub[32] = {};
                                const std::string texId = "sharepick_app_" + std::to_string(reinterpret_cast<uintptr_t>(s_windows[i].hwnd));
                                if (::GetWindowRect(s_windows[i].hwnd, &wr))
                                    std::snprintf(sub, 32, "%dx%d", wr.right - wr.left, wr.bottom - wr.top);
                                if (drawCard(i, (int)s_windows.size(), s_selApp == i,
                                    s_windows[i].title.c_str(), sub, cardW, cardH, thumbH, texId.c_str()))
                                    s_selApp = i;
                            }
                            if (s_windows.empty()) {
                                ImGui::PushStyleColor(ImGuiCol_Text, dim);
                                ImGui::SetCursorPos(ImVec2(cW * 0.25f, contentH * 0.4f));
                                ImGui::Text("No applications found");
                                ImGui::PopStyleColor();
                            }
                            ImGui::EndChild();
                        }
                        else if (s_tab == 1) {
                            ImGui::BeginChild("##screens", ImVec2(cW, contentH), false);
                            int sw = ::GetSystemMetrics(SM_CXSCREEN);
                            int sh = ::GetSystemMetrics(SM_CYSCREEN);
                            char sub[32]; std::snprintf(sub, 32, "%dx%d", sw, sh);
                            drawCard(0, 1, true, "Entire Screen", sub, cW * 0.48f, cardH, thumbH, "sharepick_screen");
                            ImGui::EndChild();
                        }
                        else {
                            ImGui::BeginChild("##cams", ImVec2(cW, contentH), false);
                            for (int i = 0; i < (int)s_cameras.size(); i++) {
                                if (drawCard(i, (int)s_cameras.size(), s_selCam == i,
                                    s_cameras[i].name.c_str(), "Camera", cardW, cardH, thumbH, nullptr))
                                    s_selCam = i;
                            }
                            if (s_cameras.empty()) {
                                ImGui::PushStyleColor(ImGuiCol_Text, dim);
                                ImGui::SetCursorPos(ImVec2(cW * 0.28f, contentH * 0.4f));
                                ImGui::Text("No cameras detected");
                                ImGui::PopStyleColor();
                            }
                            ImGui::EndChild();
                        }

                        // ── Bottom bar: settings + actions ──
                        ImGui::SetCursorPos(ImVec2(pad, popH - 78));
                        ImGui::PushStyleColor(ImGuiCol_Separator, ImVec4(0.2f, 0.2f, 0.24f, 0.5f));
                        ImGui::Separator();
                        ImGui::PopStyleColor();
                        ImGui::Dummy(ImVec2(0, 8));
                        ImGui::SetCursorPosX(pad);

                        auto pbtn = [&](const char* l, bool a, float w) {
                            ImGui::PushStyleColor(ImGuiCol_Button, a ? pillA : pill);
                            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, a ? pillA : pillH);
                            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);
                            bool c = ImGui::Button(l, ImVec2(w, 28));
                            ImGui::PopStyleVar();
                            ImGui::PopStyleColor(2);
                            return c;
                        };

                        // Resolution / FPS / Quality pills
                        if (pbtn("720p##r", s_shareRes==0, 50)) s_shareRes=0; ImGui::SameLine(0,4);
                        if (pbtn("1080p##r", s_shareRes==1, 54)) s_shareRes=1; ImGui::SameLine(0,4);
                        if (pbtn("1440p##r", s_shareRes==2, 54)) s_shareRes=2;
                        ImGui::SameLine(0, 20);
                        if (pbtn("30fps##f", s_shareFps==0, 50)) s_shareFps=0; ImGui::SameLine(0,4);
                        if (pbtn("60fps##f", s_shareFps==1, 50)) s_shareFps=1; ImGui::SameLine(0,4);
                        if (pbtn("120fps##f", s_shareFps==2, 54)) s_shareFps=2;
                        ImGui::SameLine(0, 20);
                        if (pbtn("Low##q", s_shareQuality==0, 40)) s_shareQuality=0; ImGui::SameLine(0,4);
                        if (pbtn("Med##q", s_shareQuality==1, 42)) s_shareQuality=1; ImGui::SameLine(0,4);
                        if (pbtn("High##q", s_shareQuality==2, 42)) s_shareQuality=2;

                        // Cancel / Go Live
                        ImGui::SameLine(cW - 100);
                        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);
                        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.20f, 0.24f, 1));
                        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.26f, 0.26f, 0.30f, 1));
                        if (ImGui::Button("Cancel", ImVec2(0, 28))) ImGui::CloseCurrentPopup();
                        ImGui::PopStyleColor(2);
                        ImGui::SameLine(0, 8);
                        ImGui::PushStyleColor(ImGuiCol_Button, accent);
                        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, accentH);
                        if (ImGui::Button("Go Live", ImVec2(80, 28))) {
                            const int fpsV[] = {30, 60, 120};
                            const int qualV[] = {40, 70, 95};
                            const int rW[] = {1280, 1920, 2560};
                            const int rH[] = {720, 1080, 1440};
                            void* hwnd = nullptr;
                            int sourceType = 1; // 0=application, 1=screen, 2=camera
                            std::string sourceId;
                            if (s_tab == 0 && s_selApp >= 0 && s_selApp < (int)s_windows.size())
                            {
                                hwnd = (void*)s_windows[s_selApp].hwnd;
                                sourceType = 0;
                            }
                            else if (s_tab == 2 && s_selCam >= 0 && s_selCam < (int)s_cameras.size())
                            {
                                sourceType = 2;
                                sourceId = s_cameras[s_selCam].symLink;
                            }
                            if (onStartScreenShare)
                                onStartScreenShare(fpsV[s_shareFps], qualV[s_shareQuality], rW[s_shareRes], rH[s_shareRes], hwnd, sourceType, sourceId);
                            s_refresh = true;
                            ImGui::CloseCurrentPopup();
                        }
                        ImGui::PopStyleColor(2);
                        ImGui::PopStyleVar();

                        ImGui::EndPopup();
                    } else {
                        if (s_popupWasOpen) {
                            TalkMe::TextureManager::Get().EvictTexturesWithPrefixExcept("sharepick_", std::unordered_set<std::string>{});
                            s_windows.clear();
                            s_cameras.clear();
                        }
                        s_popupWasOpen = false;
                        s_refresh = true;
                    }

                    ImGui::PopStyleColor();
                    ImGui::PopStyleVar(2);
                }

                // ====== Games Picker Popup ======
                ImGui::SetNextWindowSize(ImVec2(260, 200), ImGuiCond_Always);
                if (ImGui::BeginPopup("GamesPicker")) {
                    ImGui::Text("Choose a Game");
                    ImGui::Separator();
                    ImGui::Dummy(ImVec2(0, 4));

                    // Chess — submenu with opponent picker
                    if (ImGui::BeginMenu("Chess (2 players)")) {
                        for (const auto& m : voiceMembers) {
                            if (m == currentUser.username) continue;
                            std::string d = m; size_t h = d.find('#'); if (h != std::string::npos) d = d.substr(0, h);
                            if (ImGui::MenuItem(d.c_str())) {
                                nlohmann::json cj; cj["to"] = m; cj["game"] = "chess";
                                netClient.Send(PacketType::Game_Challenge, cj.dump());
                                ImGui::CloseCurrentPopup();
                            }
                        }
                        if (voiceMembers.size() <= 1) ImGui::TextDisabled("No other users");
                        ImGui::EndMenu();
                    }

                    // Racing — submenu with opponent picker
                    if (ImGui::BeginMenu("Car Racing (2 players)")) {
                        for (const auto& m : voiceMembers) {
                            if (m == currentUser.username) continue;
                            std::string d = m; size_t h = d.find('#'); if (h != std::string::npos) d = d.substr(0, h);
                            if (ImGui::MenuItem(d.c_str())) {
                                nlohmann::json cj; cj["to"] = m; cj["game"] = "racing";
                                netClient.Send(PacketType::Game_Challenge, cj.dump());
                                ImGui::CloseCurrentPopup();
                            }
                        }
                        if (voiceMembers.size() <= 1) ImGui::TextDisabled("No other users");
                        ImGui::EndMenu();
                    }

                    ImGui::Separator();

                    // Tic-Tac-Toe — 2 players
                    if (ImGui::BeginMenu("Tic-Tac-Toe (2 players)")) {
                        for (const auto& m : voiceMembers) {
                            if (m == currentUser.username) continue;
                            std::string d = m; size_t h = d.find('#'); if (h != std::string::npos) d = d.substr(0, h);
                            if (ImGui::MenuItem(d.c_str())) {
                                nlohmann::json cj; cj["to"] = m; cj["game"] = "tictactoe";
                                netClient.Send(PacketType::Game_Challenge, cj.dump());
                                ImGui::CloseCurrentPopup();
                            }
                        }
                        if (voiceMembers.size() <= 1) ImGui::TextDisabled("No other users");
                        ImGui::EndMenu();
                    }

                    ImGui::Separator();

                    // Flappy Bird — single player
                    if (ImGui::MenuItem("Flappy Bird (solo)")) {
                        nlohmann::json cj; cj["to"] = currentUser.username; cj["game"] = "flappy";
                        netClient.Send(PacketType::Game_Challenge, cj.dump());
                        ImGui::CloseCurrentPopup();
                    }

                    ImGui::EndPopup();
                }
            }
            // ==================== CINEMA VIEW ====================
            else if (cType == ChannelType::Cinema) {
                ImGui::Dummy(ImVec2(0, 20));
                ImGui::Indent(36);
                ImGui::PushStyleColor(ImGuiCol_Text, Styles::Accent());
                ImGui::SetWindowFontScale(1.3f);
                ImGui::Text("%s", chName.c_str());
                ImGui::SetWindowFontScale(1.0f);
                ImGui::PopStyleColor();

                ImGui::PushStyleColor(ImGuiCol_Text, Styles::TextMuted());
                ImGui::Text("Cinema Channel");
                ImGui::PopStyleColor();
                ImGui::Unindent(36);

                ImGui::Separator();
                ImGui::Dummy(ImVec2(0, 8));

                // Now Playing
                ImGui::Indent(20);
                // Access cinema state through a simple global — Application sets it
                // For now render from what we have in the protocol state
                ImGui::Text("Now Playing:");
                ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.0f, 1.0f), "  Use the controls below to manage the queue");

                ImGui::Dummy(ImVec2(0, 8));

                // Playback controls
                float controlsW = 360.0f;
                float ctrlX = (areaW - controlsW) * 0.5f;
                ImGui::SetCursorPosX(ctrlX);
                ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);

                if (ImGui::Button("Play", ImVec2(60, 30))) {
                    nlohmann::json cj; cj["cid"] = selectedChannelId; cj["action"] = "play";
                    netClient.Send(PacketType::Cinema_Control, cj.dump());
                }
                ImGui::SameLine(0, 6);
                if (ImGui::Button("Pause", ImVec2(60, 30))) {
                    nlohmann::json cj; cj["cid"] = selectedChannelId; cj["action"] = "pause";
                    netClient.Send(PacketType::Cinema_Control, cj.dump());
                }
                ImGui::SameLine(0, 6);
                if (ImGui::Button("-10s", ImVec2(50, 30))) {
                    nlohmann::json cj; cj["cid"] = selectedChannelId; cj["action"] = "seek"; cj["time"] = -10.0f;
                    netClient.Send(PacketType::Cinema_Control, cj.dump());
                }
                ImGui::SameLine(0, 6);
                if (ImGui::Button("+10s", ImVec2(50, 30))) {
                    nlohmann::json cj; cj["cid"] = selectedChannelId; cj["action"] = "seek"; cj["time"] = 10.0f;
                    netClient.Send(PacketType::Cinema_Control, cj.dump());
                }
                ImGui::SameLine(0, 6);
                if (ImGui::Button("Next", ImVec2(55, 30))) {
                    nlohmann::json cj; cj["cid"] = selectedChannelId; cj["action"] = "next";
                    netClient.Send(PacketType::Cinema_Control, cj.dump());
                }
                ImGui::PopStyleVar();

                ImGui::Dummy(ImVec2(0, 16));
                ImGui::Separator();

                // Add to queue
                ImGui::Dummy(ImVec2(0, 8));
                ImGui::Text("Add to Queue:");
                static char s_cinemaAddUrl[512] = "";
                static char s_cinemaAddTitle[128] = "";
                ImGui::PushItemWidth(areaW - 120);
                ImGui::InputTextWithHint("##cinema_add_url", "Video URL (direct link)...", s_cinemaAddUrl, sizeof(s_cinemaAddUrl));
                ImGui::PopItemWidth();
                ImGui::PushItemWidth(areaW - 120);
                ImGui::InputTextWithHint("##cinema_add_title", "Title (optional)...", s_cinemaAddTitle, sizeof(s_cinemaAddTitle));
                ImGui::PopItemWidth();
                ImGui::SameLine();
                if (ImGui::Button("Add", ImVec2(60, 0)) && strlen(s_cinemaAddUrl) > 0) {
                    nlohmann::json qj;
                    qj["cid"] = selectedChannelId;
                    qj["url"] = std::string(s_cinemaAddUrl);
                    qj["title"] = strlen(s_cinemaAddTitle) > 0 ? std::string(s_cinemaAddTitle) : std::string(s_cinemaAddUrl);
                    netClient.Send(PacketType::Cinema_Queue_Add, qj.dump());
                    memset(s_cinemaAddUrl, 0, sizeof(s_cinemaAddUrl));
                    memset(s_cinemaAddTitle, 0, sizeof(s_cinemaAddTitle));
                }

                ImGui::Unindent(20);
            }
            // ==================== TEXT VIEW ====================
            else {
                ImGui::Dummy(ImVec2(0, 20));
                ImGui::Indent(36);
                ImGui::PushStyleColor(ImGuiCol_Text, Styles::TextSecondary());
                ImGui::SetWindowFontScale(1.25f);
                ImGui::Text("# %s", chName.c_str());
                ImGui::SetWindowFontScale(1.0f);
                ImGui::PopStyleColor();

                if (!chDesc.empty()) {
                    ImGui::PushStyleColor(ImGuiCol_Text, Styles::TextMuted());
                    ImGui::TextWrapped("%s", chDesc.c_str());
                    ImGui::PopStyleColor();
                }

                ImGui::PushStyleColor(ImGuiCol_Text, Styles::TextMuted());
                ImGui::Text("Invite: %s", currentServer.inviteCode.c_str());
                ImGui::PopStyleColor();

                ImGui::Unindent(36);

                ImGui::Indent(36);
                if (showSearch) {
                    if (ImGui::SmallButton(*showSearch ? "[X] Search" : "Search"))
                        *showSearch = !*showSearch;
                    ImGui::SameLine();
                }
                if (showMemberList) {
                    if (ImGui::SmallButton(*showMemberList ? "[X] Members" : "Members"))
                        *showMemberList = !*showMemberList;
                    ImGui::SameLine();
                }
                ImGui::SmallButton("Copy Invite");
                if (ImGui::IsItemClicked()) ImGui::SetClipboardText(currentServer.inviteCode.c_str());
                ImGui::Unindent(36);

                ImGui::Dummy(ImVec2(0, 6));
                ImGui::PushStyleColor(ImGuiCol_Separator, Styles::Separator());
                ImGui::Separator();
                ImGui::PopStyleColor();
                ImGui::Dummy(ImVec2(0, 4));

                float hdrH = ImGui::GetCursorPosY();
                float inpH = 120.0f;
                float msgH = winH - hdrH - inpH;
                if (msgH < 80.0f) msgH = 80.0f;

                if (showSearch && *showSearch && searchBuf) {
                    ImGui::Indent(32);
                    ImGui::PushItemWidth(areaW - 100);
                    ImGui::InputTextWithHint("##search", "Search messages...", searchBuf, 256);
                    ImGui::PopItemWidth();
                    ImGui::Unindent(32);
                }

                std::string searchStr = (showSearch && *showSearch && searchBuf) ? searchBuf : "";
                for (auto& c : searchStr) c = (char)std::tolower((unsigned char)c);

                ImGui::SetCursorPosX(32);
                ImGui::BeginChild("Messages", ImVec2(areaW - 64, msgH), false, ImGuiWindowFlags_None);
                {
                    static float s_lastScrollY = 0.0f;
                    float scrollY = ImGui::GetScrollY();
                    if (requestOlderMessages && s_lastScrollY >= 100.0f && scrollY < 100.0f)
                        requestOlderMessages();
                    s_lastScrollY = scrollY;
                }

                // Read-anchor: highest message id that is fully visible in the viewport.
                const ImVec2 winPos = ImGui::GetWindowPos();
                const ImVec2 crMin = ImGui::GetWindowContentRegionMin();
                const ImVec2 crMax = ImGui::GetWindowContentRegionMax();
                const float visMinY = winPos.y + crMin.y;
                const float visMaxY = winPos.y + crMax.y;
                int maxFullyVisibleMid = 0;
                if (loadingOlder && *loadingOlder) {
                    ImGui::PushStyleColor(ImGuiCol_Text, Styles::TextMuted());
                    ImGui::Text("Loading older messages...");
                    ImGui::PopStyleColor();
                    ImGui::Dummy(ImVec2(0, 8));
                }

                // Virtualized rendering: build filtered index, then render only visible + overscan.
                // Per-channel scroll state
                static int  s_scrollToBottomChan = -1;
                static int  s_scrollToBottomFrames = 0;
                static std::map<int, bool>   s_chanWasAtBottom;
                static std::map<int, size_t> s_chanLastMsgCount;

                if (s_lastChannelForGifState != selectedChannelId) {
                    // Do not clear s_ChatGifStates on channel switch: keep state (uploaded, cachedW/H)
                    // so when we switch back, GetAnimatedSrv sees "texture gone" and re-requests/re-uploads.
                    s_lastChannelForGifState = selectedChannelId;
                    // New channel opened — schedule scroll-to-bottom for several frames so the
                    // layout has time to settle before we lock to the end.
                    s_scrollToBottomChan = selectedChannelId;
                    s_scrollToBottomFrames = 3;
                }
                InvalidateMsgHeights(selectedChannelId);
                std::vector<int> renderIdx;
                renderIdx.reserve(messages.size());
                for (int i = 0; i < (int)messages.size(); i++) {
                    const auto& msg = messages[i];
                    if (msg.channelId != selectedChannelId) continue;
                    if (!searchStr.empty()) {
                        std::string lower = msg.content;
                        for (auto& c : lower) c = (char)std::tolower((unsigned char)c);
                        std::string lowerSender = msg.sender;
                        for (auto& c : lowerSender) c = (char)std::tolower((unsigned char)c);
                        if (lower.find(searchStr) == std::string::npos && lowerSender.find(searchStr) == std::string::npos)
                            continue;
                    }
                    renderIdx.push_back(i);
                }

                const float scrollY = ImGui::GetScrollY();
                const float viewH = ImGui::GetWindowHeight();
                const float viewTop = scrollY - TalkMe::Limits::kChatOverscanPx;
                const float viewBot = scrollY + viewH + TalkMe::Limits::kChatOverscanPx;

                float cursorY = 0.f;
                int firstVis = (int)renderIdx.size();
                for (int i = 0; i < (int)renderIdx.size(); i++) {
                    const auto& m = messages[renderIdx[i]];
                    float h = GetEstimatedMsgHeight(m.id);
                    if (cursorY + h >= viewTop) { firstVis = i; break; }
                    cursorY += h;
                }

                if (cursorY > 0.f)
                    ImGui::Dummy(ImVec2(ImGui::GetContentRegionAvail().x, cursorY));

                // Parse each visible message's content once for both first pass and render loop.
                std::vector<std::vector<ContentSegment>> parsedContents(renderIdx.size());
                for (int ri = firstVis; ri < (int)renderIdx.size(); ri++)
                    parsedContents[ri] = ParseMessageContent(messages[renderIdx[ri]].content);

                // First pass: collect all visible image URLs and request them immediately so they
                // load in parallel and are protected from eviction (chat GIFs then render even when
                // not pre-warmed by the picker).
                std::unordered_set<std::string> visibleChatImageUrls;
                if (!gameModeOn) {
                    for (int ri = firstVis; ri < (int)renderIdx.size(); ri++) {
                        const auto& msg = messages[renderIdx[ri]];
                        if (!msg.attachmentId.empty() && mediaBaseUrl && !mediaBaseUrl->empty()) {
                            std::string mediaUrl = *mediaBaseUrl + "/media/" + msg.attachmentId;
                            visibleChatImageUrls.insert(mediaUrl);
                        }
                        for (const auto& s : parsedContents[ri])
                            if (s.isUrl && s.isImage) visibleChatImageUrls.insert(s.text);
                    }
                    auto& imgCache = TalkMe::ImageCache::Get();
                    for (const std::string& url : visibleChatImageUrls) {
                        if (!imgCache.GetImage(url) && !imgCache.IsLoading(url))
                            imgCache.RequestImage(url);
                    }
                    TalkMe::ImageCache::Get().SetProtectedUrls(visibleChatImageUrls);
                }

                std::unordered_set<std::string> visibleTexIds;

                // O(1) reply lookup: build id -> message once so we don't scan all messages per reply.
                std::unordered_map<int, const ChatMessage*> replyMap;
                replyMap.reserve(renderIdx.size());
                for (int i = 0; i < (int)renderIdx.size(); i++) {
                    const auto& m = messages[renderIdx[i]];
                    replyMap[m.id] = &m;
                }

                for (int ri = firstVis; ri < (int)renderIdx.size(); ri++) {
                    const auto& msg = messages[renderIdx[ri]];
                    if (!gameModeOn && !msg.attachmentId.empty()) {
                        visibleTexIds.insert("att_" + msg.attachmentId);
                        if (mediaBaseUrl && !mediaBaseUrl->empty()) {
                            std::string mediaUrl = *mediaBaseUrl + "/media/" + msg.attachmentId;
                            visibleChatImageUrls.insert(mediaUrl);
                        }
                    }
                    const float beforeY = ImGui::GetCursorPosY();
                    const float msgStartY = ImGui::GetCursorScreenPos().y;
                    bool isMe = (msg.sender == currentUser.username);

                    ImGui::BeginGroup();

                    if (msg.replyToId > 0) {
                        auto it = replyMap.find(msg.replyToId);
                        if (it != replyMap.end()) {
                            const ChatMessage& orig = *it->second;
                            std::string replySender = orig.sender;
                            size_t rhp = replySender.find('#');
                            if (rhp != std::string::npos) replySender = replySender.substr(0, rhp);
                            std::string preview = orig.content.substr(0, 60);
                            if (orig.content.size() > 60) preview += "...";
                            ImGui::PushStyleColor(ImGuiCol_Text, Styles::TextMuted());
                            ImGui::Text("  | %s: %s", replySender.c_str(), preview.c_str());
                            ImGui::PopStyleColor();
                        }
                    }

                    ImGui::PushStyleColor(ImGuiCol_Text, isMe ? Styles::Accent() : Styles::Error());
                    size_t hp = msg.sender.find('#');
                    std::string dispName = hp != std::string::npos ? msg.sender.substr(0, hp) : msg.sender;
                    std::string tag = hp != std::string::npos ? msg.sender.substr(hp) : "";
                    ImGui::Text("%s", dispName.c_str());
                    if (!tag.empty()) {
                        ImGui::SameLine(0, 0);
                        ImGui::PushStyleColor(ImGuiCol_Text, Styles::TextMuted());
                        ImGui::Text("%s", tag.c_str());
                        ImGui::PopStyleColor();
                    }
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Click to copy username");
                    if (ImGui::IsItemClicked()) ImGui::SetClipboardText(msg.sender.c_str());
                    ImGui::PopStyleColor();

                    ImGui::SameLine();
                    // Correct common server typo: 2006 -> 2026 for display
                    if (msg.timestamp.size() >= 4 && msg.timestamp.compare(0, 4, "2006") == 0) {
                        std::string tsFixed = "2026" + msg.timestamp.substr(4);
                        ImGui::TextDisabled("%s", tsFixed.c_str());
                    } else {
                        ImGui::TextDisabled("%s", msg.timestamp.c_str());
                    }
                    if (msg.pinned) {
                        ImGui::SameLine();
                        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "[pinned]");
                    }

                    // Inline attachment (TCP Media_Request path first; HTTP fallback if no TCP handlers) — skipped in game mode
                    if (!msg.attachmentId.empty()) {
                        if (gameModeOn) {
                            ImGui::PushStyleColor(ImGuiCol_Text, Styles::TextMuted());
                            ImGui::Text("[Image]");
                            ImGui::PopStyleColor();
                        }
                        else {
                            std::string mediaUrl = (mediaBaseUrl && !mediaBaseUrl->empty())
                                ? (*mediaBaseUrl + "/media/" + msg.attachmentId) : std::string();
                            bool drawn = false;
                            if (requestAttachment && getAttachmentDisplay) {
                                (*requestAttachment)(msg.attachmentId);
                                const TalkMe::AttachmentDisplay* att = (*getAttachmentDisplay)(msg.attachmentId);
                                if (att && att->ready && !att->textureId.empty()) {
                                    auto& tm = TalkMe::TextureManager::Get();
                                    auto* srv = tm.GetTexture(att->textureId);
                                    if (srv && att->width > 0 && att->height > 0) {
                                        const float availX = ImGui::GetContentRegionAvail().x;
                                        float maxW = (std::min)(areaW * 0.5f, availX > 0.f ? availX : areaW * 0.5f);
                                        float maxH = 300.0f;
                                        float imgW = (float)att->width;
                                        float imgH = (float)att->height;
                                        if (imgW > maxW) { imgH *= maxW / imgW; imgW = maxW; }
                                        if (imgH > maxH) { imgW *= maxH / imgH; imgH = maxH; }
                                        if (imgW > 0.f && imgH > 0.f)
                                            ImGui::Image((ImTextureID)srv, ImVec2(imgW, imgH));
                                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Click to view");
                                        if (ImGui::IsItemClicked() && onAttachmentClick)
                                            (*onAttachmentClick)(msg.attachmentId);
                                        drawn = true;
                                    }
                                    else if (onAttachmentTextureEvicted && *onAttachmentTextureEvicted) {
                                        // Texture was evicted (e.g. scrolled away); notify app so it clears cache,
                                        // then fall through to HTTP/ImageCache path to re-load and show.
                                        (*onAttachmentTextureEvicted)(msg.attachmentId);
                                    }
                                }
                                else if (att && att->failed) {
                                    ImGui::PushStyleColor(ImGuiCol_Text, Styles::TextMuted());
                                    ImGui::Text("[Attachment failed to load]");
                                    ImGui::PopStyleColor();
                                    drawn = true;
                                }
                                else {
                                    float skW = (std::min)(kSkeletonDefaultW, areaW * 0.5f);
                                    DrawImageSkeleton(skW, kSkeletonDefaultH);
                                    drawn = true;
                                }
                            }
                            if (!drawn && !mediaUrl.empty()) {
                                auto& imgCache = TalkMe::ImageCache::Get();
                                if (!imgCache.GetImage(mediaUrl) && !imgCache.IsLoading(mediaUrl))
                                    imgCache.RequestImage(mediaUrl);
                                const std::string texId = "att_" + msg.attachmentId;
                                auto* srv = GetAnimatedSrv(mediaUrl, texId, ImGui::GetTime(), onAttachmentTextureEvicted);
                                if (srv) {
                                    auto& st = s_ChatGifStates[texId];
                                    const float availX = ImGui::GetContentRegionAvail().x;
                                    float maxW = (std::min)(areaW * 0.5f, availX > 0.f ? availX : areaW * 0.5f);
                                    float maxH = 300.0f;
                                    float fw = (float)st.width;
                                    float fh = (float)st.height;
                                    if (fw > maxW) { fh *= maxW / fw; fw = maxW; }
                                    if (fh > maxH) { fw *= maxH / fh; fh = maxH; }
                                    if (fw > 0.f && fh > 0.f) {
                                        ImGui::Image((ImTextureID)srv, ImVec2(fw, fh));
                                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Click to view");
                                        if (ImGui::IsItemClicked() && onAttachmentClick)
                                            (*onAttachmentClick)(msg.attachmentId);
                                    }
                                    drawn = true;
                                }
                                else {
                                    auto cachedOpt = imgCache.GetImageCopy(mediaUrl);
                                    const TalkMe::CachedImage* cached = cachedOpt ? &*cachedOpt : nullptr;
                                    if (cached && cached->failed) {
                                        ImGui::PushStyleColor(ImGuiCol_Text, Styles::TextMuted());
                                        ImGui::Text("[Attachment failed to load]");
                                        ImGui::PopStyleColor();
                                        drawn = true;
                                    }
                                    else if (!drawn) {
                                        // Skeleton placeholder while texture is loading or re-uploading after eviction.
                                        const std::string texId2 = "att_" + msg.attachmentId;
                                        const auto& gifSt = s_ChatGifStates[texId2];
                                        if (gifSt.cachedW > 0 && gifSt.cachedH > 0) {
                                            const float availX = ImGui::GetContentRegionAvail().x;
                                            float maxW = (std::min)(areaW * 0.5f, availX > 0.f ? availX : areaW * 0.5f);
                                            float maxH = 300.0f;
                                            float fw = (float)gifSt.cachedW, fh = (float)gifSt.cachedH;
                                            if (fw > maxW) { fh *= maxW / fw; fw = maxW; }
                                            if (fh > maxH) { fw *= maxH / fh; fh = maxH; }
                                            if (fw > 0.f && fh > 0.f) DrawImageSkeleton(fw, fh);
                                        }
                                        else {
                                            float skW = (std::min)(kSkeletonDefaultW, areaW * 0.5f);
                                            DrawImageSkeleton(skW, kSkeletonDefaultH);
                                        }
                                    }
                                }
                            }
                            // Fallback: attachment but nothing drawn (e.g. no media URL, or texture evicted and re-fetch pending)
                            if (!drawn && !msg.attachmentId.empty()) {
                                float skW = (std::min)(kSkeletonDefaultW, areaW * 0.5f);
                                DrawImageSkeleton(skW, kSkeletonDefaultH);
                            }
                        }
                    }

                    {
                        auto renderTextWithMentions = [](const std::string& t) {
                            size_t p = 0;
                            while (p < t.size()) {
                                size_t atPos = t.find('@', p);
                                if (atPos == std::string::npos) {
                                    if (p < t.size()) {
                                        ImGui::PushStyleColor(ImGuiCol_Text, Styles::TextPrimary());
                                        ImGui::TextWrapped("%s", t.substr(p).c_str());
                                        ImGui::PopStyleColor();
                                    }
                                    break;
                                }
                                if (atPos > p) {
                                    ImGui::PushStyleColor(ImGuiCol_Text, Styles::TextPrimary());
                                    ImGui::TextWrapped("%s", t.substr(p, atPos - p).c_str());
                                    ImGui::PopStyleColor();
                                }
                                size_t mentionEnd = t.find_first_of(" \t\n\r", atPos);
                                if (mentionEnd == std::string::npos) mentionEnd = t.size();
                                std::string mention = t.substr(atPos, mentionEnd - atPos);
                                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.6f, 1.0f, 1.0f));
                                ImGui::TextWrapped("%s", mention.c_str());
                                ImGui::PopStyleColor();
                                p = mentionEnd;
                            }
                        };
                        bool hasUrl = false;
                        for (const auto& seg : parsedContents[ri]) {
                            if (!seg.isUrl) {
                                if (!seg.text.empty()) renderTextWithMentions(seg.text);
                                continue;
                            }
                            hasUrl = true;
                            const std::string& url = seg.text;
                            if (!seg.isImage) {
                                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.3f, 0.6f, 1.0f, 1.0f));
                                ImGui::TextWrapped("%s", url.c_str());
                                ImGui::PopStyleColor();
                                if (ImGui::IsItemHovered()) {
                                    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                                    ImGui::SetTooltip("Click to open in browser");
                                }
                                if (ImGui::IsItemClicked())
                                    ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                                std::string lower = url;
                                for (auto& c : lower) c = (char)std::tolower((unsigned char)c);
                                bool isYouTube = !gameModeOn && (lower.find("youtube.com/watch") != std::string::npos ||
                                    lower.find("youtu.be/") != std::string::npos);
                                if (isYouTube) {
                                    std::string videoId;
                                    size_t vPos = url.find("v=");
                                    if (vPos != std::string::npos) {
                                        videoId = url.substr(vPos + 2);
                                        size_t ampPos = videoId.find('&');
                                        if (ampPos != std::string::npos) videoId = videoId.substr(0, ampPos);
                                    }
                                    else {
                                        size_t slashPos = url.find("youtu.be/");
                                        if (slashPos != std::string::npos) {
                                            videoId = url.substr(slashPos + 9);
                                            size_t qPos = videoId.find('?');
                                            if (qPos != std::string::npos) videoId = videoId.substr(0, qPos);
                                        }
                                    }
                                    if (!videoId.empty()) {
                                        std::string thumbUrl = "https://img.youtube.com/vi/" + videoId + "/mqdefault.jpg";
                                        auto& imgCache = TalkMe::ImageCache::Get();
                                        auto cachedOpt = imgCache.GetImageCopy(thumbUrl);
                                        if (!cachedOpt && !imgCache.IsLoading(thumbUrl))
                                            imgCache.RequestImage(thumbUrl);
                                        const TalkMe::CachedImage* cached = cachedOpt ? &*cachedOpt : nullptr;
                                        const float messagesW = (areaW - 64.0f) - 24.0f;
                                        const float ytMaxW = (std::min)(340.0f, messagesW);
                                        const float ytMaxH = 200.0f * (ytMaxW / 340.0f);
                                        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.12f, 0.12f, 0.15f, 1.0f));
                                        ImGui::BeginChild(("yt_" + videoId).c_str(), ImVec2(ytMaxW, ytMaxH), true, ImGuiWindowFlags_NoScrollbar);
                                        const float imgW = ImGui::GetContentRegionAvail().x;
                                        const float imgH = ImGui::GetContentRegionAvail().y;
                                        if (cached && cached->ready && cached->width > 0) {
                                            auto& tm = TalkMe::TextureManager::Get();
                                            std::string texId = "yt_" + videoId;
                                            auto* srv = tm.GetTexture(texId);
                                            if (!srv) srv = tm.LoadFromRGBA(texId, cached->data.data(), cached->width, cached->height, false, cached->data.size());
                                            if (srv) ImGui::Image((ImTextureID)srv, ImVec2(imgW, imgH));
                                        }
                                        else {
                                            DrawImageSkeleton(imgW, imgH);
                                        }
                                        ImGui::EndChild();
                                        ImGui::PopStyleColor();
                                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Click to watch on YouTube");
                                        if (ImGui::IsItemClicked())
                                            ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                                    }
                                }
                                continue;
                            }
                            // Inline image (seg.isImage) — skip rendering in game mode
                            if (gameModeOn) {
                                ImGui::PushStyleColor(ImGuiCol_Text, Styles::TextMuted());
                                ImGui::Text("[Image]");
                                ImGui::PopStyleColor();
                                continue;
                            }
                            visibleTexIds.insert("img_" + url);
                            visibleChatImageUrls.insert(url);
                            auto& imgCache = TalkMe::ImageCache::Get();
                            if (!imgCache.GetImage(url) && !imgCache.IsLoading(url))
                                imgCache.RequestImage(url);
                            const std::string texId = "img_" + url;
                            auto* srv = GetAnimatedSrv(url, texId, ImGui::GetTime(), nullptr);
                            if (srv) {
                                auto& st = s_ChatGifStates[texId];
                                const float availX = ImGui::GetContentRegionAvail().x;
                                float maxW = (std::min)(areaW * 0.5f, availX > 0.f ? availX : areaW * 0.5f);
                                float maxH = 300.0f;
                                float fw = (float)st.width;
                                float fh = (float)st.height;
                                if (fw > maxW) { fh *= maxW / fw; fw = maxW; }
                                if (fh > maxH) { fw *= maxH / fh; fh = maxH; }
                                if (fw > 0.f && fh > 0.f) {
                                    ImGui::Image((ImTextureID)srv, ImVec2(fw, fh));
                                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Click to open full size");
                                    if (ImGui::IsItemClicked())
                                        ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                                }
                            }
                            else {
                                auto cachedOpt = imgCache.GetImageCopy(url);
                                const TalkMe::CachedImage* cached = cachedOpt ? &*cachedOpt : nullptr;
                                if (cached && cached->failed) {
                                    ImGui::PushStyleColor(ImGuiCol_Text, Styles::TextMuted());
                                    ImGui::Text("[Image failed to load]");
                                    ImGui::PopStyleColor();
                                }
                                else {
                                    const auto& gifSt = s_ChatGifStates["img_" + url];
                                    if (gifSt.cachedW > 0 && gifSt.cachedH > 0) {
                                        const float availX = ImGui::GetContentRegionAvail().x;
                                        float maxW = (std::min)(areaW * 0.5f, availX > 0.f ? availX : areaW * 0.5f);
                                        float maxH = 300.0f;
                                        float fw = (float)gifSt.cachedW, fh = (float)gifSt.cachedH;
                                        if (fw > maxW) { fh *= maxW / fw; fw = maxW; }
                                        if (fh > maxH) { fw *= maxH / fh; fh = maxH; }
                                        if (fw > 0.f && fh > 0.f) DrawImageSkeleton(fw, fh);
                                    }
                                    else {
                                        float skW = (std::min)(kSkeletonDefaultW, areaW * 0.5f);
                                        DrawImageSkeleton(skW, kSkeletonDefaultH);
                                    }
                                }
                            }
                        }
                    }

                    if (!msg.reactions.empty()) {
                        for (const auto& [emoji, users] : msg.reactions) {
                            bool iReacted = std::find(users.begin(), users.end(), currentUser.username) != users.end();
                            if (iReacted) ImGui::PushStyleColor(ImGuiCol_Button, Styles::Accent());
                            char label[64];
                            snprintf(label, sizeof(label), "%s %d##r_%s_%d", emoji.c_str(), (int)users.size(), emoji.c_str(), msg.id);
                            if (ImGui::SmallButton(label)) {
                                nlohmann::json rj;
                                rj["mid"] = msg.id; rj["emoji"] = emoji; rj["cid"] = selectedChannelId;
                                netClient.Send(iReacted ? PacketType::Remove_Reaction : PacketType::Add_Reaction, rj.dump());
                            }
                            if (ImGui::IsItemHovered()) {
                                std::string tip;
                                for (size_t ui = 0; ui < users.size() && ui < 5; ui++) {
                                    if (ui > 0) tip += ", ";
                                    std::string d = users[ui];
                                    size_t hp2 = d.find('#');
                                    if (hp2 != std::string::npos) d = d.substr(0, hp2);
                                    tip += d;
                                }
                                if (users.size() > 5) tip += " + " + std::to_string(users.size() - 5) + " more";
                                ImGui::SetTooltip("%s", tip.c_str());
                            }
                            if (iReacted) ImGui::PopStyleColor();
                            ImGui::SameLine();
                        }
                        ImGui::NewLine();
                    }

                    ImGui::EndGroup();

                    if (msg.id > 0) {
                        static int s_editingMsgId = 0;
                        static char s_editBuf[1024] = "";

                        if (s_editingMsgId == msg.id) {
                            ImGui::PushItemWidth(areaW - 180);
                            bool submitted = ImGui::InputText(("##edit_" + std::to_string(msg.id)).c_str(),
                                s_editBuf, sizeof(s_editBuf), ImGuiInputTextFlags_EnterReturnsTrue);
                            ImGui::PopItemWidth();
                            ImGui::SameLine();
                            if (ImGui::SmallButton("Save") || submitted) {
                                if (strlen(s_editBuf) > 0) {
                                    nlohmann::json ej;
                                    ej["mid"] = msg.id; ej["cid"] = selectedChannelId; ej["msg"] = std::string(s_editBuf);
                                    netClient.Send(PacketType::Edit_Message_Request, ej.dump());
                                }
                                s_editingMsgId = 0;
                            }
                            ImGui::SameLine();
                            if (ImGui::SmallButton("Cancel")) s_editingMsgId = 0;
                        }

                        if (ImGui::BeginPopupContextItem(("msg_" + std::to_string(msg.id)).c_str())) {
                            if (replyingToMessageId && ImGui::Selectable("Reply"))
                                *replyingToMessageId = msg.id;

                            ImGui::Separator();
                            ImGui::Text("React:");
                            ImGui::SameLine();
                            const char* quickEmojis[] = { "+1", "<3", ":)", "eyes", "fire", "GG" };
                            for (int ei = 0; ei < 6; ei++) {
                                if (ei > 0) ImGui::SameLine();
                                char eid[32];
                                snprintf(eid, sizeof(eid), "%s##qr%d", quickEmojis[ei], ei);
                                if (ImGui::SmallButton(eid)) {
                                    nlohmann::json rj;
                                    rj["mid"] = msg.id; rj["emoji"] = quickEmojis[ei]; rj["cid"] = selectedChannelId;
                                    netClient.Send(PacketType::Add_Reaction, rj.dump());
                                    ImGui::CloseCurrentPopup();
                                }
                            }
                            ImGui::Separator();

                            if (ImGui::Selectable(msg.pinned ? "Unpin Message" : "Pin Message")) {
                                nlohmann::json pj;
                                pj["mid"] = msg.id; pj["cid"] = selectedChannelId; pj["pin"] = !msg.pinned;
                                netClient.Send(PacketType::Pin_Message_Request, pj.dump());
                            }
                            if (isMe && ImGui::Selectable("Edit Message")) {
                                s_editingMsgId = msg.id;
                                strncpy_s(s_editBuf, msg.content.c_str(), sizeof(s_editBuf) - 1);
                            }
                            if (isMe && ImGui::Selectable("Delete Message"))
                                netClient.Send(PacketType::Delete_Message_Request,
                                    PacketHandler::CreateDeleteMessagePayload(msg.id, selectedChannelId, currentUser.username));
                            ImGui::EndPopup();
                        }
                    }
                    ImGui::Dummy(ImVec2(0, 8));

                    const float afterY = ImGui::GetCursorPosY();
                    RecordMsgHeight(msg.id, afterY - beforeY);
                    if (msg.id > 0 && afterY <= scrollY + viewH) {
                        if (msg.id > maxFullyVisibleMid) maxFullyVisibleMid = msg.id;
                    }
                    cursorY = afterY;
                    if (cursorY > viewBot) {
                        float remaining = 0.f;
                        for (int j = ri + 1; j < (int)renderIdx.size(); j++)
                            remaining += GetEstimatedMsgHeight(messages[renderIdx[j]].id);
                        if (remaining > 0.f)
                            ImGui::Dummy(ImVec2(ImGui::GetContentRegionAvail().x, remaining));
                        break;
                    }
                }

                {
                    static int s_evictionTick = 0;
                    ++s_evictionTick;
                    if (s_evictionTick % TalkMe::Limits::kEvictionIntervalFrames == 0) {
                        TalkMe::TextureManager::Get().EvictTexturesWithPrefixExcept("att_", visibleTexIds);
                        TalkMe::TextureManager::Get().EvictTexturesWithPrefixExcept("img_", visibleTexIds);
                    }
                }
                // Protected URLs are set once in the first pass (above); do not overwrite with the
                // smaller render set here, so cache entries for the visible+overscan range stay
                // resident and images re-appear when scrolling back (only GPU texture is re-uploaded).

                // ---- Scroll management ----
                {
                    const float curScrollY = ImGui::GetScrollY();
                    const float maxScrollY = ImGui::GetScrollMaxY();
                    const bool  isAtBottom = (maxScrollY <= 0.0f || curScrollY >= maxScrollY - 4.0f);

                    // Auto-scroll when new messages arrive if the user was already at the bottom
                    const bool  wasBotLast = s_chanWasAtBottom.count(selectedChannelId)
                        ? s_chanWasAtBottom[selectedChannelId] : true;
                    const size_t lastCnt = s_chanLastMsgCount.count(selectedChannelId)
                        ? s_chanLastMsgCount[selectedChannelId] : 0;

                    if (renderIdx.size() > lastCnt && wasBotLast) {
                        s_scrollToBottomChan = selectedChannelId;
                        s_scrollToBottomFrames = 2;
                    }

                    s_chanWasAtBottom[selectedChannelId] = isAtBottom;
                    s_chanLastMsgCount[selectedChannelId] = renderIdx.size();

                    // Apply scheduled scroll-to-bottom
                    if (s_scrollToBottomChan == selectedChannelId && s_scrollToBottomFrames > 0) {
                        ImGui::SetScrollHereY(1.0f);
                        --s_scrollToBottomFrames;
                    }
                }

                if (onReadAnchorAdvanced && maxFullyVisibleMid > 0) {
                    static std::map<int, int> s_lastReportedByCid;
                    static double s_lastReportTime = 0.0;
                    const double now = ImGui::GetTime();
                    const int last = s_lastReportedByCid[selectedChannelId];
                    if (maxFullyVisibleMid > last && (now - s_lastReportTime) > 0.25) {
                        s_lastReportedByCid[selectedChannelId] = maxFullyVisibleMid;
                        s_lastReportTime = now;
                        onReadAnchorAdvanced(selectedChannelId, maxFullyVisibleMid);
                    }
                }
                ImGui::EndChild();

                // Typing indicator
                {
                    std::string typingText;
                    if (typingUsers) {
                        float now = (float)ImGui::GetTime();
                        std::vector<std::string> active;
                        for (const auto& [user, ts] : *typingUsers) {
                            if (now - ts < 4.0f) {
                                std::string disp = user;
                                size_t hp = user.find('#');
                                if (hp != std::string::npos) disp = user.substr(0, hp);
                                active.push_back(disp);
                            }
                        }
                        if (active.size() == 1) typingText = active[0] + " is typing...";
                        else if (active.size() == 2) typingText = active[0] + " and " + active[1] + " are typing...";
                        else if (active.size() > 2) typingText = std::to_string(active.size()) + " people are typing...";
                    }
                    ImGui::Indent(32);
                    if (!typingText.empty()) {
                        ImGui::PushStyleColor(ImGuiCol_Text, Styles::TextMuted());
                        ImGui::Text("%s", typingText.c_str());
                        ImGui::PopStyleColor();
                    }
                    else {
                        ImGui::Dummy(ImVec2(0, ImGui::GetTextLineHeight()));
                    }
                    ImGui::Unindent(32);
                }

                // GIF/Emotions panel: 30% of content area, right-aligned; close when clicking outside (disabled in game mode)
                if (!gameModeOn && showGifPicker && renderGifPanel) {
                    ImVec2 vpPos = ImGui::GetMainViewport()->Pos;
                    ImVec2 vpSize = ImGui::GetMainViewport()->Size;
                    float contentWidth = vpSize.x - Styles::MainContentLeftOffset;
                    float panelW = contentWidth * 0.30f;
                    if (panelW < 200.0f) panelW = 200.0f;
                    ImVec2 panelMin = ImVec2(vpPos.x + vpSize.x - panelW, vpPos.y);
                    ImVec2 panelSize = ImVec2(panelW, vpSize.y);

                    // Click outside panel (e.g. in chat area or sidebar) closes it
                    if (*showGifPicker && ImGui::IsMouseClicked(0)) {
                        ImVec2 m = ImGui::GetMousePos();
                        bool insidePanel = (m.x >= panelMin.x && m.x < panelMin.x + panelSize.x &&
                            m.y >= panelMin.y && m.y < panelMin.y + panelSize.y);
                        if (!insidePanel) {
                            *showGifPicker = false;
                            if (ImGui::IsPopupOpen("GifPickerPopup"))
                                ImGui::CloseCurrentPopup();
                        }
                    }

                    if (*showGifPicker && !ImGui::IsPopupOpen("GifPickerPopup")) {
                        ImGui::SetNextWindowPos(panelMin, ImGuiCond_Always, ImVec2(0.0f, 0.0f));
                        ImGui::SetNextWindowSize(panelSize, ImGuiCond_Always);
                        ImGui::OpenPopup("GifPickerPopup");
                    }
                    if (!ImGui::IsPopupOpen("GifPickerPopup"))
                        *showGifPicker = false;
                    else if (ImGui::BeginPopup("GifPickerPopup", ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar)) {
                        renderGifPanel(ImGui::GetContentRegionAvail().x, ImGui::GetContentRegionAvail().y);
                        ImGui::EndPopup();
                    }
                }

                // Reply bar (if replying)
                if (replyingToMessageId && *replyingToMessageId > 0) {
                    ImGui::Indent(32);
                    ImGui::PushStyleColor(ImGuiCol_Text, Styles::Accent());
                    for (const auto& orig : messages) {
                        if (orig.id == *replyingToMessageId) {
                            std::string rn = orig.sender;
                            size_t rhp = rn.find('#');
                            if (rhp != std::string::npos) rn = rn.substr(0, rhp);
                            ImGui::Text("Replying to %s", rn.c_str());
                            break;
                        }
                    }
                    ImGui::PopStyleColor();
                    ImGui::SameLine();
                    ImGui::PushStyleColor(ImGuiCol_Text, Styles::TextMuted());
                    if (ImGui::SmallButton("X")) *replyingToMessageId = 0;
                    ImGui::PopStyleColor();
                    ImGui::Unindent(32);
                }

                // Attached GIF/media chip with preview (above input bar)
                constexpr float kComposePreviewMax = 96.f;
                if (attachedGifUrl && !attachedGifUrl->empty()) {
                    ImGui::Indent(32);
                    const std::string& gifUrl = *attachedGifUrl;
                    if (!TalkMe::ImageCache::Get().GetImage(gifUrl) && !TalkMe::ImageCache::Get().IsLoading(gifUrl))
                        TalkMe::ImageCache::Get().RequestImage(gifUrl);
                    const std::string composeGifTexId = "compose_gif";
                    ID3D11ShaderResourceView* gifSrv = GetAnimatedSrv(gifUrl, composeGifTexId, ImGui::GetTime(), nullptr);
                    if (gifSrv) {
                        const auto& st = s_ChatGifStates[composeGifTexId];
                        float fw = (float)st.width, fh = (float)st.height;
                        if (fw > 0.f && fh > 0.f) {
                            if (fw > kComposePreviewMax) { fh *= kComposePreviewMax / fw; fw = kComposePreviewMax; }
                            if (fh > kComposePreviewMax) { fw *= kComposePreviewMax / fh; fh = kComposePreviewMax; }
                            ImGui::Image((ImTextureID)gifSrv, ImVec2(fw, fh));
                            ImGui::SameLine();
                        }
                    }
                    else {
                        const auto& gifSt = s_ChatGifStates[composeGifTexId];
                        if (gifSt.cachedW > 0 && gifSt.cachedH > 0) {
                            float fw = (float)gifSt.cachedW, fh = (float)gifSt.cachedH;
                            if (fw > kComposePreviewMax) { fh *= kComposePreviewMax / fw; fw = kComposePreviewMax; }
                            if (fh > kComposePreviewMax) { fw *= kComposePreviewMax / fh; fh = kComposePreviewMax; }
                            DrawImageSkeleton(fw, fh);
                            ImGui::SameLine();
                        }
                    }
                    ImGui::PushStyleColor(ImGuiCol_Text, Styles::TextSecondary());
                    ImGui::Text("GIF attached");
                    ImGui::PopStyleColor();
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Remove##attached_gif")) {
                        attachedGifUrl->clear();
                    }
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Remove attachment");
                    ImGui::Unindent(32);
                }
                // Attached image chip with preview (paste or drag-and-drop; sent when user clicks Send)
                if (attachedImageFilename && !attachedImageFilename->empty() && onClearAttachedImage) {
                    ImGui::Indent(32);
                    auto* imgSrv = TalkMe::TextureManager::Get().GetTexture("compose_attached");
                    if (imgSrv) {
                        int cw = TalkMe::TextureManager::Get().GetWidth("compose_attached");
                        int ch = TalkMe::TextureManager::Get().GetHeight("compose_attached");
                        float fw = (float)cw, fh = (float)ch;
                        if (fw > 0.f && fh > 0.f) {
                            if (fw > kComposePreviewMax) { fh *= kComposePreviewMax / fw; fw = kComposePreviewMax; }
                            if (fh > kComposePreviewMax) { fw *= kComposePreviewMax / fh; fh = kComposePreviewMax; }
                            ImGui::Image((ImTextureID)imgSrv, ImVec2(fw, fh));
                            ImGui::SameLine();
                        }
                    }
                    ImGui::PushStyleColor(ImGuiCol_Text, Styles::TextSecondary());
                    ImGui::Text("Image attached");
                    ImGui::PopStyleColor();
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Remove##attached_img")) {
                        onClearAttachedImage();
                    }
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Remove attachment");
                    ImGui::Unindent(32);
                }

                // Drag-over overlay: show when user is dragging files over the window (OLE IDropTarget)
                if (showDragOverlay) {
                    ImVec2 overlayPos = ImGui::GetWindowPos();
                    ImVec2 overlaySize(areaW, winH);
                    ImGui::SetNextWindowPos(overlayPos);
                    ImGui::SetNextWindowSize(overlaySize);
                    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.1f, 0.1f, 0.15f, 0.92f));
                    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 12.0f);
                    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24, 24));
                    ImGui::Begin("##DragOverlay", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoInputs);
                    const size_t kMaxMb = kMaxUploadBytes / (1024 * 1024);
                    ImGui::SetCursorPosY(ImGui::GetWindowHeight() * 0.5f - 30);
                    ImGui::PushStyleColor(ImGuiCol_Text, Styles::Accent());
                    ImGui::SetWindowFontScale(1.4f);
                    ImGui::TextUnformatted("Drop images here");
                    ImGui::SetWindowFontScale(1.0f);
                    ImGui::PopStyleColor();
                    ImGui::PushStyleColor(ImGuiCol_Text, Styles::TextMuted());
                    ImGui::Text("Max %zu MB. Supported: PNG, JPG, GIF, BMP, WebP.", kMaxMb);
                    ImGui::PopStyleColor();
                    ImGui::End();
                    ImGui::PopStyleVar(2);
                    ImGui::PopStyleColor();
                }

                // Input bar
                ImGui::Indent(32);
                ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f);
                ImGui::PushStyleColor(ImGuiCol_FrameBg, Styles::ButtonSubtle());
                float inputW = areaW - 240;
                ImGui::PushItemWidth(inputW);
                bool enter = ImGui::InputText("##chat_in", chatInputBuf, 1024, ImGuiInputTextFlags_EnterReturnsTrue);
                ImGui::PopItemWidth();

                // Ctrl+V paste image from clipboard — attach to bar (don't send until user clicks Send)
                if (ImGui::IsItemActive() && ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_V) &&
                    selectedChannelId != -1 && OpenClipboard(nullptr)) {
                    bool clipboardClosed = false;
                    HANDLE hDib = GetClipboardData(CF_DIB);
                    if (hDib) {
                        void* pDib = GlobalLock(hDib);
                        if (pDib) {
                            const BITMAPINFOHEADER* bi = static_cast<const BITMAPINFOHEADER*>(pDib);
                            SIZE_T dibLen = GlobalSize(hDib);
                            DWORD headerSize = bi->biSize;
                            DWORD colorTableSize = 0;
                            if (bi->biBitCount <= 8)
                                colorTableSize = (bi->biClrUsed ? bi->biClrUsed : (1u << bi->biBitCount)) * 4u;
                            // BI_BITFIELDS (3): 16/32 bpp have 3 DWORD masks between header and pixel data
                            DWORD dataOffsetInDib = headerSize + colorTableSize;
                            if (bi->biCompression == 3u && (bi->biBitCount == 16 || bi->biBitCount == 32))
                                dataOffsetInDib += 12u;
                            DWORD offBits = 14u + dataOffsetInDib;
                            if (offBits <= dibLen && dibLen <= 10u * 1024u * 1024u && onAttachImage) {
                                std::vector<uint8_t> bmp;
                                bmp.reserve(14 + dibLen);
                                BITMAPFILEHEADER bf = {};
                                bf.bfType = 0x4D42;
                                bf.bfSize = 14u + static_cast<DWORD>(dibLen);
                                bf.bfOffBits = offBits;
                                bmp.insert(bmp.end(), reinterpret_cast<uint8_t*>(&bf), reinterpret_cast<uint8_t*>(&bf) + sizeof(bf));
                                bmp.insert(bmp.end(), static_cast<const uint8_t*>(pDib), static_cast<const uint8_t*>(pDib) + dibLen);
                                GlobalUnlock(hDib);
                                CloseClipboard();
                                clipboardClosed = true;
                                (*onAttachImage)(std::move(bmp), "paste.bmp");
                            }
                            else {
                                GlobalUnlock(hDib);
                            }
                        }
                    }
                    if (!clipboardClosed) CloseClipboard();
                }

                if (ImGui::IsItemActive() && strlen(chatInputBuf) > 0 && onUserTyping)
                    onUserTyping();

                if (!gameModeOn) {
                    ImGui::SameLine();
                    if (ImGui::Button("Emoji", ImVec2(48, 32))) {
                        *showGifPicker = true;
                    }
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Emotions (GIF & Emoji)");
                }
                ImGui::SameLine();
                if (UI::AccentButton("Send", ImVec2(60, 32)) || enter) {
                    bool hasText = strlen(chatInputBuf) > 0;
                    bool hasGifAttachment = attachedGifUrl && !attachedGifUrl->empty();
                    bool hasImageAttachment = attachedImageFilename && !attachedImageFilename->empty();
                    if (hasImageAttachment && onSendWithAttachedImage) {
                        // Send attached image (and optional caption); upload runs async, message sent when upload completes
                        int replyTo = (replyingToMessageId && *replyingToMessageId > 0) ? *replyingToMessageId : 0;
                        (*onSendWithAttachedImage)(std::string(chatInputBuf), replyTo);
                        memset(chatInputBuf, 0, 1024);
                        if (replyingToMessageId) *replyingToMessageId = 0;
                        if (attachedGifUrl) attachedGifUrl->clear();
                        ImGui::SetKeyboardFocusHere(-1);
                    }
                    else if (hasText || hasGifAttachment) {
                        std::string input(chatInputBuf);
                        if (hasText && input.size() > 1 && input[0] == '/') {
                            // Bot command: send only the command, no attachment
                            size_t spacePos = input.find(' ');
                            std::string cmd = (spacePos != std::string::npos) ? input.substr(1, spacePos - 1) : input.substr(1);
                            std::string args = (spacePos != std::string::npos) ? input.substr(spacePos + 1) : "";
                            nlohmann::json cmdJ;
                            cmdJ["cid"] = selectedChannelId;
                            cmdJ["cmd"] = cmd;
                            cmdJ["args"] = args;
                            netClient.Send(PacketType::Bot_Command, cmdJ.dump());
                        }
                        else {
                            // One message: optional text + optional attached GIF
                            std::string content;
                            if (hasGifAttachment) {
                                content = *attachedGifUrl;
                                if (hasText) {
                                    std::string trimmed(input);
                                    size_t start = trimmed.find_first_not_of(" \t\n\r");
                                    size_t end = trimmed.find_last_not_of(" \t\n\r");
                                    if (start != std::string::npos && end != std::string::npos)
                                        trimmed = trimmed.substr(start, end - start + 1);
                                    else if (start != std::string::npos)
                                        trimmed = trimmed.substr(start);
                                    if (!trimmed.empty())
                                        content = trimmed + "\n" + content;
                                }
                            }
                            else {
                                content = input;
                            }
                            if (!content.empty()) {
                                nlohmann::json msgJ;
                                msgJ["cid"] = selectedChannelId;
                                msgJ["u"] = currentUser.username;
                                msgJ["msg"] = content;
                                if (replyingToMessageId && *replyingToMessageId > 0) {
                                    msgJ["reply_to"] = *replyingToMessageId;
                                    *replyingToMessageId = 0;
                                }
                                netClient.Send(PacketType::Message_Text, msgJ.dump());
                            }
                        }
                        memset(chatInputBuf, 0, 1024);
                        if (attachedGifUrl) attachedGifUrl->clear();
                        ImGui::SetKeyboardFocusHere(-1);
                    }
                }
                ImGui::PopStyleColor();
                ImGui::PopStyleVar();
                ImGui::Unindent(32);
            }
        }
        else {
            const char* t = "Select a channel to get started";
            ImVec2 sz = ImGui::CalcTextSize(t);
            ImGui::Dummy(ImVec2(0, winH * 0.5f - sz.y));
            float cx = (areaW - sz.x) * 0.5f;
            if (cx > 0) { ImGui::Dummy(ImVec2(cx, 0)); ImGui::SameLine(); }
            ImGui::TextDisabled("%s", t);
        }

        // Member list panel (overlaid on right side of chat area)
        if (showMemberList && *showMemberList && serverMembers && !serverMembers->empty()) {
            float panelW = 180.0f;
            float panelX = areaW - panelW;
            ImGui::SetCursorPos(ImVec2(panelX, 0));
            ImGui::PushStyleColor(ImGuiCol_ChildBg, Styles::BgSidebar());
            ImGui::BeginChild("MemberList", ImVec2(panelW, winH), true);

            ImGui::PushStyleColor(ImGuiCol_Text, Styles::TextMuted());
            int onlineCount = 0;
            for (const auto& [_, on] : *serverMembers) if (on) onlineCount++;
            ImGui::Text("MEMBERS - %d/%d", onlineCount, (int)serverMembers->size());
            ImGui::PopStyleColor();
            ImGui::Separator();
            ImGui::Dummy(ImVec2(0, 4));

            for (const auto& [name, online] : *serverMembers) {
                std::string disp = name;
                size_t hp = name.find('#');
                if (hp != std::string::npos) disp = name.substr(0, hp);

                ImU32 dotCol = online ? IM_COL32(80, 220, 100, 255) : IM_COL32(120, 120, 125, 255);
                ImVec2 pos = ImGui::GetCursorScreenPos();
                ImGui::GetWindowDrawList()->AddCircleFilled(
                    ImVec2(pos.x + 6, pos.y + 8), 4.0f, dotCol);
                ImGui::Dummy(ImVec2(16, 0));
                ImGui::SameLine();

                if (!online) ImGui::PushStyleColor(ImGuiCol_Text, Styles::TextMuted());
                ImGui::Text("%s", disp.c_str());
                if (!online) ImGui::PopStyleColor();

                // Right-click for admin actions on members
                if (name != currentUser.username && ImGui::BeginPopupContextItem(("ml_admin_" + name).c_str())) {
                    ImGui::TextDisabled("%s", disp.c_str());
                    ImGui::Separator();
                    if (ImGui::Selectable("Chat Mute (10 min)")) {
                        nlohmann::json aj; aj["sid"] = currentServer.id; aj["u"] = name;
                        aj["type"] = "chat_mute"; aj["reason"] = "Admin"; aj["duration_minutes"] = 10;
                        netClient.Send(PacketType::Admin_Sanction_User, aj.dump());
                    }
                    if (ImGui::Selectable("Grant Admin")) {
                        nlohmann::json aj; aj["sid"] = currentServer.id; aj["u"] = name; aj["perms"] = 0xF;
                        netClient.Send(PacketType::Set_Member_Role, aj.dump());
                    }
                    ImGui::EndPopup();
                }
            }

            ImGui::EndChild();
            ImGui::PopStyleColor();
        }

        ImGui::EndChild();
        ImGui::PopStyleColor();
    }
}