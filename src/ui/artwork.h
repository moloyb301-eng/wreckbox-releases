// Cover art for the UI: downloads the Spotify cover if needed (via the store), decodes it with WIC straight to the
// size it's drawn at, and keeps a bounded LRU of Direct2D bitmaps. While a cover loads, callers draw a placeholder.
#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>

#include "library/store.h"
#include "ui/gfx.h"
#include "ui/jobs.h"

namespace wb::ui {

class ArtworkCache {
public:
    ArtworkCache(LibraryStore& store, Jobs& jobs, Gfx& gfx) : store_(store), jobs_(jobs), gfx_(gfx) {}

    void begin_frame();  // call once per paint
    // The bitmap for `t` at roughly `dips` DIPs square, or nullptr while it loads / if there is none.
    ID2D1Bitmap* get(const LibraryTrack& t, float dips);

private:
    struct Entry {
        ComPtr<ID2D1Bitmap> bmp;
        uint64_t used = 0, requested = 0;
        bool loading = false, missing = false;
    };
    void evict();

    LibraryStore& store_;
    Jobs& jobs_;
    Gfx& gfx_;
    std::unordered_map<std::string, Entry> entries_;
    uint64_t frame_ = 0;
    int generation_ = -1;
};

}  // namespace wb::ui
