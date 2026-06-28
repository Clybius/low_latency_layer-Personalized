#include "display_deadline_pacer.hh"

#include "device_clock.hh"
#include "device_context.hh"
#include "physical_device_context.hh"
#include "predictive_queue_pacer.hh"
#include "present_timing_probe.hh"
#include "sleep_util.hh"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>

namespace low_latency {

DisplayDeadlinePacer::DisplayDeadlinePacer(const DeviceContext& device)
    : device_(device), core_(std::make_unique<PredictiveQueuePacer>(device)) {
    vblank_anchor_ = DeviceClock::now();
}

DisplayDeadlinePacer::~DisplayDeadlinePacer() = default;

bool DisplayDeadlinePacer::set_swapchain(VkSwapchainKHR swapchain) {
    if (swapchain == VK_NULL_HANDLE || refresh_queried_) {
        return swapchain_ != VK_NULL_HANDLE;
    }
    swapchain_ = swapchain;
    DeviceClock::duration refresh{};
    bool vrr = false;
    if (query_refresh_cycle(device_, swapchain, refresh, vrr)) {
        refresh_interval_ = refresh;
        is_vrr_ = vrr;
        refresh_queried_ = true;
        // Seed the vblank anchor to "now rounded up to next refresh boundary".
        const auto now = DeviceClock::now();
        const auto refresh_ns = static_cast<std::int64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(refresh).count());
        if (refresh_ns > 0) {
            const auto now_ns = static_cast<std::int64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    now.time_since_epoch())
                    .count());
            const auto k = (now_ns + refresh_ns - 1) / refresh_ns;
            vblank_anchor_ = DeviceClock::time_point{
                DeviceClock::duration{k * refresh_ns}};
        }
    }
    return true;
}

void DisplayDeadlinePacer::record_present_feedback(std::uint64_t target_ns,
                                                   std::uint64_t actual_ns) {
    if (actual_ns == 0) {
        return;
    }
    const auto lock = std::scoped_lock{feedback_mutex_};
    if (feedback_count_ < FEEDBACK_CAP) {
        feedback_ring_[feedback_count_++] = {target_ns, actual_ns};
    }
}

std::optional<DeviceClock::time_point>
DisplayDeadlinePacer::current_target_present() const {
    if (!has_target_present_.load(std::memory_order_acquire)) {
        return std::nullopt;
    }
    return last_target_present_;
}

DeviceClock::time_point DisplayDeadlinePacer::pace(
    const FrameTiming& timing, const DeviceClock::duration& min_delay) {
    // 1. Drain feedback samples into the PI integral + rebase the vblank grid.
    {
        const auto lock = std::scoped_lock{feedback_mutex_};
        double last_err_this_drain = pi_last_err_ns_;
        for (std::size_t i = 0; i < feedback_count_; ++i) {
            const auto& s = feedback_ring_[i];
            if (s.actual_ns == 0) {
                continue;
            }
            const auto actual = absolute_to_device_time(s.actual_ns);

            // Append to the median anchor window.
            if (anchor_window_count_ < ANCHOR_WINDOW) {
                anchor_window_[anchor_window_count_++] = actual;
            } else {
                for (std::size_t j = 1; j < ANCHOR_WINDOW; ++j) {
                    anchor_window_[j - 1] = anchor_window_[j];
                }
                anchor_window_[ANCHOR_WINDOW - 1] = actual;
            }
            // Rebase the vblank anchor to the median of the recent actuals
            // so a single noisy feedback can't drag the grid.
            if (anchor_window_count_ >= 3) {
                std::array<DeviceClock::time_point, ANCHOR_WINDOW> tmp{};
                for (std::size_t j = 0; j < anchor_window_count_; ++j) {
                    tmp[j] = anchor_window_[j];
                }
                const auto mid = tmp.begin() + anchor_window_count_ / 2;
                std::nth_element(tmp.begin(), mid,
                    tmp.begin() + anchor_window_count_);
                vblank_anchor_ = *mid;
            } else {
                vblank_anchor_ = actual;
            }

            if (s.target_ns != 0) {
                const auto err_ns = static_cast<double>(
                    s.actual_ns) - static_cast<double>(s.target_ns);
                pi_integral_ns_ += err_ns;
                last_err_this_drain = err_ns;
                // Clamp integral to ±half refresh to prevent windup.
                if (refresh_interval_.count() > 0) {
                    const auto half = static_cast<double>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(
                            refresh_interval_).count()) /
                        2.0;
                    pi_integral_ns_ = std::clamp(pi_integral_ns_, -half, half);
                }
            }
        }
        pi_last_err_ns_ = last_err_this_drain;
        feedback_count_ = 0;
    }

    // 2. Defer to the Tier 1 pacer for the latency-optimal release point.
    const auto jit_release = core_->pace(timing, min_delay);

    // 3. If we have no refresh cycle yet, or the present mode is one where
    //    there is no fixed vblank to align to (MAILBOX/IMMEDIATE), return
    //    the Tier 1 answer as-is. For MAILBOX/IMMEDIATE the queue depth is
    //    1 — there is no "next vblank" to align to and the vblank grid is
    //    meaningless. Just-in-time release is correct.
    const auto skip_grid =
        refresh_interval_.count() <= 0 ||
        present_mode_ == VK_PRESENT_MODE_MAILBOX_KHR ||
        present_mode_ == VK_PRESENT_MODE_IMMEDIATE_KHR;
    if (skip_grid) {
        last_target_present_ = jit_release;
        has_target_present_.store(true, std::memory_order_release);
        return jit_release;
    }

    // 4. Compute a target present time on the vblank grid.
    //    target_present = smallest vblank_k >= jit_release + render_budget
    //    (We use the Tier 1 release as the floor — never release before then.)
    const auto now = DeviceClock::now();
    const auto refresh_ns = static_cast<std::int64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(refresh_interval_).count());
    if (refresh_ns <= 0) {
        return jit_release;
    }
    const auto anchor_ns = static_cast<std::int64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            vblank_anchor_.time_since_epoch())
            .count());
    const auto release_ns = static_cast<std::int64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::max(jit_release, now).time_since_epoch())
            .count());
    // k = ceil((release_ns - anchor_ns) / refresh_ns); k >= 1.
    const auto diff_ns = release_ns - anchor_ns;
    auto k = (diff_ns + refresh_ns - 1) / refresh_ns;
    if (k < 1) {
        k = 1;
    }
    auto target_present = DeviceClock::time_point{
        DeviceClock::duration{anchor_ns + k * refresh_ns}};

    // 5. Apply PI correction to the release (so the frame lands on target_present).
    //    Proportional term reacts to the most recent error; integral term
    //    corrects for sustained offset. Combined output is clamped to ±half
    //    refresh so a single very-bad first sample can't overshoot by a
    //    full frame.
    auto target_release = jit_release + (target_present - std::max(jit_release, now));
    const auto pi_term_ns = KP * pi_last_err_ns_ + KI * pi_integral_ns_;
    if (refresh_interval_.count() > 0) {
        const auto half = static_cast<double>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                refresh_interval_).count()) /
            2.0;
        const auto clamped = std::clamp(pi_term_ns, -half, half);
        target_release -= DeviceClock::duration{
            static_cast<DeviceClock::rep>(clamped)};
    } else {
        target_release -= DeviceClock::duration{
            static_cast<DeviceClock::rep>(pi_term_ns)};
    }

    // 6. Clamp to one refresh beyond now so we never sleep more than a frame.
    target_release = std::min(target_release, now + refresh_interval_);

    // 7. Adaptive spin budget from the calibrated DeviceClock error bound.
    DeviceClock::duration spin_budget{LEGACY_SPIN_BUDGET};
    if (device_.clock) {
        const auto eb = device_.clock->error_bound_duration();
        const auto adaptive = std::max(
            DeviceClock::duration{MIN_SPIN_BUDGET},
            DeviceClock::duration{
                static_cast<DeviceClock::rep>(SPIN_BUDGET_MULTIPLIER * eb.count())});
        if (adaptive > DeviceClock::duration{}) {
            spin_budget = adaptive;
        }
    }

    const auto release = std::max(now, target_release);
    hybrid_sleep_until(release, spin_budget);

    last_target_present_ = target_present;
    has_target_present_.store(true, std::memory_order_release);
    return release;
}

} // namespace low_latency
