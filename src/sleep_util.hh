#ifndef SLEEP_UTIL_HH_
#define SLEEP_UTIL_HH_

#include "device_clock.hh"

#include <chrono>
#include <thread>

namespace low_latency {

// Sleep until `deadline` using a hybrid strategy:
//   - if remaining > spin_budget: coarse sleep_for, leaving a spin tail
//   - else: busy-yield spin for precision
// spin_budget should be >= DeviceClock::error_bound_duration() to absorb calibration jitter.
inline void hybrid_sleep_until(const DeviceClock::time_point& deadline,
                                const DeviceClock::duration& spin_budget) {
    auto now = DeviceClock::now();
    while (now < deadline) {
        const auto remaining = deadline - now;
        if (remaining > spin_budget) {
            std::this_thread::sleep_for(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    remaining - spin_budget));
        } else {
            std::this_thread::yield();
        }
        now = DeviceClock::now();
    }
}

} // namespace low_latency

#endif
