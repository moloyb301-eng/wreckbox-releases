// Synced lyrics for DasVis's kinetic type: LRC text ("[01:02.50]a line"), from a .lrc file next to the song or from
// LRCLIB (lrclib.net, an open lyrics database). Lyrics are off until the user turns them on: then the app asks LRCLIB with
// the song's title, artist, album and length, nothing else. No network code here: the app does the asking.
#pragma once
#include <string>
#include <string_view>
#include <vector>

namespace wb::dasvis {

struct Lyrics {
    struct Line {
        double start = 0, end = 0;  // song seconds; `end` is the next line's start (at most 8 s on)
        std::string text;           // UTF-8, never empty
    };
    std::vector<Line> lines;  // in time order
    // The line being sung at `t` (song seconds): its index, or -1 (before the first, in a gap, after the last).
    int at(double t) const;
};

// Every "[mm:ss.xx]" line (several stamps on one line repeat it; "[offset:+200]" moves them all, in ms, + = earlier, as
// players do). Tags ([ar:], [ti:]…), word stamps ("<00:01.20>") and empty lines (they end the line before) are dropped.
Lyrics parse_lrc(std::string_view lrc);

// The synced lyrics in LRCLIB's answer: /api/get's song, or the first of /api/search's songs that has them ("" if none
// has, or only plain ones).
std::string lrclib_synced(std::string_view json_body);

}  // namespace wb::dasvis
