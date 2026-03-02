#pragma once

namespace TalkMe::Limits {

/// Maximum number of static (non-GIF) textures kept in TextureManager.
/// Kept low so only visible/smart portion of images stay in VRAM.
constexpr int kMaxStaticTextures = 40;

/// Maximum number of GIF animation sets kept in TextureManager (global ceiling).
constexpr int kMaxGifSets = 20;
/// Chat viewport: max animated GIFs (att_, img_, yt_) kept at once; LRU evicts the rest.
constexpr int kMaxChatGifSetsAnimating = 5;
/// Picker grid: max animated GIFs (gif_ prefix) kept at once; LRU evicts the rest.
constexpr int kMaxPickerGifSetsAnimating = 12;

/// Longest edge (px) for decoded GIFs; larger GIFs are downscaled at decode time to save RAM/VRAM.
constexpr int kMaxGifDimension = 480;

/// Maximum number of frames to decode per GIF; extra frames are dropped to bound memory.
constexpr int kMaxGifFramesPerSet = 90;

/// Maximum number of decoded entries in ImageCache. GIF source bytes live on disk;
/// this caps in-RAM decoded images to bound memory.
constexpr int kMaxImageCacheEntries = 50;

/// Maximum texture width/height for D3D creation. Avoids driver crashes (e.g. Intel) on bad or huge dimensions.
constexpr int kMaxTextureDimension = 4096;

/// Pixels above and below the visible chat viewport that are still rendered.
/// Larger = smoother scroll, more CPU. 600 px ≈ ~10 average messages.
constexpr float kChatOverscanPx = 600.f;

/// Estimated height (pixels) for a message with no known height yet.
/// Over-estimated so the scrollbar thumb doesn't jump; refined on first render.
constexpr float kDefaultMsgHeightPx = 60.f;

/// waitMs used when MainApp is visible but no GIFs are animating.
/// ~20 fps — fast enough for text cursor blinks and hover highlights.
constexpr unsigned kIdleWaitMs = 48;

/// waitMs used when at least one GIF is animating. Keep at 16 (≈60 fps).
constexpr unsigned kAnimWaitMs = 16;

} // namespace TalkMe::Limits
