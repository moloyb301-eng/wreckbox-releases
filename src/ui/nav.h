// Where you've been, so Back can take you there. No Windows types: it is tested by itself (tests/ui_logic_tests.cpp).
//
// The rules: going somewhere new remembers where you were. Going to where Back would lead is going back (the trail
// gets shorter, not longer). Going home — the shelf — starts the trail over. With nothing remembered, Back leads home.
#pragma once
#include <optional>
#include <vector>

namespace wb::ui {

template <class Place>
class History {
public:
    // You went from `from` to `to`. False if that was no move at all.
    bool went(const Place& from, const Place& to, const Place& home) {
        if (from == to) return false;
        if (to == home) trail_.clear();
        else if (!trail_.empty() && trail_.back() == to) trail_.pop_back();
        else {
            trail_.push_back(from);
            if (trail_.size() > kMost) trail_.erase(trail_.begin());
        }
        return true;
    }
    // Where Back leads from `from`: the place before, or home when nothing is remembered (nothing if already home).
    std::optional<Place> back(const Place& from, const Place& home) const {
        if (!trail_.empty()) return trail_.back();
        if (!(from == home)) return home;
        return std::nullopt;
    }
    size_t depth() const { return trail_.size(); }

private:
    static constexpr size_t kMost = 32;
    std::vector<Place> trail_;
};

}  // namespace wb::ui
