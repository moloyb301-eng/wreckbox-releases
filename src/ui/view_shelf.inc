// Included inside class View (private part). The shelf of crates and the way into one (view_shelf.cpp).
void shelf(const Rect& r);     // the page: planks of crates
bool shelf_key(WPARAM vk);     // arrows move between crates, Enter opens the chosen one; true if it used the key
// Opening a crate with the pointer: goes to its page, which arrives out of the crate at `from` (its rect on the shelf).
void open_crate(Page p, std::optional<std::string> playlist, const Rect& from);
// One frame of a crate opening or closing: draws what's needed of the shelf and the page into r. False when no zoom is
// running, and the caller draws the page as usual.
bool zoom_frame(const Rect& r);
// Up to n songs whose covers stand for a crate (a playlist's first songs, a library view's, the most recent).
std::vector<LibraryTrack> crate_covers(Page p, const std::optional<std::string>& playlist, size_t n) const;
float shelf_scroll_ = 0;
int shelf_sel_ = -1;  // the crate chosen with the keyboard (-1: none)

// The crates on the wall, in order: the library's views, then one per playlist.
struct Crate {
    Page page;
    std::optional<std::string> playlist;
    std::wstring name, count;
    std::optional<double> filled;  // a playlist: how much of it is in your crate
};
std::vector<Crate> crates() const;
// One crate: its box at `box` on the wall; `chosen`: the keyboard's ring is on it.
void draw_crate(const Crate& c, const Rect& box, bool full_motion, bool chosen);
// The songs in a crate, front first (the cache's own copy; crate_covers() hands out copies of it).
const std::vector<LibraryTrack>& crate_songs(Page p, const std::optional<std::string>& playlist, size_t n) const;
void crate_cache_check() const;  // forgets the covers and counts below once the library or its counts changed
// Kept per crate until the library (or how many songs are in each state) changes: the shelf asks every paint.
struct CrateSongs {
    std::vector<LibraryTrack> tracks;
    size_t asked = 0;  // how many were asked for when they were found
};
mutable std::map<std::pair<int, std::string>, CrateSongs> crate_cache_;
mutable std::map<std::string, size_t> crate_have_;  // a playlist's songs that are in your crate
mutable size_t crate_sig_ = 0;
int shelf_cols_ = 1, shelf_count_ = 0;  // the last layout, for the arrow keys
bool shelf_reveal_ = false;             // scroll the chosen crate into view at the next paint
// The shelf's own keys (or a key that came back here) chose the crate: its ring shows while the keyboard is in use. Not
// set by other keys, so a command from the palette doesn't put a ring on a crate the mouse came back from.
bool shelf_ring_ = false;
uint64_t shelf_visit_ = 0;              // the visit the shelf was last drawn for (a new one: maybe back from a crate)
std::optional<Where> shelf_from_;       // the last crate page shown: chosen when the shelf comes back
// The shelf as the user last saw it: the rect it was drawn in, and the lean the pointer gave the crate under it. A crate
// opening draws the shelf from these, so it grows out of exactly what was clicked (the page may be narrower by then).
Rect shelf_rect_{};
D2D1_POINT_2F shelf_lean_{0, 0};
float shelf_head_ = 0;  // the height of the shelf's header, as last drawn
// What a drawing of the shelf for show includes: the crate opening fades later than the rest around it.
enum class ShelfPass { all, rest, opening };
ShelfPass shelf_pass_ = ShelfPass::all;
// How far the opening crate's page has come in (0–1; 1 when no crate is opening). For what arrives with that page, such
// as the inspector beside it, to fade in with it.
float zoom_page_a_ = 1;
// The crate opening: from its rect on the shelf, into its page. `start` is set by the first frame drawn (a click lands
// between paints, and the clock is the paint's).
struct Zoom {
    bool on = false;
    Where to;
    Rect from;
    double start = -1;
};
Zoom zoom_;
