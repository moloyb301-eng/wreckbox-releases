// How hard a scan may work this PC: how many files are analysed at once, and how much decoded audio they may hold
// between them. Sized from what the machine has right now — its cores, whether it's on battery, its free memory —
// so a plugged-in desktop uses every core and a laptop on battery doesn't spin its fans to analyse a folder.
//
// Analysis is CPU work (decoding and FFTs), so cores are what it's given. The graphics card is left to the picture
// (DasVis, MilkDrop), which is where it earns its keep.
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>

namespace wb {

struct Machine {
    unsigned logical = 1;   // hardware threads
    unsigned physical = 1;  // cores (= logical when the system doesn't say)
    bool on_battery = false;
    bool battery_saver = false;
    uint64_t free_mb = 0;  // 0 = unknown
};

Machine detect_machine();

struct ScanPlan {
    unsigned workers = 1;
    uint64_t memory_budget_mb = 1024;
    bool eco = false;    // workers run in Windows' efficiency mode
    std::string reason;  // for the log: why this many
};

// Plugged in: every hardware thread but one, which stays free for the window and the player.
// On battery: half the cores, in efficiency mode. Battery saver: a quarter.
// Never more workers than files. Memory: half of what's free, between 256 MB and 8 GB (1 GB when unknown).
ScanPlan plan_scan(const Machine& m, size_t jobs);

// About how many bytes a file's audio takes once decoded to mono at the analysis rate — an upper estimate from its
// size and type, known before it's opened (an hour-long mix is ~320 MB; a 5-minute track ~26 MB).
uint64_t estimated_audio_bytes(const std::filesystem::path& file, uint64_t file_size);

// A shared allowance of bytes. reserve() waits until the bytes are free; a request bigger than the whole allowance
// is trimmed to it, so one huge file still runs — alone. Returns what was reserved (0 if `cancelled` came first).
class MemoryBudget {
public:
    explicit MemoryBudget(uint64_t total) : total_(total), free_(total) {}
    uint64_t reserve(uint64_t want, const std::atomic<bool>& cancelled);
    void release(uint64_t bytes);

private:
    uint64_t total_, free_;
    std::mutex m_;
    std::condition_variable released_;
};

// Marks the calling thread as background work: below the window and the player in priority and, with `eco`,
// in Windows' efficiency mode (slower, less energy).
void enter_background_worker(bool eco);

}  // namespace wb
