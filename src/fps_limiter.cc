#include "fps_limiter.hh"

#include <algorithm>
#include <chrono>

namespace low_latency {

DeviceClock::duration effective_min_delay(std::chrono::nanoseconds app_delay,
                                          double fps_limit) {
    if (fps_limit <= 0.0) {
        return app_delay;
    }
    // 1e9 ns / fps_limit = ns per frame. Use double-precision then round-trip
    // through integer nanoseconds for the max.
    const auto cap_interval_ns = 1e9 / fps_limit;
    const auto cap_interval =
        std::chrono::nanoseconds{static_cast<std::int64_t>(cap_interval_ns)};
    return std::max(app_delay, cap_interval);
}

} // namespace low_latency
