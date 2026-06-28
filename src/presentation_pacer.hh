#ifndef PRESENTATION_PACER_HH_
#define PRESENTATION_PACER_HH_

#include "device_clock.hh"

#include <vulkan/vulkan.h>

#include <memory>

namespace low_latency {

// Per-frame timing data passed into the pacer. Carries the GPU work interval
// (aggregated from the frame's SubmissionSpans) and the previous release point.
// skip_next_release is an out-parameter the pacer may set when it detected a
// hitch > 2.5x the predicted render time; the caller uses it to release the
// next frame immediately rather than waiting for the next full interval.
struct FrameTiming final {
    DeviceClock::time_point gpu_start{};
    DeviceClock::time_point gpu_end{};
    DeviceClock::time_point release_prev{};
    bool skip_next_release{false};
};

// The PresentationPacer decides when to release a frame after its GPU work
// has completed. It is app-agnostic — pacing the observable render queue
// depth directly using GPU timestamps we already collect.
//
// Two backends:
//   - PredictiveQueuePacer (Tier 1, always available): just-in-time release
//     so the next frame's GPU work starts as the previous frame's GPU work
//     ends. EWMA-driven, closed loop on observed CPU release-to-GPU-start lag.
//   - DisplayDeadlinePacer (Tier 2, optional): vblank-anchored PI loop using
//     VK_EXT_present_timing / VK_GOOGLE_display_timing when available.
class PresentationPacer {
  public:
    virtual ~PresentationPacer() = default;
    PresentationPacer() = default;
    PresentationPacer(const PresentationPacer&) = delete;
    PresentationPacer(PresentationPacer&&) = delete;
    PresentationPacer& operator=(const PresentationPacer&) = delete;
    PresentationPacer& operator=(PresentationPacer&&) = delete;

    // Called once per frame AFTER the frame's SubmissionSpans have been awaited.
    // Returns the chosen release time. Caller sleeps until max(now, release)
    // then signals (NV) / returns (AMD). The returned value becomes the next
    // frame's release_prev.
    virtual DeviceClock::time_point pace(
        const FrameTiming& timing,
        const DeviceClock::duration& min_delay) = 0;

    // Bind a swapchain so Tier 2 can lazily query its refresh cycle. Default
    // is a no-op for Tier 1. Returns true if the pacer accepted the swapchain
    // (i.e. is Tier 2); false otherwise.
    virtual bool set_swapchain(VkSwapchainKHR /*swapchain*/) { return false; }

    // Set the present mode of the swapchain. Used by Tier 2 to skip the
    // vblank grid for MAILBOX/IMMEDIATE (no fixed vblank to align to).
    // Default is a no-op for Tier 1.
    virtual void set_present_mode(VkPresentModeKHR /*mode*/) {}
};

class DeviceContext;

// Factory: selects backend based on PhysicalDeviceContext::present_timing_mode.
// Defined in presentation_pacer.cc.
std::unique_ptr<PresentationPacer>
make_presentation_pacer(const DeviceContext& device);

} // namespace low_latency

#endif
