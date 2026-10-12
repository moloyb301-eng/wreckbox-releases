#include "dasvis/director.h"

#include <algorithm>
#include <iterator>

namespace wb::dasvis {

namespace {

// Random numbers that depend only on the song's seed, the pick's number and a salt: the same every time.
double rnd(uint32_t seed, uint32_t index, uint32_t salt) {
    uint64_t z = (uint64_t(seed) << 32 | index) + uint64_t(salt) * 0x9E3779B97F4A7C15ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return double((z ^ (z >> 31)) >> 11) / double(1ull << 53);
}

}  // namespace

Director::Director(std::vector<SceneTags> scenes) : scenes_(std::move(scenes)) {}

void Director::set_scenes(std::vector<SceneTags> scenes) {
    scenes_ = std::move(scenes);
    recent_.clear();
}

void Director::set_cost(const std::string& scene, Tier tier, float ms) { costs_[scene + "|" + tier_name(tier)] = ms; }

float Director::cost(size_t scene, Tier tier) const {
    const auto it = scene < scenes_.size() ? costs_.find(scenes_[scene].name + "|" + tier_name(tier)) : costs_.end();
    return it == costs_.end() ? 0.f : it->second;
}

bool Director::fits(size_t scene, Tier tier, float budget_ms) const {
    if (scene >= scenes_.size()) return false;
    const float ms = cost(scene, tier);
    return ms > 0 ? ms <= budget_ms : !(scenes_[scene].cost == 'H' && tier == Tier::low);
}

uint8_t Director::energy_of(Section s, float part_energy) {
    if (s == Section::intro || s == Section::breakdown || s == Section::outro || part_energy < 0.35f) return kCalm;
    if (s == Section::drop || (s == Section::chorus && part_energy > 0.75f)) return kPeak;
    return kMid;
}

uint8_t Director::events(uint32_t seed, size_t drop, int intensity) {
    if (intensity >= 2) return kShockwave | kWhiteout | kCoverShatter;
    if (intensity <= 0) return kShockwave;  // chill: no white-out
    const double r = rnd(seed, uint32_t(drop), 7);
    return r < 1 / 3.0 ? kShockwave : r < 2 / 3.0 ? kWhiteout : kCoverShatter | kShockwave;
}

Cue Director::dress(size_t scene, const Moment& m) const {
    Cue c;
    c.scene = scene;
    if (m.drop) c.transition = m.from == Section::build ? Transition::zoom : Transition::shatter;  // build → drop: through it
    else if (m.glitchy) c.transition = Transition::glitch;
    else if (m.energy == kCalm && last_energy_ == kCalm) c.transition = Transition::crossfade;
    else c.transition = Transition::wipe;
    static constexpr float kBeats[] = {4, 2, 1, 2, 1};  // crossfade, wipe, zoom, shatter, glitch
    c.beats = kBeats[int(c.transition)];
    if (scene < scenes_.size()) {
        const SceneTags& t = scenes_[scene];
        // A mid layer (particles or the ribbon): never on chill; on insane whenever there's room; on normal half the
        // time, not when calm. The sigils: not on chill nor when calm; always on insane, else 40 % of the time.
        const bool room = cost(scene, m.tier) <= 0.7f * m.budget_ms;
        if (t.layers && room && m.intensity > 0 && (m.intensity == 2 || (m.energy != kCalm && rnd(m.seed, m.index, 3) < 0.5)))
            c.mid = rnd(m.seed, m.index, 4) < 0.5 ? Mid::particles : Mid::ribbon;
        c.ring = t.ring || (m.intensity == 2 && m.energy == kPeak);
        c.sigils = m.intensity > 0 && m.energy != kCalm && (m.intensity == 2 || rnd(m.seed, m.index, 5) < 0.4);
    }
    return c;
}

Cue Director::pick(const Moment& m, size_t current) {
    std::vector<size_t> ok;
    for (size_t i = 0; i < scenes_.size(); ++i)
        if (i != current && fits(i, m.tier, m.budget_ms)) ok.push_back(i);
    if (ok.empty()) return dress(current, m);  // nothing else fits: stay
    const auto keep = [&](auto pred) {
        std::vector<size_t> sub;
        std::copy_if(ok.begin(), ok.end(), std::back_inserter(sub), pred);
        if (!sub.empty()) ok = std::move(sub);
    };
    // Not one of the last 4, if there's anything else; a drop gets a hero (even a recent one, if it must).
    const auto fresh = [&](size_t i) { return std::find(recent_.begin(), recent_.end(), i) == recent_.end(); };
    if (m.drop) {
        keep([&](size_t i) { return scenes_[i].hero; });
        keep(fresh);
    } else {
        keep(fresh);
    }
    // Weighted: the energy it suits (×3, else ×0.4) and your taste for this genre.
    std::vector<double> w;
    double total = 0;
    for (const size_t i : ok) {
        double v = (scenes_[i].energy & m.energy) ? 3 : 0.4;
        if (taste_)
            if (const auto it = taste_->find(scenes_[i].name); it != taste_->end()) v *= std::max(0.05f, it->second);
        w.push_back(v), total += v;
    }
    double r = rnd(m.seed, m.index, 1) * total;
    size_t chosen = ok.back();
    for (size_t k = 0; k < ok.size(); ++k)
        if ((r -= w[k]) < 0) {
            chosen = ok[k];
            break;
        }
    const Cue c = dress(chosen, m);
    recent_.push_back(chosen);
    if (recent_.size() > 4) recent_.erase(recent_.begin());
    last_energy_ = m.energy;
    return c;
}

}  // namespace wb::dasvis
