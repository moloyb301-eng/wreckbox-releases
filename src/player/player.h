// The player (port of app/lib/player.dart, plus files outside the library, internet radio, volume, VLC's equalizer and
// normalizer). Lives on the UI thread: every method is called from there; libVLC's events are brought back to it
// through `post`.
#pragma once
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "library/store.h"
#include "player/audio_output.h"
#include "player/queue.h"
#include "player/vlc_engine.h"

namespace wb::player {

class Player {
public:
    // `post` runs a function on the UI thread later.
    Player(LibraryStore& store, std::function<void(std::function<void()>)> post);
    ~Player();

    std::function<void()> on_changed;  // something the player bar shows changed

    bool can_play(const std::string& track_id) const;  // downloaded and the file is there
    // Plays a library track; next / previous move through the playable tracks of `list` (the list on screen).
    void play(const std::string& track_id, const std::vector<std::string>& list);
    void play_items(std::vector<Item> items, size_t start = 0);  // files / URLs (Open, drag & drop, radio)
    void toggle();
    void next();
    void previous();
    void jump(size_t index);  // play the queue item at `index`
    void seek(int64_t ms);
    void stop();

    // State for the UI.
    const Item* current() const { return queue_.current(); }
    const Queue& queue() const { return queue_; }
    bool active() const { return queue_.current() != nullptr; }
    bool playing() const { return playing_; }
    int64_t time_ms() const;
    int64_t length_ms() const;
    bool seekable() const;
    // What to show for the current item (bar, visualizer, Windows' media flyout): a file's title and artist; for a
    // stream the song the station announces, then the station's name, then the host.
    struct Display {
        std::string title, subtitle;
    };
    Display display() const;
    const std::string& error() const { return error_; }
    void fail(std::string message);    // show a problem in the player bar (e.g. nothing playable was dropped)

    // Sound settings (saved in settings.json under "player").
    float volume() const { return volume_; }
    bool muted() const { return muted_; }
    void set_volume(float v, bool save = true);
    void set_muted(bool m);
    bool normalize() const { return normalize_; }
    void set_normalize(bool on);
    const Equalizer& equalizer() const { return eq_; }
    void set_equalizer(const Equalizer& eq, bool save = true);
    void save_settings();

    AudioOutput* output() { return out_.get(); }  // the visualizer reads its tap; null until something has played

    // Files and folders → playable items (folders searched recursively, sorted by path); for drag & drop and Open.
    // Runs on a worker: it touches the disk and reads tags.
    static std::vector<Item> collect(const std::vector<std::string>& paths);
    static bool playable_extension(const std::string& path);
    static const std::set<std::string>& extensions();  // lower case, without the dot

private:
    bool ensure_engine();
    void load_current(int64_t start_ms = 0);
    void on_event(Event e);
    void changed() const {
        if (on_changed) on_changed();
    }

    LibraryStore& store_;
    std::function<void(std::function<void()>)> post_;
    std::unique_ptr<AudioOutput> out_;
    std::unique_ptr<VlcEngine> engine_;
    Queue queue_;
    bool playing_ = false;
    std::string error_;
    float volume_ = 0.8f;
    bool muted_ = false, normalize_ = false;
    Equalizer eq_;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);  // events posted after destruction are dropped
};

}  // namespace wb::player
