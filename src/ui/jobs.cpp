#include "ui/jobs.h"

#include <objbase.h>

namespace wb::ui {

void Jobs::start(HWND hwnd, unsigned threads) {
    hwnd_ = hwnd;
    for (unsigned i = 0; i < threads; ++i) threads_.emplace_back([this] { loop(); });
}

void Jobs::stop() {
    {
        std::lock_guard lock(m_);
        stopping_ = true;
        fifo_.clear();
        latest_.clear();
    }
    cv_.notify_all();
    // ponytail: a job that's mid-way (e.g. a long rescan) holds up exit until it finishes; add cancellation if quitting
    // during a rescan ever feels slow.
    for (auto& t : threads_) t.join();
    threads_.clear();
}

void Jobs::run(std::function<void()> work, std::function<void()> done) {
    {
        std::lock_guard lock(m_);
        fifo_.push_back({std::move(work), std::move(done)});
    }
    cv_.notify_one();
}

void Jobs::run_latest(std::function<void()> work, std::function<void()> done) {
    {
        std::lock_guard lock(m_);
        latest_.push_front({std::move(work), std::move(done)});
        if (latest_.size() > 48) latest_.pop_back();  // scrolled past long ago: not worth doing
    }
    cv_.notify_one();
}

void Jobs::dispatch(LPARAM lp) {
    auto* fn = reinterpret_cast<std::function<void()>*>(lp);
    (*fn)();
    delete fn;
}

void Jobs::loop() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);  // WIC and Media Foundation need COM on the worker
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    for (;;) {
        Job job;
        {
            std::unique_lock lock(m_);
            cv_.wait(lock, [&] { return stopping_ || !fifo_.empty() || !latest_.empty(); });
            if (stopping_) break;
            auto& q = !fifo_.empty() ? fifo_ : latest_;
            job = std::move(q.front());
            q.pop_front();
        }
        try {
            job.work();
        } catch (...) {
            // A failing job must not take the app down; the work itself logs what went wrong.
        }
        if (job.done) PostMessageW(hwnd_, WM_APP_DONE, 0, reinterpret_cast<LPARAM>(new std::function<void()>(std::move(job.done))));
    }
    CoUninitialize();
}

}  // namespace wb::ui
