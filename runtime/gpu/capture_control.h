#pragma once
// Frame captures asked for from the F1 Capture tab, or taken automatically while playing (D3D12Backend::Present).
// The renderer starts them; the menu reads Status and requests delayed ones. F4 stays gpu::g_capture.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <string>
#include <utility>

namespace gpu::capture {

constexpr float kMinInterval = 5, kMaxInterval = 600;  // seconds between automatic captures
constexpr uint32_t kMaxPerSession = 500;

// when the next automatic capture is due: every interval seconds of play while enabled, at most limit a session.
// Turning it on starts the interval; nothing is due while the settings menu is open (paused), and a capture that
// fell due meanwhile is taken once it closes.
class AutoSchedule {
public:
    bool Due(double now, bool enabled, float interval, uint32_t taken, uint32_t limit, bool paused) {
        if (!enabled) {
            armed_ = false;
            return false;
        }
        if (!armed_) {
            armed_ = true;
            last_ = now;
        }
        const float wait = std::isfinite(interval) ? std::clamp(interval, kMinInterval, kMaxInterval) : kMinInterval;
        return !paused && taken < limit && now - last_ >= wait;
    }
    void Taken(double now) { last_ = now; }

private:
    bool armed_ = false;
    double last_ = 0;
};

// "Capture in N seconds" from the menu: one capture once the delay has passed and the menu is closed
class Delayed {
public:
    void Request(double now, double delay) {
        std::lock_guard lock(lock_);
        at_ = now + delay;
        pending_ = true;
    }
    void Cancel() {
        std::lock_guard lock(lock_);
        pending_ = false;
    }
    bool Pending() const {
        std::lock_guard lock(lock_);
        return pending_;
    }
    bool Take(double now, bool menu_open) {
        std::lock_guard lock(lock_);
        if (!pending_ || menu_open || now < at_) return false;
        pending_ = false;
        return true;
    }

private:
    mutable std::mutex lock_;
    double at_ = 0;
    bool pending_ = false;
};

// what the menu shows: this session's captures and the latest folder or error
class Status {
public:
    struct View {
        uint32_t manual = 0, automatic = 0;
        std::string last, error;
    };
    void Finished(std::string folder, bool automatic) {
        std::lock_guard lock(lock_);
        (automatic ? automatic_ : manual_)++;
        last_ = std::move(folder);
        error_.clear();
    }
    void Failed(std::string why) {
        std::lock_guard lock(lock_);
        error_ = std::move(why);
    }
    uint32_t Automatic() const { return automatic_.load(); }
    View Snapshot() const {
        std::lock_guard lock(lock_);
        return {manual_.load(), automatic_.load(), last_, error_};
    }

private:
    mutable std::mutex lock_;
    std::atomic<uint32_t> manual_{0}, automatic_{0};
    std::string last_, error_;
};

inline Status g_status;
inline Delayed g_delayed;

// seconds on the steady clock, the time base of both the renderer's and the menu's requests
inline double Now() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

}  // namespace gpu::capture
