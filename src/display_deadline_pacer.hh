#ifndef DISPLAY_DEADLINE_PACER_HH_
#define DISPLAY_DEADLINE_PACER_HH_

#include "presentation_pacer.hh"

#include <vulkan/vulkan.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>

namespace low_latency {

class DeviceContext;

// Tier 2 backend: vblank-anchored deadline pacer.
//   - Wraps PredictiveQueuePacer for the just-in-time render/cpu_lag math
//     (the latency-optimal release point).
//   - Aligns that release point to the next vblank on a steady grid anchored
//     to the most recent known actualPresentTime.
//   - Optional PI feedback loop driven by vkGetPastPresentationTiming(EXT|GOOGLE)
//     to keep release-to-actual present error small.
//
// VRR displays (EXT reports refreshInterval==0) fall back to cadence pacing
// using refreshDuration as a soft target rather than a hard vblank grid.
class DisplayDeadlinePacer final : public PresentationPacer {
  public:
    explicit DisplayDeadlinePacer(const DeviceContext& device);
    ~DisplayDeadlinePacer() override;
    DisplayDeadlinePacer(const DisplayDeadlinePacer&) = delete;
    DisplayDeadlinePacer(DisplayDeadlinePacer&&) = delete;
    DisplayDeadlinePacer& operator=(const DisplayDeadlinePacer&) = delete;
    DisplayDeadlinePacer& operator=(DisplayDeadlinePacer&&) = delete;

    DeviceClock::time_point pace(
        const FrameTiming& timing,
        const DeviceClock::duration& min_delay) override;

    // Called by the present-timing feedback poll (driven by the caller/strategy).
    // target may be 0 if the app didn't set one (we still get actual).
    void record_present_feedback(std::uint64_t target_ns, std::uint64_t actual_ns);

    // Returns the most recently chosen target present time, or nullopt if none.
    std::optional<DeviceClock::time_point> current_target_present() const;

    // Bind a swapchain so the pacer can lazily query its refresh cycle.
    // Safe to call multiple times; the first non-null refresh_cycle wins.
    // Returns true (Tier 2 accepted the swapchain).
    bool set_swapchain(VkSwapchainKHR swapchain) override;

    // Set the present mode of the swapchain. Used to skip the vblank grid
    // for MAILBOX/IMMEDIATE (no fixed vblank to align to).
    void set_present_mode(VkPresentModeKHR mode) override {
        this->present_mode_ = mode;
    }

    // Accessor for the monitor to retrieve the bound swapchain (used to
    // poll past presentation timings from the monitor thread).
    VkSwapchainKHR swapchain_handle() const { return swapchain_; }

  private:
    const DeviceContext& device_;
    VkSwapchainKHR swapchain_{VK_NULL_HANDLE};
    std::unique_ptr<class PredictiveQueuePacer> core_;

    DeviceClock::duration refresh_interval_{};
    bool is_vrr_{false};
    bool refresh_queried_{false};

    // Present mode of the swapchain. Set by the SwapchainMonitor via a
    // helper. Used to skip the vblank grid for MAILBOX/IMMEDIATE modes
    // (where there is no fixed vblank to align to).
    VkPresentModeKHR present_mode_{VK_PRESENT_MODE_FIFO_KHR};

    // PI feedback state. We apply KP * (latest err) + KI * integral of
    // (actual - target) to the release, clamped to ±half refresh.
    static constexpr double KP = 0.10;
    static constexpr double KI = 0.05;
    double pi_integral_ns_{0.0};
    double pi_last_err_ns_{0.0};
    DeviceClock::time_point vblank_anchor_{};
    std::atomic<bool> has_target_present_{false};
    DeviceClock::time_point last_target_present_{};

    // Median-of-N vblank anchor rebase. Each feedback sample updates the
    // window; vblank_anchor_ is rebased to the median so a single noisy
    // feedback can't drag the grid. 8 samples gives a robust median
    // without being slow to adapt to genuine clock drift.
    static constexpr std::size_t ANCHOR_WINDOW = 8;
    std::array<DeviceClock::time_point, ANCHOR_WINDOW> anchor_window_{};
    std::size_t anchor_window_count_{0};

    // Min spin budget — even when calibration error is zero, leave a small
    // spin tail to absorb clock noise between check and wake.
    static constexpr auto MIN_SPIN_BUDGET = std::chrono::microseconds{50};
    static constexpr auto LEGACY_SPIN_BUDGET = std::chrono::microseconds{200};
    static constexpr std::uint32_t SPIN_BUDGET_MULTIPLIER = 2;

    // Feedback ring from sampler → pace().
    struct FeedbackSample {
        std::uint64_t target_ns;
        std::uint64_t actual_ns;
    };
    static constexpr std::size_t FEEDBACK_CAP = 32;
    std::mutex feedback_mutex_;
    FeedbackSample feedback_ring_[FEEDBACK_CAP]{};
    std::size_t feedback_count_{0};
};

} // namespace low_latency

#endif
