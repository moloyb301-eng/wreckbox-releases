// Included inside class View (private part). Extra members and state of the track pages (view_tracks.cpp).
// The open crate's covers left of the page title, fanned like cards in a hand: the first song in front, each one after it
// behind, turned a little further and shifted right. Drawn at r's top left; returns the fan's bounds (empty: no covers).
// The geometry is here so the shelf's zoom can land in exactly this shape.
Rect cover_stack(const Rect& r);
static constexpr float kStackCover = 56;            // each cover's side
static constexpr float kStackCentre = 48;           // the covers' centre, this far below the page's top (level with the title)
static constexpr float kStackTurn[3] = {-3, 3, 9};  // degrees, front to back, each about its own centre
static constexpr float kStackShift[3] = {0, 9, 18}; // then moved right by this
// crate_covers() looks through the library, so it is asked again only when the page or the library changes.
std::vector<LibraryTrack> stack_covers_;
Where stack_for_;
uint64_t stack_at_ = ~0ull;  // the refresh they were taken at
uint64_t refreshes_ = 0;     // refresh() calls so far
