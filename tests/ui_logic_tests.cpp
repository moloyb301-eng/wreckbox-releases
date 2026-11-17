// The parts of the UI that are plain logic (no window needed): where Back leads, how the command palette ranks names.
#include <cstdio>
#include <string>
#include <vector>

#include "ui/nav.h"
#include "ui/palette_match.h"

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    if (ok) return;
    ++failures;
    std::printf("FAILED: %s\n", what);
}

void history_rules() {
    using wb::ui::History;
    const std::string home = "shelf";
    History<std::string> h;
    check(!h.back(home, home), "nothing to go back to from home");
    check(h.back("settings", home) == home, "with nothing remembered, Back leads home");

    check(h.went(home, "house", home), "home to a crate is a move");
    check(!h.went("house", "house", home), "staying put is not a move");
    check(h.back("house", home) == home, "Back from the crate leads home");
    h.went("house", "settings", home);
    check(h.back("settings", home) == std::string("house"), "Back from settings leads to the crate");
    h.went("settings", "house", home);  // going where Back leads is going back
    check(h.depth() == 1 && h.back("house", home) == home, "going back shortens the trail");
    h.went("house", home, home);
    check(h.depth() == 0, "going home starts the trail over");

    for (int i = 0; i < 100; ++i) h.went(std::to_string(i), std::to_string(i + 1), home);
    check(h.depth() <= 32, "the trail doesn't grow without end");
}

void palette_scores() {
    using namespace wb::ui;
    check(match_score("set", "Set") == kExact, "the name is the query");
    check(match_score("set", "Settings") == kStartsWith, "the name starts with the query");
    check(match_score("tracks", "All tracks") == kWordStart, "a word of the name starts with the query");
    check(match_score("off", "Motion: off") == kWordStart, "a word starts after punctuation too");
    check(match_score("set", "Reset") == kInside, "the query is inside the name");
    check(match_score("stg", "Settings") == kInOrder, "the query's letters appear in order");
    check(match_score("gts", "Settings") == kNoMatch, "letters out of order are no match");
    check(match_score("xyz", "Settings") == kNoMatch, "no match scores 0");
    check(kExact > kStartsWith && kStartsWith > kWordStart && kWordStart > kInside && kInside > kInOrder && kInOrder > kNoMatch,
          "the kinds of match are ordered");

    check(match_score("SETTINGS", "settings") == kExact, "case is ignored");
    check(match_score("all TR", "All tracks") == kStartsWith, "case is ignored in a longer query");
    check(match_score("caf\xC3\xA9", "Caf\xC3\xA9 del mar") == kStartsWith, "UTF-8 passes through");
    check(match_score("CAF\xC3\x89", "caf\xC3\xA9") == kNoMatch, "only ASCII letters fold");

    check(match_score("all tracks", "All tracks") == kExact, "several words, exactly");
    check(match_score("  all   tracks ", "All tracks") == kExact, "extra spaces don't count");
    check(match_score("tracks all", "All tracks") == kWordStart, "words in another order still match, as words");
    check(match_score("mo off", "Motion: off") == kWordStart, "each word scores where it matches; the weakest counts");
    check(match_score("my zzz", "My folders") == kNoMatch, "every word must match");
    check(match_score("set fl", "Settings flac") > match_score("set fl", "Settings for all"), "a word match beats letters in order");

    check(match_score("", "Settings") == kAnyMatch && match_score("", "x") == kAnyMatch, "an empty query matches everything");
    check(match_score("   ", "Settings") == kAnyMatch, "a query of spaces is empty");
    check(kAnyMatch > kNoMatch, "what an empty query finds is a match");
}

void palette_ranking() {
    using namespace wb::ui;
    const std::vector<std::string> names = {"Shelf", "Reset", "Settings", "Set"};
    check(rank_names("set", names) == std::vector<size_t>{3, 2, 1}, "best first, no match left out");
    check(rank_names("", names) == std::vector<size_t>{0, 1, 2, 3}, "an empty query keeps every name, in order");
    check(rank_names("zzz", names).empty(), "nothing matches");
    check(rank_names("s", {"Shelf", "Settings", "Soulseek sync"}) == std::vector<size_t>{0, 1, 2}, "ties keep their order");
    check(rank_by_score({1, 3, 0, 3, 1}) == std::vector<size_t>{1, 3, 0, 4}, "ranking is stable");
}

}  // namespace

int main() {
    history_rules();
    palette_scores();
    palette_ranking();
    if (failures) return std::printf("%d check(s) failed\n", failures), 1;
    std::printf("ui_logic_tests: all passed\n");
    return 0;
}
