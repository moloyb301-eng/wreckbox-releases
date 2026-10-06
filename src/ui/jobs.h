// Background work for the UI: run something on a worker thread, then (optionally) a continuation on the UI thread.
// Two lanes: `run` is first-in-first-out (user actions); `run_latest` is newest-first and capped, for work that only
// matters while it's on screen (cover thumbnails while scrolling).
#pragma once
#include <windows.h>

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace wb::ui {

inline constexpr UINT WM_APP_DONE = WM_APP + 1;     // lParam: std::function<void()>* to run on the UI thread
inline constexpr UINT WM_APP_CHANGED = WM_APP + 2;  // the store changed: repaint

class Jobs {
public:
    void start(HWND hwnd, unsigned threads);
    void stop();
    void run(std::function<void()> work, std::function<void()> done = {});
    void run_latest(std::function<void()> work, std::function<void()> done = {});
    // Call from the window procedure for WM_APP_DONE.
    static void dispatch(LPARAM lp);

private:
    struct Job {
        std::function<void()> work, done;
    };
    void loop();

    HWND hwnd_ = nullptr;
    std::mutex m_;
    std::condition_variable cv_;
    std::deque<Job> fifo_, latest_;
    bool stopping_ = false;
    std::vector<std::thread> threads_;
};

}  // namespace wb::ui
