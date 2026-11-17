// Included inside class View (private part). The command palette (view_palette.cpp): Ctrl+K, type, Enter.
void open_palette();
void close_palette();
void palette();                // draws it over everything else (called while palette_open_)
bool palette_key(WPARAM vk);   // Up / Down / Enter / Esc while it's open; true if it used the key
bool palette_open_ = false;
// One thing the palette can find, and what choosing it does.
struct PaletteItem {
    enum class Kind { page, playlist, action, song } kind;
    std::wstring name, detail;  // detail: the quieter part after the name (a song's artist, a playlist's song count)
    const wchar_t* glyph;
    std::function<void()> run;
};
void palette_build();          // the results for what's in the box (on open, and on each change)
void palette_run(size_t i);    // closes the palette, then does what result i stands for
void palette_show_choice();    // scrolls the results so the chosen one is in view
std::vector<PaletteItem> palette_items_;
size_t palette_choice_ = 0;    // the result Enter runs (moved with Up / Down / Page Up / Page Down)
float palette_scroll_ = 0;
size_t palette_rows_ = 9;      // results in view at the last paint: what Page Up / Down move by
