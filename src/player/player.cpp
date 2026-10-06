#include "player/player.h"

#include <algorithm>
#include <filesystem>

#include "engine/engine.h"
#include "model/settings.h"
#include "player/playlist.h"

namespace fs = std::filesystem;

namespace wb::player {
namespace {

std::string utf8(const fs::path& p) {
    const auto u = p.u8string();
    return {u.begin(), u.end()};
}

json& player_settings() {
    json& j = Settings::current().extra["player"];
    if (!j.is_object()) j = json::object();
    return j;
}

}  // namespace

// What VLC (with the plugins WreckBox ships) can play, plus playlist files.
const std::set<std::string>& Player::extensions() {
    static const std::set<std::string> e = {
        // common
        "mp3", "mp2", "mp1", "mpa", "aac", "adts", "m4a", "m4b", "mp4", "alac", "flac", "fla", "wav", "wave", "w64", "rf64", "aif",
        "aiff", "aifc", "caf", "ogg", "oga", "opus", "spx", "wma", "asf", "mka", "webm", "weba",
        // audiophile / less common
        "ape", "wv", "tta", "mpc", "mp+", "mpp", "ac3", "eac3", "ec3", "dts", "amr", "3ga", "au", "snd", "voc", "ra", "rm",
        // trackers, game and chip music
        "mod", "s3m", "xm", "it", "mtm", "669", "stm", "ult", "far", "med", "okt", "nsf", "nsfe", "spc", "vgm", "vgz", "gbs", "gym", "hes",
        "kss", "ay", "sap", "sid",
        // playlists
        "m3u", "m3u8", "pls", "xspf", "asx"};
    return e;
}

bool Player::playable_extension(const std::string& path) {
    std::string ext = utf8(fs::path(std::u8string(path.begin(), path.end())).extension());
    if (!ext.empty() && ext[0] == '.') ext.erase(0, 1);
    for (auto& c : ext) c = char(std::tolower(static_cast<unsigned char>(c)));
    return extensions().contains(ext);
}

std::vector<Item> Player::collect(const std::vector<std::string>& inputs) {
    std::vector<std::string> files;
    for (const auto& in : inputs) {
        const fs::path p(std::u8string(in.begin(), in.end()));
        std::error_code ec;
        if (fs::is_directory(p, ec)) {
            std::vector<std::string> found;
            for (fs::recursive_directory_iterator it(p, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec))
                if (it->is_regular_file(ec) && playable_extension(utf8(it->path()))) found.push_back(utf8(it->path()));
            std::sort(found.begin(), found.end());
            files.insert(files.end(), found.begin(), found.end());
        } else if (is_playlist(in)) {
            for (auto& e : expand_playlist(in)) files.push_back(std::move(e));  // .m3u / .pls / radio playlists
        } else if (is_url(in) || playable_extension(in)) {
            files.push_back(in);
        }
    }
    std::vector<Item> out;
    for (const auto& f : files) {
        Item it;
        it.location = f;
        if (!is_url(f)) {
            // Title and artist from the file's own tags, else its name.
            const json tags = read_tags_json(fs::path(std::u8string(f.begin(), f.end())));
            if (tags.contains("title") && tags["title"].is_string()) it.title = tags["title"].get<std::string>();
            if (tags.contains("artists") && tags["artists"].is_array() && !tags["artists"].empty()) {
                for (size_t i = 0; i < tags["artists"].size(); ++i) it.artist += (i ? ", " : "") + tags["artists"][i].get<std::string>();
            }
            if (it.title.empty()) it.title = utf8(fs::path(std::u8string(f.begin(), f.end())).stem());
        } else {
            it.title = f;
        }
        out.push_back(std::move(it));
    }
    return out;
}

Player::Player(LibraryStore& store, std::function<void(std::function<void()>)> post) : store_(store), post_(std::move(post)) {
    const json& j = player_settings();
    volume_ = std::clamp(j.value("volume", 0.8f), 0.f, 1.f);
    muted_ = j.value("muted", false);
    normalize_ = j.value("normalize", false);
    if (j.contains("equalizer") && j["equalizer"].is_object()) {
        const json& e = j["equalizer"];
        eq_.enabled = e.value("enabled", false);
        eq_.preset = e.value("preset", -1);
        eq_.preamp = e.value("preamp", 12.f);
        if (e.contains("bands") && e["bands"].is_array())
            for (const auto& b : e["bands"])
                if (b.is_number()) eq_.bands.push_back(b.get<float>());
    }
}

Player::~Player() {
    *alive_ = false;
    engine_.reset();  // stops libVLC's threads before the output goes away
    out_.reset();
}

bool Player::ensure_engine() {
    if (engine_ && engine_->ok()) return true;
    out_ = std::make_unique<AudioOutput>();
    if (!out_->ok()) {
        error_ = "No sound device found.";
        out_.reset();
        return false;
    }
    out_->set_volume(volume_);
    out_->set_muted(muted_);
    engine_ = std::make_unique<VlcEngine>(*out_);
    if (!engine_->ok()) {
        error_ = engine_->error();
        engine_.reset();
        return false;
    }
    // libVLC's threads → the UI thread. `alive` drops events that arrive while shutting down.
    engine_->on_event = [this, alive = alive_](Event e) {
        post_([this, alive, e] {
            if (*alive) on_event(e);
        });
    };
    engine_->set_equalizer(eq_);
    return true;
}

bool Player::can_play(const std::string& id) const {
    const auto r = store_.row(id);
    if (!r || r->status() != TrackStatus::downloaded || !r->state || !r->state->local_path) return false;
    std::error_code ec;
    const auto& p = *r->state->local_path;
    return fs::is_regular_file(fs::path(std::u8string(p.begin(), p.end())), ec);
}

void Player::play(const std::string& id, const std::vector<std::string>& list) {
    if (!can_play(id)) {
        error_ = "Can't play " + store_.describe(id) + ": it isn't on this computer.";
        changed();
        return;
    }
    // Only the clicked track's file is checked (a long list on a slow disk would stall the click); a missing file later
    // in the queue fails when reached and is skipped.
    std::vector<Item> items;
    size_t start = 0;
    auto add = [&](const std::string& tid) {
        const auto r = store_.row(tid);
        if (!r || r->status() != TrackStatus::downloaded || !r->state || !r->state->local_path) return;
        if (tid == id) start = items.size();
        items.push_back(Item{tid, *r->state->local_path, r->track.title, r->track.artist()});
    };
    for (const auto& tid : list) add(tid);
    if (std::none_of(items.begin(), items.end(), [&](const Item& i) { return i.track_id == id; })) {
        start = items.size();
        add(id);
    }
    play_items(std::move(items), start);
}

void Player::play_items(std::vector<Item> items, size_t start) {
    error_.clear();
    queue_.set(std::move(items), start);
    load_current();
}

void Player::load_current(int64_t start_ms) {
    const Item* it = queue_.current();
    if (!it) return;
    if (!ensure_engine()) {
        changed();
        return;
    }
    out_->pause(false);
    if (!engine_->open(it->location, normalize_, start_ms)) {
        error_ = "Can't play " + it->title + ".";
        playing_ = false;
    } else {
        playing_ = true;
    }
    changed();
}

void Player::on_event(Event e) {
    switch (e) {
        case Event::ended:
            if (queue_.next()) load_current();
            else {
                playing_ = false;
                out_->pause(true);
            }
            break;
        case Event::error: {
            const Item* it = queue_.current();
            error_ = "Can't play " + (it ? it->title : std::string("this")) + " — VLC couldn't decode it.";
            // Keep going through a dropped folder rather than stopping at one odd file.
            if (queue_.next()) load_current();
            else playing_ = false;
            break;
        }
        case Event::playing: playing_ = true; break;
        case Event::paused: playing_ = false; break;
        default: break;
    }
    changed();
}

void Player::toggle() {
    if (!engine_ || !queue_.current()) return;
    if (playing_) {
        engine_->pause(true);
        out_->pause(true);
        playing_ = false;
    } else if (engine_->playing() || engine_->time() > 0) {
        out_->pause(false);
        engine_->pause(false);
        playing_ = true;
    } else {
        load_current();  // finished or stopped: start this track again
    }
    changed();
}

void Player::next() {
    if (queue_.next()) load_current();
}

void Player::previous() {
    if (queue_.previous(time_ms()) == Queue::Prev::restart) seek(0);
    else load_current();
}

void Player::jump(size_t index) {
    if (queue_.jump(index)) load_current();
}

void Player::seek(int64_t ms) {
    if (!engine_ || !engine_->seekable()) return;
    engine_->seek(ms);
    changed();
}

void Player::stop() {
    if (engine_) engine_->stop();
    if (out_) out_->pause(true);
    queue_.clear();
    playing_ = false;
    changed();
}

// VLC's clock already is what's heard (measured against the frames the device played: tests/player_tests.cpp pacing),
// even though our buffer holds VLC's head start of ~1.3 s.
int64_t Player::time_ms() const { return engine_ ? engine_->time() : 0; }

int64_t Player::length_ms() const { return engine_ ? engine_->length() : 0; }
bool Player::seekable() const { return engine_ && engine_->seekable(); }
Player::Display Player::display() const {
    const Item* it = queue_.current();
    if (!it) return {};
    if (!is_url(it->location)) return {it->title, it->artist};
    const std::string song = engine_ ? engine_->now_playing() : "", station = engine_ ? engine_->meta_title() : "";
    // "https://radio.example/stream" → "radio.example"
    const auto p = it->location.find("://") + 3;
    const std::string host = it->location.substr(p, it->location.find_first_of("/?#:", p) - p);
    return {!song.empty() ? song : !station.empty() ? station : host, !song.empty() && !station.empty() ? station : host};
}

void Player::fail(std::string message) {
    error_ = std::move(message);
    changed();
}

void Player::set_volume(float v, bool save) {
    volume_ = std::clamp(v, 0.f, 1.f);
    if (out_) out_->set_volume(volume_);
    if (save) save_settings();
    changed();
}

void Player::set_muted(bool m) {
    muted_ = m;
    if (out_) out_->set_muted(m);
    save_settings();
    changed();
}

void Player::set_normalize(bool on) {
    normalize_ = on;
    save_settings();
    // VLC's normalizer is chosen when a track opens: reopen the current one where it was.
    if (engine_ && queue_.current() && engine_->length() > 0) {
        const bool was_playing = playing_;
        load_current(time_ms());
        if (!was_playing) toggle();
    }
    changed();
}

void Player::set_equalizer(const Equalizer& eq, bool save) {
    eq_ = eq;
    if (engine_) engine_->set_equalizer(eq_);
    if (save) save_settings();
    changed();
}

void Player::save_settings() {
    json& j = player_settings();
    j["volume"] = volume_;
    j["muted"] = muted_;
    j["normalize"] = normalize_;
    j["equalizer"] = {{"enabled", eq_.enabled}, {"preset", eq_.preset}, {"preamp", eq_.preamp}, {"bands", eq_.bands}};
    try {
        Settings::current().save();
    } catch (const std::exception&) {
        // Not worth interrupting playback over; the next change tries again.
    }
}

}  // namespace wb::player
