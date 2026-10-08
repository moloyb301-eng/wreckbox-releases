#include "net/account.h"

#include <windows.h>
#include <bcrypt.h>

#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <thread>

#include "model/settings.h"
#include "net/http_client.h"

namespace wb::account {
namespace {

std::string hex(const unsigned char* b, size_t n) {
    static const char* d = "0123456789abcdef";
    std::string out;
    for (size_t i = 0; i < n; ++i) out += {d[b[i] >> 4], d[b[i] & 15]};
    return out;
}

std::string trim(const std::string& s) {
    const auto a = s.find_first_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}
std::string lower_trim(std::string s) {
    s = trim(s);
    for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

void save_quietly() {
    try {
        Settings::current().save();
    } catch (const std::exception&) {
        // The session stays in memory; the next save writes it.
    }
}

json call(const std::string& method, const std::string& path, const std::optional<std::string>& body = std::nullopt) {
    http::Headers headers{{"content-type", "application/json"}, {"x-wreckbox-device", device_id()}};
    if (const auto& t = Settings::current().account_token) headers["authorization"] = "Bearer " + *t;
    const auto r = http::request(method, api_base() + path, headers, body.value_or(""));
    if (r.status == 0) throw Error(r.error);
    const json j = r.body.empty() ? json::object() : json::parse(r.body, nullptr, false);
    // 401 means the session ended (password changed elsewhere, or signed out) -- except where it means a wrong password.
    // (The original build signs you out on a wrong "current password" too; that is not ported.)
    if (r.status == 401 && path != "/v1/login" && path != "/v1/password") {
        Settings::current().account_token.reset();
        save_quietly();
    }
    if (r.status >= 400)
        throw Error(j.is_object() && j.contains("error") && j["error"].is_string() ? j["error"].get<std::string>()
                                                                                    : "Account service error " + std::to_string(r.status));
    return j.is_object() ? j : json{{"value", j}};
}

void adopt_session(const json& j) {
    auto& s = Settings::current();
    s.account_token = j.value("token", "");
    if (const auto u = j.find("user"); u != j.end() && u->is_object()) {
        s.account_email = u->value("email", "");
        s.account_name = u->value("name", "");
    }
    s.save();
}

// The upload scheduler: one thread, created on first use, waiting for the due time.
std::mutex g_m;
std::condition_variable g_cv;
std::chrono::steady_clock::time_point g_due;
bool g_pending = false, g_stop = false;
std::thread g_thread;

}  // namespace

std::string api_base() {
    if (const char* v = std::getenv("WRECKBOX_API"); v && *v) return v;
    return "https://wreckbox-api.moloyb301.workers.dev";
}

bool signed_in() { return Settings::current().account_token.has_value(); }

std::string device_id() {
    auto& s = Settings::current();
    if (!s.device_id) {
        unsigned char b[12];
        BCryptGenRandom(nullptr, b, sizeof b, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        s.device_id = hex(b, sizeof b);
        save_quietly();
    }
    return *s.device_id;
}

std::string pbkdf2_hex(const std::string& password, const std::string& salt, unsigned rounds) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG) != 0)
        throw Error("Can't derive the key on this PC.");
    unsigned char out[32];
    const NTSTATUS st = BCryptDeriveKeyPBKDF2(alg, PUCHAR(password.data()), ULONG(password.size()), PUCHAR(salt.data()), ULONG(salt.size()), rounds,
                                              out, sizeof out, 0);
    BCryptCloseAlgorithmProvider(alg, 0);
    if (st != 0) throw Error("Can't derive the key on this PC.");
    return hex(out, sizeof out);
}

std::string derive_key(const std::string& email, const std::string& password, unsigned rounds) {
    return pbkdf2_hex(password, "wreckbox:" + lower_trim(email), rounds);
}

void sign_up(const std::string& email, const std::string& password, const std::string& name) {
    if (password.size() < 8) throw Error("Use at least 8 characters for the password.");
    adopt_session(call("POST", "/v1/signup", json{{"email", trim(email)}, {"key", derive_key(email, password)}, {"name", trim(name)}}.dump()));
}

void sign_in(const std::string& email, const std::string& password) {
    adopt_session(call("POST", "/v1/login", json{{"email", trim(email)}, {"key", derive_key(email, password)}}.dump()));
}

void sign_out() {
    try {
        call("POST", "/v1/logout");
    } catch (const std::exception&) {
        // Offline or already ended: signing out here still works.
    }
    {
        std::lock_guard lock(g_m);
        g_pending = false;
    }
    Settings::current().account_token.reset();
    save_quietly();
}

void change_password(const std::string& old_password, const std::string& new_password) {
    if (new_password.size() < 8) throw Error("Use at least 8 characters for the password.");
    const std::string email = Settings::current().account_email.value_or("");
    call("POST", "/v1/password", json{{"oldKey", derive_key(email, old_password)}, {"newKey", derive_key(email, new_password)}}.dump());
}

json crate_summary(const LibraryStore& store) {
    json tracks = json::object();
    const auto lib = store.library();
    const auto state = store.state_copy();
    if (lib)
        for (const auto& t : lib->tracks) {
            const auto st = state.tracks.find(t.id);
            if (st == state.tracks.end()) continue;
            json o = {{"s", to_string(st->second.status)}};
            if (st->second.local_path)
                if (const auto a = store.analysis_of(*st->second.local_path)) {
                    if (a->bpm) o["bpm"] = *a->bpm;
                    if (a->camelot) o["camelot"] = *a->camelot;
                    if (a->key) o["key"] = *a->key;
                    if (a->energy) o["energy"] = *a->energy;
                }
            tracks[t.id] = std::move(o);
        }
    return {{"tracks", tracks}, {"from", device_id()}, {"at", iso_seconds_now()}};
}

void upload_library(const LibraryStore& store) {
    const auto lib = store.library();
    if (!signed_in() || !lib) return;
    call("PUT", "/v1/blob/library", lib->to_json().dump());
    call("PUT", "/v1/blob/state", crate_summary(store).dump());
}

void schedule_upload(LibraryStore& store, std::chrono::milliseconds delay) {
    if (!signed_in()) return;
    std::lock_guard lock(g_m);
    g_due = std::chrono::steady_clock::now() + delay;
    g_pending = true;
    if (!g_thread.joinable()) {
        g_stop = false;
        g_thread = std::thread([&store] {
            std::unique_lock lock(g_m);
            while (!g_stop) {
                if (!g_pending) {
                    g_cv.wait(lock);
                } else if (std::chrono::steady_clock::now() < g_due) {
                    g_cv.wait_until(lock, g_due);  // woken early when the due time moves or we stop
                } else {
                    g_pending = false;
                    lock.unlock();
                    try {
                        upload_library(store);
                    } catch (const std::exception&) {
                        // Best effort: the next change schedules another.
                    }
                    lock.lock();
                }
            }
        });
    }
    g_cv.notify_all();
}

void shutdown() {
    {
        std::lock_guard lock(g_m);
        g_stop = true;
    }
    g_cv.notify_all();
    if (g_thread.joinable()) g_thread.join();
}

void register_computer(const std::string& name, const std::string& platform, const std::optional<std::string>& url,
                       const std::optional<std::string>& sync_token) {
    call("POST", "/v1/devices",
         json{{"id", device_id()}, {"name", name}, {"platform", platform}, {"url", url ? json(*url) : json(nullptr)},
              {"syncToken", sync_token ? json(*sync_token) : json(nullptr)}}.dump());
}

}  // namespace wb::account
