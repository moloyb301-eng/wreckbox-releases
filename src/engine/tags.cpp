// Port of core/src/tags.rs. TagLib's property map gives one vocabulary across formats:
// TITLE, ARTIST, ALBUMARTIST, ALBUM, DATE, GENRE, BPM, INITIALKEY, ISRC — mapped to TIT2/TPE1/…/TKEY/TSRC for ID3v2,
// the same names for Vorbis comments, and ©nam/©ART/…/tmpo/----:com.apple.iTunes:* atoms for MP4.
#include "engine/tags.h"

#include <windows.h>

#include <fstream>
#include <iterator>
#include <stdexcept>

#include <taglib/aifffile.h>
#include <taglib/fileref.h>
#include <taglib/flacfile.h>
#include <taglib/mp4file.h>
#include <taglib/mp4tag.h>
#include <taglib/mpegfile.h>
#include <taglib/opusfile.h>
#include <taglib/tpropertymap.h>
#include <taglib/vorbisfile.h>
#include <taglib/wavfile.h>

namespace fs = std::filesystem;

namespace wb {
namespace {

const char* const kMp4TextBpm = "----:com.apple.iTunes:BPM";

std::string utf8(const TagLib::String& s) { return s.to8Bit(true); }
TagLib::String tl(const std::string& s) { return TagLib::String(s, TagLib::String::UTF8); }
std::string utf8(const fs::path& p) {
    auto u = p.u8string();
    return {u.begin(), u.end()};
}
fs::path path_from_utf8(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

std::string trim(const std::string& s) {
    const char* ws = " \t\r\n\f\v";
    const auto a = s.find_first_not_of(ws);
    if (a == std::string::npos) return "";
    return s.substr(a, s.find_last_not_of(ws) - a + 1);
}

std::optional<std::string> first(const TagLib::PropertyMap& p, const char* key) {
    const auto it = p.find(key);
    if (it == p.end() || it->second.isEmpty()) return std::nullopt;
    return utf8(it->second.front());
}

enum class Flavour { Id3, Vorbis, Other };

Flavour flavour_of(TagLib::File* f) {
    if (dynamic_cast<TagLib::MPEG::File*>(f) || dynamic_cast<TagLib::RIFF::WAV::File*>(f) || dynamic_cast<TagLib::RIFF::AIFF::File*>(f))
        return Flavour::Id3;
    if (dynamic_cast<TagLib::FLAC::File*>(f) || dynamic_cast<TagLib::Ogg::Vorbis::File*>(f) || dynamic_cast<TagLib::Ogg::Opus::File*>(f))
        return Flavour::Vorbis;
    return Flavour::Other;
}

void set_text(TagLib::PropertyMap& p, const char* key, const std::optional<std::string>& v) {
    if (v && !trim(*v).empty()) p.replace(key, TagLib::StringList(tl(trim(*v))));
}

std::string join(const std::vector<std::string>& v, const char* sep) {
    std::string out;
    for (size_t i = 0; i < v.size(); ++i) out += (i ? sep : "") + v[i];
    return out;
}

void apply(TagLib::File* file, const TrackTags& t, const TagLib::ByteVector* cover) {
    TagLib::PropertyMap p = file->properties();  // start from what's there: setProperties drops anything left out
    set_text(p, "TITLE", t.title);
    if (!t.artists.empty()) {
        TagLib::StringList artists;
        switch (flavour_of(file)) {
            // Vorbis comments hold one ARTIST per artist; ID3v2.3 / MP4 use a single joined value.
            case Flavour::Vorbis:
                for (const auto& a : t.artists) artists.append(tl(a));
                break;
            case Flavour::Id3: artists.append(tl(join(t.artists, " / "))); break;
            case Flavour::Other: artists.append(tl(join(t.artists, ", "))); break;
        }
        p.replace("ARTIST", artists);
        p.replace("ALBUMARTIST", TagLib::StringList(tl(t.artists[0])));
    }
    set_text(p, "ALBUM", t.album);
    set_text(p, "DATE", t.year);
    set_text(p, "GENRE", t.genre);
    if (t.bpm && *t.bpm > 0.0) p.replace("BPM", TagLib::StringList(tl(std::to_string(std::llround(*t.bpm)))));
    if (t.key && !t.key->empty()) p.replace("INITIALKEY", TagLib::StringList(tl(short_key(*t.key))));
    set_text(p, "ISRC", t.isrc);
    file->setProperties(p);
    // MP4: TagLib writes the standard 2-byte `tmpo`; the Rust engine (Flutter build) reads BPM from the
    // ----:com.apple.iTunes:BPM text atom instead, so write that too.
    if (auto* mp4 = dynamic_cast<TagLib::MP4::File*>(file); mp4 && t.bpm && *t.bpm > 0.0)
        mp4->tag()->setItem(kMp4TextBpm, TagLib::StringList(tl(std::to_string(std::llround(*t.bpm)))));

    if (cover) {
        const bool png = cover->startsWith(TagLib::ByteVector("\x89PNG", 4));
        TagLib::List<TagLib::VariantMap> pictures;
        for (const auto& pic : file->complexProperties("PICTURE")) {
            // Keep other picture types; replace the front cover (MP4 covers have no type, so they count as front).
            const bool front = !pic.contains("pictureType") || pic.value("pictureType").toString() == "Front Cover";
            if (!front) pictures.append(pic);
        }
        TagLib::VariantMap pic;
        pic.insert("data", *cover);
        pic.insert("mimeType", TagLib::String(png ? "image/png" : "image/jpeg"));
        pic.insert("description", TagLib::String("Cover"));
        pic.insert("pictureType", TagLib::String("Front Cover"));
        pictures.append(pic);
        file->setComplexProperties("PICTURE", pictures);
    }
}

bool save(TagLib::File* file) {
    if (auto* f = dynamic_cast<TagLib::MPEG::File*>(file))
        return f->save(TagLib::MPEG::File::AllTags, TagLib::File::StripNone, TagLib::ID3v2::v3, TagLib::File::DoNotDuplicate);
    if (auto* f = dynamic_cast<TagLib::RIFF::WAV::File*>(file))
        return f->save(TagLib::RIFF::WAV::File::AllTags, TagLib::File::StripNone, TagLib::ID3v2::v3);
    if (auto* f = dynamic_cast<TagLib::RIFF::AIFF::File*>(file)) return f->save(TagLib::ID3v2::v3);
    return file->save();
}

std::vector<char> read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) throw std::runtime_error("can't read cover " + utf8(p));
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

}  // namespace

std::string short_key(const std::string& k) {
    if (!k.empty() && k.back() == 'm' && k.find(' ') == std::string::npos) return k;
    const auto words_at = k.find_first_not_of(" \t");
    if (words_at == std::string::npos) return "";
    const auto tonic_end = k.find_first_of(" \t", words_at);
    const std::string tonic = k.substr(words_at, tonic_end - words_at);
    bool minor = false;
    if (tonic_end != std::string::npos) {
        const auto m = k.find_first_not_of(" \t", tonic_end);
        if (m != std::string::npos) {
            std::string mode = k.substr(m, 3);
            for (auto& c : mode) c = char(::tolower(static_cast<unsigned char>(c)));
            minor = mode == "min";
        }
    }
    return tonic + (minor ? "m" : "");
}

TrackTags read_tags(const fs::path& path) {
    TagLib::FileRef f(path.c_str());
    if (f.isNull() || !f.file()->isValid()) throw std::runtime_error("can't read tags of " + utf8(path));
    const TagLib::PropertyMap p = f.file()->properties();
    TrackTags t;
    t.title = first(p, "TITLE");
    if (const auto it = p.find("ARTIST"); it != p.end())
        for (const auto& a : it->second) t.artists.push_back(utf8(a));
    t.album = first(p, "ALBUM");
    t.year = first(p, "DATE");
    t.genre = first(p, "GENRE");
    auto parse_bpm = [](const std::string& raw) -> std::optional<double> {
        try {
            size_t used = 0;
            const std::string s = trim(raw);
            const double v = std::stod(s, &used);
            if (used == s.size()) return v;
        } catch (const std::exception&) {
        }
        return std::nullopt;
    };
    if (const auto b = first(p, "BPM")) t.bpm = parse_bpm(*b);
    // MP4 files tagged by the Rust engine carry a 4-byte `tmpo` (TagLib reads 0) plus the text atom; use the latter.
    if (auto* mp4 = dynamic_cast<TagLib::MP4::File*>(f.file()); mp4 && (!t.bpm || *t.bpm <= 0.0)) {
        if (const auto item = mp4->tag()->item(kMp4TextBpm); item.isValid() && !item.toStringList().isEmpty())
            if (const auto v = parse_bpm(utf8(item.toStringList().front()))) t.bpm = v;
    }
    t.key = first(p, "INITIALKEY");
    t.isrc = first(p, "ISRC");
    t.has_cover = !f.file()->complexProperties("PICTURE").isEmpty();
    return t;
}

void write_tags(const fs::path& path, const TrackTags& t) {
    std::optional<TagLib::ByteVector> cover;
    if (t.cover && !t.cover->empty()) {
        const auto bytes = read_file(path_from_utf8(*t.cover));
        cover = TagLib::ByteVector(bytes.data(), static_cast<unsigned int>(bytes.size()));
    }
    if (!path.has_parent_path()) throw std::runtime_error("no parent folder");
    const fs::path tmp = path.parent_path() / (L".wreckbox-" + std::to_wstring(GetCurrentProcessId()) + L"-" + path.filename().wstring());
    std::error_code ec;
    fs::copy_file(path, tmp, fs::copy_options::overwrite_existing, ec);
    if (ec) throw std::runtime_error("can't copy the file for tagging");
    try {
        {
            TagLib::FileRef f(tmp.c_str());
            if (f.isNull() || !f.file()->isValid()) throw std::runtime_error("unsupported or corrupt audio file");
            apply(f.file(), t, cover ? &*cover : nullptr);
            if (!save(f.file())) throw std::runtime_error("can't save the tags");
        }
        // Make sure the result still opens as audio before replacing the original.
        {
            TagLib::FileRef check(tmp.c_str());
            if (check.isNull() || !check.file()->isValid()) throw std::runtime_error("file unreadable after tagging");
        }
        fs::rename(tmp, path, ec);
        if (ec) throw std::runtime_error("can't replace the original file");
    } catch (...) {
        fs::remove(tmp, ec);
        throw;
    }
}

}  // namespace wb
