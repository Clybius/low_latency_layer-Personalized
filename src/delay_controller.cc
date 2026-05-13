#include "delay_controller.hh"

#include <algorithm>
#include <fstream>
#include <print>

namespace low_latency {

DelayController::DelayController() {}

DelayController::~DelayController() {}

void DelayController::delay(const DeviceClock::duration& min_delay) {
    using namespace std::chrono;

    if (!this->previous_frame.has_value()) {
        this->previous_frame.emplace(frame_info{
            .release = DeviceClock::now(),
        });
        return;
    }

    const auto frametime = DeviceClock::now() - this->previous_frame->release;

    // Apply frame limits.
    while (DeviceClock::now() < this->previous_frame->release + min_delay) {
        std::this_thread::yield();
    }

    // Apply jitter.
    const auto jitter =
        this->previous_frame->jitter == 0ns ? frametime / 100 : 0ns;
    for (const auto start = DeviceClock::now();
         DeviceClock::now() < start + jitter;) {

        std::this_thread::yield();
    }

    // Apply extra drain time.
    for (const auto start = DeviceClock::now();
         DeviceClock::now() < start + this->drain;) {
        std::this_thread::yield();
    }

    const auto now = DeviceClock::now();

    // Calculate our gradient.
    // -1 => Applying a random jitter sleep actually improved frametimes.
    //       Simulation depth is probably completely backlogged.
    // 0  => Applying a sleep did nothing. We are behind in simulation depth.
    // 1  => Applying a sleep directly impacted frametimes. Don't sleep!
    const auto gradient = [&]() -> auto {
        const auto dt_jitter = this->previous_frame->jitter != 0ns
                                   ? -this->previous_frame->jitter
                                   : jitter;
        const auto dt_frametime = frametime - this->previous_frame->frametime;
        const auto dt_frametime_ns = static_cast<double>(dt_frametime.count());
        const auto dt_jitter_ns = static_cast<double>(dt_jitter.count());
        return std::clamp(dt_frametime_ns / dt_jitter_ns, -1.0, 1.0);
    }();

    // Feed our gradient into ewma -> our gradient is noisy and an ewma smooths
    // it out into a readable signal.

    if (this->previous_frame->jitter != 0ns) {
        constexpr auto ALPHA = 0.01; // Not tuned - appears to work OK.
        this->gradient_ewma =
            (ALPHA * gradient) + ((1.0 - ALPHA) * this->gradient_ewma);

        this->drain = std::clamp(
            this->drain + DeviceClock::duration{static_cast<long long>(
                              (0.5 - this->gradient_ewma) *
                              static_cast<double>(this->previous_frame->jitter.count()))},
            DeviceClock::duration{0}, frametime);
    }

    // LOGGING
    static auto out = std::ofstream{"/tmp/low_latency.log", std::ios::trunc};
    std::println(out,
                 "delay_controller::delay()\n"
                 "    gradient: {}\n"
                 "    gradient_ewma: {}\n"
                 "    drain: {}\n"
                 "    jitter: {}\n",
                 gradient, gradient_ewma, drain, jitter);
    // END LOGGING

    this->previous_frame.emplace(frame_info{
        .frametime = frametime,
        .jitter = jitter,
        .release = now,
    });
}

} // namespace low_latency
