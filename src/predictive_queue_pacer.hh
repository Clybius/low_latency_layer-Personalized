#ifndef PREDICTIVE_QUEUE_PACER_HH_
#define PREDICTIVE_QUEUE_PACER_HH_

#include "presentation_pacer.hh"

#include <array>
#include <cstddef>
#include <optional>

namespace low_latency {

class DeviceContext;

// Tier 1 backend: predictive just-in-time render-queue pacer.
// Uses only the GPU timestamp spans we already collect (via SubmissionSpan).
// Closed loop on observed CPU release-to-GPU-start lag.
//
// Algorithm (per frame, after await_completed on the frame's spans):
//   1. Update EWMA of (gpu_end - gpu_start)  -> predicted render duration
//   2. Update EWMA of (gpu_start - release_prev) -> predicted CPU lag
//   3. target_release = gpu_end - ewma_cpu_lag
//        (so next frame's GPU start ~= this frame's GPU end -> depth ~0)
//   4. target_release = max(target_release, release_prev + min_delay)  // cap floor
//   5. target_release = min(target_release, now + ewma_render)          // hitch recovery
//   6. release = max(now, target_release); sleep until release.
//
// EWMA alpha is adaptive: a small rolling window of samples classifies the
// current observation as hitch / noisy / nominal / stable, and the alpha
// for that band is chosen accordingly. Hitching samples (>75% deviation
// from the current EWMA) are not folded into the EWMA but are recorded
// in the window so the next non-hitch sample can compare against the
// recent history.
class PredictiveQueuePacer final : public PresentationPacer {
  public:
    explicit PredictiveQueuePacer(const DeviceContext& device);
    PredictiveQueuePacer() = delete;

    DeviceClock::time_point pace(
        const FrameTiming& timing,
        const DeviceClock::duration& min_delay) override;

  private:
    // Adaptive alpha bands: higher alpha reacts faster to genuine changes
    // (hitches, scene complexity shifts); lower alpha smooths during
    // stability to avoid amplifying noise.
    static constexpr double RENDER_ALPHA_HITCH = 0.05;
    static constexpr double RENDER_ALPHA_NOISY = 0.20;
    static constexpr double RENDER_ALPHA_NOMINAL = 0.40;
    static constexpr double RENDER_ALPHA_STABLE = 0.60;
    static constexpr double LAG_ALPHA_HITCH = 0.05;
    static constexpr double LAG_ALPHA_NOISY = 0.20;
    static constexpr double LAG_ALPHA_NOMINAL = 0.40;
    static constexpr double LAG_ALPHA_STABLE = 0.60;

    // Relative deviation bands used to classify a sample.
    static constexpr double DEV_HITCH = 0.30;
    static constexpr double DEV_NOISY = 0.15;
    static constexpr double DEV_NOMINAL = 0.05;

    // Hitch rejection: samples more than this fraction off the current EWMA
    // are not folded in (but ARE recorded in the rolling window so the next
    // sample can be compared against recent history).
    static constexpr double REJECT_HITCH = 0.75;

    // Hitch-driven frame-skip: if the observed render time exceeds 2.5x the
    // EWMA, mark skip_next_release so the caller releases the next frame
    // immediately rather than waiting for the next full interval.
    static constexpr double SKIP_HITCH_MULT = 2.5;

    // Min spin budget — even when calibration error is zero, leave a small
    // spin tail to absorb clock noise between check and wake.
    static constexpr auto MIN_SPIN_BUDGET = std::chrono::microseconds{50};

    // Fallback spin budget (matches legacy hard-coded value).
    static constexpr auto LEGACY_SPIN_BUDGET = std::chrono::microseconds{200};

    // Multiplier on the calibrated error bound for the spin tail.
    static constexpr std::uint32_t SPIN_BUDGET_MULTIPLIER = 2;

    static constexpr std::size_t WINDOW_SIZE = 8;

    const DeviceContext* device_{};

    std::optional<double> ewma_render_ns{};
    std::optional<double> ewma_cpu_lag_ns{};
    std::optional<DeviceClock::time_point> release_prev{};

    std::array<double, WINDOW_SIZE> render_window{};
    std::size_t render_window_count{0};
    std::array<double, WINDOW_SIZE> lag_window{};
    std::size_t lag_window_count{};

    static double choose_alpha(double obs, double ewma);
    static void window_push(std::array<double, WINDOW_SIZE>& w,
                            std::size_t& count, double v);
};

} // namespace low_latency

#endif
