#include "predictive_queue_pacer.hh"

#include "device_clock.hh"
#include "device_context.hh"
#include "instance_context.hh"
#include "sleep_util.hh"

#include <algorithm>
#include <chrono>

namespace low_latency {

PredictiveQueuePacer::PredictiveQueuePacer(const DeviceContext& device)
    : device_(&device) {}

double PredictiveQueuePacer::choose_alpha(double obs, double ewma) {
    if (ewma <= 0.0) {
        return RENDER_ALPHA_NOMINAL;
    }
    const auto dev = std::abs(obs - ewma) / ewma;
    if (dev > DEV_HITCH) {
        return RENDER_ALPHA_HITCH;
    }
    if (dev > DEV_NOISY) {
        return RENDER_ALPHA_NOISY;
    }
    if (dev > DEV_NOMINAL) {
        return RENDER_ALPHA_NOMINAL;
    }
    return RENDER_ALPHA_STABLE;
}

void PredictiveQueuePacer::window_push(std::array<double, WINDOW_SIZE>& w,
                                        std::size_t& count, double v) {
    if (count < WINDOW_SIZE) {
        w[count++] = v;
    } else {
        for (std::size_t i = 1; i < WINDOW_SIZE; ++i) {
            w[i - 1] = w[i];
        }
        w[WINDOW_SIZE - 1] = v;
    }
}

DeviceClock::time_point PredictiveQueuePacer::pace(
    const FrameTiming& timing, const DeviceClock::duration& min_delay) {
    using namespace std::chrono;

    const auto now = DeviceClock::now();

    // Update render-duration EWMA from the awaited frame's GPU interval.
    // Adaptive alpha: 0.05 (hitch) / 0.20 (noisy) / 0.40 (nominal) / 0.60
    // (stable). Samples that deviate from the current EWMA by more than 75%
    // are recorded in the window but not folded into the EWMA — but only
    // once the EWMA has had a chance to converge. During the first
    // WINDOW_SIZE frames the EWMA is too small to be a reliable baseline,
    // so a normal frame would look like a hitch; rejecting it would trap
    // the EWMA at its initial value.
    if (timing.gpu_end > timing.gpu_start) {
        const auto render_ns = static_cast<double>(
            duration_cast<nanoseconds>(timing.gpu_end - timing.gpu_start).count());
        if (render_ns > 0.0) {
            window_push(render_window, render_window_count, render_ns);
            // Use level+trend as the prediction for classification and
            // rejection. During trend periods (scene complexity changing)
            // this avoids misclassifying legitimate shifts as hitches.
            const auto predicted = ewma_render_ns.has_value()
                ? *ewma_render_ns + ewma_render_trend
                : 0.0;
            const auto reject = render_window_count >= WINDOW_SIZE &&
                predicted > 0.0 &&
                std::abs(render_ns - predicted) / predicted > REJECT_HITCH;
            if (!reject) {
                const auto alpha = choose_alpha(render_ns, predicted);
                if (ewma_render_ns.has_value()) {
                    // Double EWMA (Holt's linear method):
                    //   level_t = alpha * obs + (1-alpha) * (level_{t-1} + trend_{t-1})
                    //   trend_t = beta  * (level_t - level_{t-1}) + (1-beta) * trend_{t-1}
                    const auto old_level = *ewma_render_ns;
                    const auto new_level = alpha * render_ns +
                        (1.0 - alpha) * (old_level + ewma_render_trend);
                    ewma_render_trend = TREND_ALPHA * (new_level - old_level) +
                        (1.0 - TREND_ALPHA) * ewma_render_trend;
                    // Clamp trend to ±half the level so a single anomalous
                    // frame cannot yank the prediction beyond reason.
                    if (new_level > 0.0) {
                        const auto max_trend = new_level / 2.0;
                        ewma_render_trend = std::clamp(
                            ewma_render_trend, -max_trend, max_trend);
                    }
                    ewma_render_ns = new_level;
                } else {
                    ewma_render_ns = render_ns;
                    ewma_render_trend = 0.0;
                }
            }
        }
    }

    // Update CPU-lag EWMA: time from previous release to this frame's GPU start.
    // Same ramp-up protection as the render EWMA.
    if (release_prev && timing.gpu_start > *release_prev) {
        const auto lag_ns = static_cast<double>(
            duration_cast<nanoseconds>(timing.gpu_start - *release_prev).count());
        if (lag_ns >= 0.0) {
            window_push(lag_window, lag_window_count, lag_ns);
            const auto reject = lag_window_count >= WINDOW_SIZE &&
                ewma_cpu_lag_ns &&
                std::abs(lag_ns - *ewma_cpu_lag_ns) / *ewma_cpu_lag_ns > REJECT_HITCH;
            if (!reject) {
                const auto alpha = choose_alpha(lag_ns,
                    ewma_cpu_lag_ns.value_or(0.0));
                ewma_cpu_lag_ns = ewma_cpu_lag_ns
                                      ? alpha * lag_ns +
                                            (1.0 - alpha) * *ewma_cpu_lag_ns
                                      : lag_ns;
            }
        }
    }

    // Hitch-driven frame-skip: if the observed render time is wildly above
    // the EWMA, the next frame's release is set to now (no sleep) so the
    // app can catch up to vsync instead of compounding the late frame.
    //
    // We require the rolling window to be full before trusting the EWMA
    // for hitch detection. During ramp-up the EWMA is very small, and a
    // single normal frame would look like a hitch — but allowing the skip
    // there would bypass the user's FPS cap, which is worse than the hitch.
    if (ewma_render_ns && *ewma_render_ns > 0.0 &&
        render_window_count >= WINDOW_SIZE &&
        timing.gpu_end > timing.gpu_start) {
        const auto observed_ns = static_cast<double>(
            duration_cast<nanoseconds>(timing.gpu_end - timing.gpu_start).count());
        const auto predicted = *ewma_render_ns + ewma_render_trend;
        if (observed_ns > SKIP_HITCH_MULT * predicted) {
            const_cast<FrameTiming&>(timing).skip_next_release = true;
        }
    }

    // Hitch-driven catch-up: if the previous frame hitched, set the
    // target release to now so the queue catches up rather than
    // compounding the late frame. The FPS cap floor (below) can still
    // override this — the cap is the user's explicit choice and must
    // be respected even on a hitch. Previously this was an unconditional
    // `release = now` that bypassed the cap entirely, which made the
    // FPS_LIMIT env var a no-op whenever a hitch was detected.
    const auto skip_target_to_now = timing.skip_next_release;

    // Just-in-time target: release so the next frame's GPU start ~= this frame's
    // GPU end. next_gpu_start_est = release + ewma_cpu_lag; solve for release.
    auto target_release = timing.gpu_end;
    if (ewma_cpu_lag_ns && *ewma_cpu_lag_ns > 0.0) {
        target_release -= DeviceClock::duration{
            static_cast<DeviceClock::rep>(*ewma_cpu_lag_ns)};
    }

    if (skip_target_to_now) {
        target_release = now;
    }

    // Clamp the predictive target to at most one predicted render beyond now
    // (hitch recovery — don't sleep forever on a stale render estimate).
    // With trend tracking the prediction = level + trend, which is more
    // accurate during scene-complexity shifts.
    if (ewma_render_ns && *ewma_render_ns > 0.0) {
        const auto predicted = *ewma_render_ns + ewma_render_trend;
        const auto max_sleep = DeviceClock::duration{
            static_cast<DeviceClock::rep>(predicted)};
        target_release = std::min(target_release, now + max_sleep);
    }

    // Frame-rate cap floor (preserves maxFPS / minimumIntervalUs / layer
    // FPS_LIMIT semantics). Applied AFTER the hitch clamp and the
    // skip_next_release target adjustment so a hard user/app cap is
    // always respected — neither recovery mechanism can undo it.
    if (release_prev && min_delay.count() > 0) {
        target_release = std::max(target_release, *release_prev + min_delay);
    }

    const auto release = std::max(now, target_release);

    // Spin budget adapts to the calibrated DeviceClock error bound so the
    // final spin tail is sized to the actual noise floor of this device.
    DeviceClock::duration spin_budget{LEGACY_SPIN_BUDGET};
    if (device_ && device_->clock) {
        const auto eb = device_->clock->error_bound_duration();
        const auto adaptive = std::max(
            DeviceClock::duration{MIN_SPIN_BUDGET},
            DeviceClock::duration{
                static_cast<DeviceClock::rep>(SPIN_BUDGET_MULTIPLIER * eb.count())});
        if (adaptive > DeviceClock::duration{}) {
            spin_budget = adaptive;
        }
    }
    hybrid_sleep_until(release, spin_budget);

    release_prev = release;
    return release;
}

} // namespace low_latency
