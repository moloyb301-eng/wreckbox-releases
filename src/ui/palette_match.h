// The command palette's ranking (view_palette.cpp): how well a name matches what was typed. No Windows types: it is
// tested by itself (tests/ui_logic_tests.cpp).
//
// The rules, best first: the name is the query, the name starts with it, a word of the name starts with it, it is
// somewhere inside the name, or its letters appear in the name in order ("stg" in "Settings"). Case is ignored for
// ASCII letters only; every other byte (UTF-8) is compared as it is.
//
// A query of several words is tried as one piece and also word by word: there every word must match somewhere, and
// the name scores as its weakest word, so "tracks all" still finds "All tracks" (as a word match, below "all tr").
// The better of the two counts. An empty query matches every name, all equally.
#pragma once
#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace wb::ui {

inline constexpr int kNoMatch = 0, kInOrder = 1, kInside = 2, kWordStart = 3, kStartsWith = 4, kExact = 5;
inline constexpr int kAnyMatch = kInOrder;  // what every name scores for an empty query

namespace match_detail {

inline bool is_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
inline bool is_alnum(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

inline std::string fold_case(std::string_view s) {
    std::string out(s);
    for (char& c : out)
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return out;
}

// A word of the name starts at i: the name's start, or after a space or ASCII punctuation ("(Remix)", "Motion: off").
// UTF-8 bytes count as letters, so a word never starts inside one.
inline bool word_starts(std::string_view name, size_t i) {
    return i == 0 || (static_cast<unsigned char>(name[i - 1]) < 0x80 && !is_alnum(name[i - 1]));
}

// One piece of the query against the name, both already folded (everything but kExact).
inline int piece(std::string_view name, std::string_view q) {
    if (name.starts_with(q)) return kStartsWith;
    const size_t first = name.find(q);
    for (size_t at = first; at != std::string_view::npos; at = name.find(q, at + 1))
        if (word_starts(name, at)) return kWordStart;
    if (first != std::string_view::npos) return kInside;
    size_t i = 0;
    for (const char c : name)
        if (i < q.size() && c == q[i]) ++i;
    return i == q.size() ? kInOrder : kNoMatch;
}

}  // namespace match_detail

// The query's words (split at white space).
inline std::vector<std::string_view> query_words(std::string_view q) {
    std::vector<std::string_view> out;
    for (size_t i = 0; i < q.size();) {
        while (i < q.size() && match_detail::is_space(q[i])) ++i;
        size_t j = i;
        while (j < q.size() && !match_detail::is_space(q[j])) ++j;
        if (j > i) out.push_back(q.substr(i, j - i));
        i = j;
    }
    return out;
}

// 0: no match; higher is better (kInOrder … kExact).
inline int match_score(std::string_view query, std::string_view name) {
    using namespace match_detail;
    const auto words = query_words(query);
    if (words.empty()) return kAnyMatch;
    std::string whole;  // the query as one piece, folded and single-spaced
    for (const auto w : words) whole += (whole.empty() ? "" : " ") + fold_case(w);
    const std::string n = fold_case(name);
    if (n == whole) return kExact;
    int best = piece(n, whole);
    if (words.size() > 1) {
        int weakest = kStartsWith;
        for (const auto w : words) weakest = std::min(weakest, piece(n, fold_case(w)));
        best = std::max(best, weakest);
    }
    return best;
}

// The indices of the entries scoring above 0, best first; equal scores keep the order they came in.
inline std::vector<size_t> rank_by_score(const std::vector<int>& scores) {
    std::vector<size_t> out;
    for (size_t i = 0; i < scores.size(); ++i)
        if (scores[i] > kNoMatch) out.push_back(i);
    std::stable_sort(out.begin(), out.end(), [&](size_t a, size_t b) { return scores[a] > scores[b]; });
    return out;
}

// The same, scoring each name against the query.
inline std::vector<size_t> rank_names(std::string_view query, const std::vector<std::string>& names) {
    std::vector<int> scores;
    scores.reserve(names.size());
    for (const auto& n : names) scores.push_back(match_score(query, n));
    return rank_by_score(scores);
}

}  // namespace wb::ui
