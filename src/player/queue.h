// The play queue's rules (from app/lib/player.dart), kept apart from playback so they can be tested on their own.
#pragma once
#include <optional>
#include <string>
#include <vector>

namespace wb::player {

struct Item {
    std::optional<std::string> track_id;  // a library track, or…
    std::string location;                 // …just a file path / URL (dropped files, Open, radio)
    std::string title, artist;            // what the player bar shows
};

class Queue {
public:
    void set(std::vector<Item> items, size_t start) {
        items_ = std::move(items);
        index_ = items_.empty() ? -1 : int(std::min(start, items_.size() - 1));
    }
    void clear() { items_.clear(), index_ = -1; }
    const Item* current() const { return index_ >= 0 && size_t(index_) < items_.size() ? &items_[size_t(index_)] : nullptr; }
    Item* current() { return index_ >= 0 && size_t(index_) < items_.size() ? &items_[size_t(index_)] : nullptr; }
    const std::vector<Item>& items() const { return items_; }
    size_t size() const { return items_.size(); }
    bool jump(size_t i) {  // play this one (false if there's no such item)
        if (i >= items_.size()) return false;
        index_ = int(i);
        return true;
    }
    int index() const { return index_; }
    bool has_next() const { return index_ + 1 < int(items_.size()); }

    // Moves on; false at the end of the queue.
    bool next() {
        if (!has_next()) return false;
        ++index_;
        return true;
    }
    // Like most players: restart the track unless we're at its very beginning (or it's the first one).
    enum class Prev { restart, moved };
    Prev previous(int64_t position_ms) {
        if (position_ms > 3000 || index_ <= 0) return Prev::restart;
        --index_;
        return Prev::moved;
    }

private:
    std::vector<Item> items_;
    int index_ = -1;
};

}  // namespace wb::player
