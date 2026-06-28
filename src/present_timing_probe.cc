#include "present_timing_probe.hh"

#include "device_context.hh"

#include <vulkan/vulkan_core.h>

namespace low_latency {

DeviceClock::time_point absolute_to_device_time(std::uint64_t absolute_ns) {
    return DeviceClock::time_point{DeviceClock::duration{
        static_cast<DeviceClock::rep>(absolute_ns)}};
}

bool query_refresh_cycle(const DeviceContext& device, VkSwapchainKHR swapchain,
                         DeviceClock::duration& out_refresh, bool& out_vrr) {
    const auto mode = device.physical_device.present_timing_mode;
    if (mode == PresentTimingMode::google_display_timing) {
        if (!device.vtable.GetRefreshCycleDurationGOOGLE) {
            return false;
        }
        auto info = VkRefreshCycleDurationGOOGLE{};
        const auto result = device.vtable.GetRefreshCycleDurationGOOGLE(
            device.device, swapchain, &info);
        if (result != VK_SUCCESS) {
            return false;
        }
        out_refresh = DeviceClock::duration{
            static_cast<DeviceClock::rep>(info.refreshDuration)};
        // GOOGLE treats VRR as FRR per spec.
        out_vrr = false;
        return true;
    }
    if (mode == PresentTimingMode::ext_present_timing) {
        // vkGetSwapchainTimingPropertiesEXT isn't in the vku dispatch table.
        // Resolve via the dispatch table's GetDeviceProcAddr.
        if (!device.vtable.GetDeviceProcAddr) {
            return false;
        }
        const auto fn = reinterpret_cast<PFN_vkGetSwapchainTimingPropertiesEXT>(
            device.vtable.GetDeviceProcAddr(
                device.device, "vkGetSwapchainTimingPropertiesEXT"));
        if (!fn) {
            return false;
        }
        auto props = VkSwapchainTimingPropertiesEXT{};
        props.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_TIMING_PROPERTIES_EXT;
        auto counter = std::uint64_t{0};
        const auto result =
            fn(device.device, swapchain, &props, &counter);
        if (result != VK_SUCCESS) {
            return false;
        }
        // refreshDuration is the full period; refreshInterval is the granularity.
        // Use the full period for vblank grid math.
        out_refresh = DeviceClock::duration{
            static_cast<DeviceClock::rep>(props.refreshDuration)};
        // VkSwapchainTimingPropertiesEXT doesn't expose a VRR flag directly.
        // Detect VRR heuristically: refreshInterval == 0 (continuous) suggests VRR.
        out_vrr = (props.refreshInterval == 0);
        return true;
    }
    return false;
}

bool poll_past_presentation_timings(const DeviceContext& device,
                                    VkSwapchainKHR swapchain,
                                    const PresentFeedbackCallback& cb) {
    const auto mode = device.physical_device.present_timing_mode;
    if (mode == PresentTimingMode::google_display_timing) {
        if (!device.vtable.GetPastPresentationTimingGOOGLE) {
            return false;
        }
        auto count = std::uint32_t{0};
        if (device.vtable.GetPastPresentationTimingGOOGLE(
                device.device, swapchain, &count, nullptr) != VK_SUCCESS) {
            return false;
        }
        if (count == 0) {
            return false;
        }
        auto timings = std::vector<VkPastPresentationTimingGOOGLE>(count);
        if (device.vtable.GetPastPresentationTimingGOOGLE(
                device.device, swapchain, &count, timings.data()) != VK_SUCCESS) {
            return false;
        }
        for (const auto& t : timings) {
            cb(t.desiredPresentTime, t.actualPresentTime);
        }
        return true;
    }
    if (mode == PresentTimingMode::ext_present_timing) {
        if (!device.vtable.GetPastPresentationTimingEXT) {
            return false;
        }
        // EXT takes an info struct; we ask for all completed entries.
        auto info = VkPastPresentationTimingInfoEXT{};
        info.sType = VK_STRUCTURE_TYPE_PAST_PRESENTATION_TIMING_INFO_EXT;
        info.swapchain = swapchain;
        // First call to get count.
        auto props = VkPastPresentationTimingPropertiesEXT{};
        props.sType = VK_STRUCTURE_TYPE_PAST_PRESENTATION_TIMING_PROPERTIES_EXT;
        const auto first_result = device.vtable.GetPastPresentationTimingEXT(
            device.device, &info, &props);
        if (first_result != VK_SUCCESS) {
            return false;
        }
        const auto count = props.presentationTimingCount;
        if (count == 0) {
            return false;
        }
        // Re-call with buffer.
        std::vector<VkPastPresentationTimingEXT> entries(count);
        props = VkPastPresentationTimingPropertiesEXT{};
        props.sType = VK_STRUCTURE_TYPE_PAST_PRESENTATION_TIMING_PROPERTIES_EXT;
        props.presentationTimingCount = count;
        props.pPresentationTimings = entries.data();
        if (device.vtable.GetPastPresentationTimingEXT(device.device, &info,
                                                       &props) != VK_SUCCESS) {
            return false;
        }
        // EXT: actual present time is read from pPresentStages for the desired stage
        // (typically VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_VISIBLE_BIT_EXT). We requested
        // no stage queries, so pPresentStages is null; in that case the layer falls
        // back to using targetTime as a coarse "actual-ish" signal (drivers that
        // support the extension but no stage queries still report targetTime as
        // the actually-attempted present time). Treat as (target, target) — PI will
        // see 0 error and rely on the grid rebase from feedback re-anchor logic.
        for (const auto& t : entries) {
            std::uint64_t actual = t.targetTime;
            for (std::uint32_t i = 0; i < t.presentStageCount; ++i) {
                if (t.pPresentStages[i].stage &
                    VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_VISIBLE_BIT_EXT) {
                    actual = t.pPresentStages[i].time;
                    break;
                }
            }
            cb(t.targetTime, actual);
        }
        return true;
    }
    return false;
}

} // namespace low_latency
