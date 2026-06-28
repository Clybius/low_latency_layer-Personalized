#ifndef PRESENT_TIMING_PROBE_HH_
#define PRESENT_TIMING_PROBE_HH_

#include "device_clock.hh"
#include "physical_device_context.hh"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <functional>

namespace low_latency {

class DeviceContext;

// Query the refresh cycle for a swapchain. Returns false on failure.
// On success fills out_refresh and out_vrr.
bool query_refresh_cycle(const DeviceContext& device, VkSwapchainKHR swapchain,
                         DeviceClock::duration& out_refresh, bool& out_vrr);

// Convert a Vulkan absolute timestamp (e.g. VkPastPresentationTimingEXT::actualPresentTime,
// VkPastPresentationTimingGOOGLE::actualPresentTime) into a DeviceClock::time_point.
// VK_EXT_present_timing uses a device-specific time domain; the layer assumes the
// host CLOCK_MONOTONIC domain (matches DeviceClock's calibration).
// VK_GOOGLE_display_timing uses CLOCK_MONOTONIC.
DeviceClock::time_point absolute_to_device_time(std::uint64_t absolute_ns);

// Poll vkGetPastPresentationTiming(EXT|GOOGLE) for feedback samples.
// Returns true if at least one sample was read into the callback.
// `cb` is invoked once per sample with (target_ns, actual_ns). target_ns may be 0
// if the application didn't set a target (caller can ignore).
using PresentFeedbackCallback = std::function<void(std::uint64_t target_ns,
                                                  std::uint64_t actual_ns)>;
bool poll_past_presentation_timings(const DeviceContext& device,
                                    VkSwapchainKHR swapchain,
                                    const PresentFeedbackCallback& cb);

} // namespace low_latency

#endif
