// WreckBox account: email + password sign-in, library sync through the account, and
// finding your computers from anywhere (they register their tunnel address; see tunnel.h).
//
// The password never leaves the device: it's turned into a key with PBKDF2-HMAC-SHA256 (200,000 rounds, salted with the
// email) and only that key is sent. All calls block (network, and ~0.1 s of key derivation): run them on a worker.
#pragma once
#include <chrono>
#include <optional>
#include <stdexcept>
#include <string>

#include "library/store.h"

namespace wb::account {

struct Error : std::runtime_error {
    using std::runtime_error::runtime_error;  // the message is meant for the user
};

// WRECKBOX_API overrides it (tests talk to a local fake service).
std::string api_base();
bool signed_in();
std::string device_id();  // 12 random bytes as hex, made and saved on first use

// PBKDF2-HMAC-SHA256 with one 32-byte block, as lower-case hex.
std::string pbkdf2_hex(const std::string& password, const std::string& salt, unsigned rounds);
std::string derive_key(const std::string& email, const std::string& password, unsigned rounds = 200000);

void sign_up(const std::string& email, const std::string& password, const std::string& name);  // throws Error
void sign_in(const std::string& email, const std::string& password);
void sign_out();  // never throws; local sign-out always succeeds
void change_password(const std::string& old_password, const std::string& new_password);

// What a phone needs about each track: whether the computer has it, and its analysis. No file paths.
json crate_summary(const LibraryStore& store);
void upload_library(const LibraryStore& store);  // library + crate summary; does nothing when signed out
// Upload once changes have settled for 2 minutes (downloads arrive in bursts). Call after every change; `store` must
// outlive the call to shutdown().
void schedule_upload(LibraryStore& store, std::chrono::milliseconds delay = std::chrono::minutes(2));
void shutdown();  // stops the scheduler thread (at exit, before the store goes away)

// Announces (or refreshes) this computer's tunnel address; with no url it marks the computer unreachable.
void register_computer(const std::string& name, const std::string& platform, const std::optional<std::string>& url = std::nullopt,
                       const std::optional<std::string>& sync_token = std::nullopt);

}  // namespace wb::account
