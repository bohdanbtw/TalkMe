# Messenger (Chat) Design Review

## What’s working well

### Architecture
- **Virtualized rendering**: You only lay out and draw messages in the visible window + overscan (`viewTop`–`viewBot`), and use a single `Dummy` for above-view content. That keeps draw calls and layout work bounded.
- **Two-phase image handling**: First pass collects visible image URLs and requests/protects them; render pass draws. Eviction is by `visibleTexIds` / protected URLs, so off-screen media is unloaded without thrashing.
- **Resource limits**: Centralized in `ResourceLimits.h` (texture counts, overscan, cache sizes). Easy to tune and reason about.
- **Skeleton placeholders**: Loading/evicted media show a sized, pulsing placeholder instead of text or empty space, which reduces perceived flicker and “jumpiness.”

### Design choices
- **Overscan (600 px)**: Good balance: enough so content doesn’t pop in at the edge, not so much that you render hundreds of messages.
- **Frame pacing**: Idle ~20 fps when no GIFs, ~60 fps when animating. Saves CPU when the feed is mostly text.
- **Messages by reference**: `GetChannelMessages()` returns `std::vector<ChatMessage>&`, so no per-frame copy of the message list.

---

## Where the remaining lag likely comes from

1. **Reply lookup is O(n) per message with a reply**  
   For each message with `replyToId > 0` you do a linear scan over `messages` to find the original. With many replies in view, this is O(visible messages × total messages) per frame.

2. **URL parsing is done twice**  
   First pass (build `visibleChatImageUrls`) and render loop both walk message content and parse URLs (prefixes, extensions, hostnames). Same work duplicated for every visible message.

3. **Heavy work every frame**  
   - `ProcessPendingGifDecodes()` runs every frame and can do WIC decoding.  
   - `EvictTexturesWithPrefixExcept` + `EvictGifIfNeeded` touch all texture/gif maps.  
   - First pass iterates from `firstVis` to end of `renderIdx` (not just “on screen”), so you’re still scanning a lot of messages for URLs.

4. **ImGui cost**  
   Every visible message does many `ImGui::Text`, `SameLine`, `PushStyleColor`, etc. Clipping is good, but the number of widgets per message is high (header, body with URL/mention splitting, attachments, reactions, context menu).

5. **GIF animation**  
   When GIFs are visible, you run at 60 fps and sample the current frame by time. That’s correct, but it keeps the frame budget tight.

---

## Recommendations

### Performance (to reduce lag)

| Priority | Change | Why |
|----------|--------|-----|
| **High** | **Reply lookup: O(1) per message** | Build once per frame a `std::unordered_map<int, const ChatMessage*>` (or index) from `msg.id` to message; for each `replyToId` do a map lookup instead of scanning `messages`. Big win with many replies. |
| **Medium** | **Throttle eviction** | Call `EvictTexturesWithPrefixExcept` every 2nd or 3rd frame (e.g. when `(frame_count % 2) == 0`), so scroll doesn’t constantly evict and then re-upload the same few items at the boundary. |
| **Medium** | **Parse URLs once** | In the first pass, build “message index → list of image URLs” (or store on a side structure for the current channel). In the render loop, use that instead of re-parsing content. |
| **Low** | **Slightly reduce overscan when scrolling fast** | If you track scroll velocity, you could reduce `kChatOverscanPx` when velocity is high (e.g. 400 px), and restore when idle. Fewer messages laid out = less work. |
| **Low** | **Cap ProcessPendingGifDecodes per frame** | Process at most 1–2 GIFs per frame instead of the whole queue, so one heavy decode doesn’t spike a single frame. |

### UX / “busyness” design

- **Layout**: Message header (name, tag, time, pinned) + body + attachments + reactions is clear. Skeleton placeholders already reduce busyness; keeping them one color and a soft pulse (as now) is good.
- **Density**: If the feed feels noisy, consider:
  - Slightly more spacing between messages (e.g. after each `EndGroup`).
  - Collapsing or shortening reply preview to one line with a clear “in reply to X” so the main content stands out.
- **Scroll feel**: The remaining “sticky” feel can be from (1) eviction/re-upload at the boundary, (2) layout recalc when message heights change. Throttling eviction and keeping reply lookup O(1) should help; if needed, you can also round scroll position or smooth it slightly (e.g. lerp toward target scroll) so it doesn’t feel like it’s fighting the layout.

### Code quality

- **Split RenderChannelView**: The function is very large. Consider extracting: “build render index + firstVis”, “first pass: collect URLs and request”, “render one message”, “scroll and eviction bookkeeping”. That will make it easier to add the “parse URLs once” and reply-map optimizations without touching everything.
- **Constants**: `areaW * 0.5f`, `300.0f` (max height), `280x180` (skeleton default) appear in several places. Centralizing them (e.g. in `ResourceLimits.h` or a small `ChatLayout` struct) would make tuning and consistency easier.

---

## Summary

- **Busyness-design**: Virtualization, resource limits, and skeleton placeholders are in good shape; the main remaining lag is likely from per-frame work (reply scan, duplicate URL parsing, eviction and GIF decode).
- **Highest-impact, low-risk change**: Make reply lookup O(1) with a per-frame id→message map.
- **Next**: Throttle texture eviction and/or parse URLs once; then consider splitting the big render function and capping GIF decodes per frame if you still see hitches.
