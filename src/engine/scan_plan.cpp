#include "engine/scan_plan.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <format>
#include <thread>
#include <vector>

namespace wb {

Machine detect_machine() {
    Machine m;
    m.logical = std::max(1u, std::thread::hardware_concurrency());
    m.physical = m.logical;
    DWORD len = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &len);
    if (len) {
        std::vector<BYTE> buf(len);
        if (GetLogicalProcessorInformationEx(RelationProcessorCore, reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buf.data()), &len)) {
            unsigned cores = 0;
            for (DWORD off = 0; off < len;) {
                const auto* rec = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buf.data() + off);
                if (rec->Size == 0) break;
                if (rec->Relationship == RelationProcessorCore) ++cores;
                off += rec->Size;
            }
            if (cores) m.physical = std::clamp(cores, 1u, m.logical);
        }
    }
    SYSTEM_POWER_STATUS power{};
    if (GetSystemPowerStatus(&power)) {
        m.on_battery = power.ACLineStatus == 0;
        m.battery_saver = power.SystemStatusFlag == 1;
    }
    MEMORYSTATUSEX mem{};
    mem.dwLength = sizeof mem;
    if (GlobalMemoryStatusEx(&mem)) m.free_mb = mem.ullAvailPhys / (1024 * 1024);
    return m;
}

ScanPlan plan_scan(const Machine& m, size_t jobs) {
    const unsigned logical = std::max(1u, m.logical), physical = std::clamp(m.physical, 1u, logical);
    ScanPlan p;
    if (m.battery_saver) {
        p.workers = std::max(1u, physical / 4);
        p.reason = std::format("battery saver is on: a quarter of the {} cores", physical);
    } else if (m.on_battery) {
        p.workers = std::max(1u, physical / 2);
        p.reason = std::format("on battery: half of the {} cores, in efficiency mode", physical);
    } else {
        p.workers = std::max(1u, logical - 1);
        p.reason = std::format("plugged in: {} hardware threads, one left free for the app", logical);
    }
    if (jobs > 0 && p.workers > jobs) {
        p.workers = unsigned(jobs);
        p.reason = std::format("{} file{} to do", jobs, jobs == 1 ? "" : "s");
    }
    p.eco = m.on_battery || m.battery_saver;
    p.memory_budget_mb = m.free_mb == 0 ? 1024 : std::clamp<uint64_t>(m.free_mb / 2, 256, 8192);
    return p;
}

uint64_t estimated_audio_bytes(const std::filesystem::path& file, uint64_t file_size) {
    auto ext = file.extension().wstring();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
    // Decoded mono float at 22.05 kHz is 88 kB a second. Uncompressed stereo 16-bit is twice that on disk; FLAC about
    // the same; lossy audio down to ~120 kbit/s is a sixth of it.
    const uint64_t per_byte_x10 = ext == L".wav" || ext == L".aif" || ext == L".aiff" ? 10 : ext == L".flac" ? 15 : 60;
    return file_size * per_byte_x10 / 10 + 8 * 1024 * 1024;  // + the decoder's and the FFTs' working memory
}

uint64_t MemoryBudget::reserve(uint64_t want, const std::atomic<bool>& cancelled) {
    const uint64_t need = std::min(want, total_);
    std::unique_lock lock(m_);
    while (free_ < need) {
        if (cancelled) return 0;
        released_.wait_for(lock, std::chrono::milliseconds(100));
    }
    free_ -= need;
    return need;
}

void MemoryBudget::release(uint64_t bytes) {
    if (!bytes) return;
    {
        std::lock_guard lock(m_);
        free_ += bytes;
    }
    released_.notify_all();
}

void enter_background_worker(bool eco) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    if (eco) {
        THREAD_POWER_THROTTLING_STATE state{};
        state.Version = THREAD_POWER_THROTTLING_CURRENT_VERSION;
        state.ControlMask = THREAD_POWER_THROTTLING_EXECUTION_SPEED;
        state.StateMask = THREAD_POWER_THROTTLING_EXECUTION_SPEED;
        SetThreadInformation(GetCurrentThread(), ThreadPowerThrottling, &state, sizeof state);  // Windows 10 1709+; harmless before
    }
}

}  // namespace wb
