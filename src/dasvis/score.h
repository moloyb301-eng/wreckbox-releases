// The Score scene's sequencer grid (an idea from SQNCR, seq.halfof8.com: the picture *is* the score). Two bars of
// sixteenths across, sixteen rows up; every hit of the song is a block on it, placed by the style's rule, and the playhead
// loops over the two bars lighting each block as it plays. A block that plays again turns a quarter turn; each hit throws
// rings of pixels out across the grid, one ring after another, that hold and fade. Blocks the music stops playing fade
// as the playhead passes them, so the grid follows the song. Each visit picks a style:
//   city    scattered and hectic, blocks anywhere in their voice's rows
//   gagaku  formal: marks run straight across or straight up and meet like a crossword
//   koto    diagonal runs (escalators) with dots coming off them
//   ma      tall stacks far apart, on the beats only, throwing big blocks around them
// No GPU here: write() fills the texture the shader draws (scenes/score.hlsl).
#pragma once
#include <cstdint>
#include <vector>

namespace wb::dasvis {

class Score {
public:
    static constexpr int kCols = 32, kRows = 16;  // two bars of sixteenths; the voices' rows, bottom to top
    enum Voice { kick, snare, hats, bass, lead, kVoices };
    enum Style { city, gagaku, koto, ma, kStyles };

    void reset(uint32_t seed, double t);  // a new visit: a new style and layout; the first page strikes in
    Style style() const { return style_; }

    // A hit of `voice` at `at` (bars: whole bars from the song's first downbeat plus the phase), strength 0–1, sounding at
    // song time `when`. It's placed on the grid at once and lit when the clock reaches `when`.
    void place(Voice voice, double at, double when, float strength);
    // The clock: `bars` where the playhead is (as above), `t` song seconds. Fires the hits it passed, fades blocks no
    // longer played once the playhead is past them, moves the ripples.
    void update(double bars, double t);
    void burst(double t);  // a drop: every block lights and throws its rings

    float playhead() const { return float(head_); }  // columns, 0–kCols
    // The texture: (kCols × 2) × kRows RGBA floats. Left half per cell: voice + 1 (0: empty), shown (0–1), lit (0–1),
    // quarter turns (eased). Right half: a ripple dot (0–1), a ripple block (0–1), the ripple's voice + 1, 0.
    void write(float* out, double t) const;
    size_t blocks() const;
    size_t ripples() const { return ripples_.size(); }

private:
    struct Cell {
        int voice = -1;
        double shown_at = 0, gone_at = -1;  // appears from; fades from (−1: staying)
        double lit_from = -1, lit_until = -1, turned_at = -10;
        int turns = 0;
        long page = -1;  // the last page a hit put it there
    };
    struct Hit {
        double when;
        float strength;
        int voice;
        std::vector<int> cells;  // the first throws the ripples
    };
    struct Ripple {
        int cell, voice;
        bool block;
        double born, fade, dead;
    };
    Cell& at(int col, int row) { return cells_[size_t(row * kCols + col)]; }
    bool free_for(int col, int row, int voice) const;
    int place_near(int col, int row, int lo, int hi, int voice);  // the nearest free row in [lo, hi), else −1
    void fire(const Hit& h, double t);
    void throw_rings(int cell, int voice, float strength, double t);
    float rand();

    Style style_ = city;
    uint32_t salt_ = 1, rng_ = 1;
    std::vector<Cell> cells_ = std::vector<Cell>(size_t(kCols * kRows));
    std::vector<Hit> pending_;
    std::vector<Ripple> ripples_;
    double head_ = 0, now_ = 0;
    long page_ = 0;
    bool strike_ = false;  // the first page's blocks flash in, one after another
};

}  // namespace wb::dasvis
