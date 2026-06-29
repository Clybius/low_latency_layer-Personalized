#include "device_strategy.hh"

#include "device_context.hh"
#include "fps_limiter.hh"
#include "layer_context.hh"
#include "monitor.hh"
#include "queue_strategy.hh"

#include <chrono>
#include <cstdio>
#include <vulkan/vulkan_core.h>

namespace low_latency {

AnywhereDeviceStrategy::AnywhereDeviceStrategy(DeviceContext& device)
    : DeviceStrategy(device),
      monitor(std::make_unique<AnywhereMonitor>(device)) {}

AnywhereDeviceStrategy::~AnywhereDeviceStrategy() {}

void AnywhereDeviceStrategy::notify_create_swapchain(
    const VkSwapchainKHR& swapchain,
    const VkSwapchainCreateInfoKHR& info) {
    const auto lock = std::scoped_lock{this->mutex};
    this->current_swapchain_ = swapchain;
    this->present_mode_ = info.presentMode;
    if (monitor) {
        monitor->set_swapchain(swapchain);
        monitor->set_present_mode(info.presentMode);
    }
    if (this->device.instance.layer.should_debug) {
        std::fprintf(stderr, "[LowLatency] Anywhere: notify_create_swapchain swapchain=%p mode=%d\n",
                     static_cast<void*>(swapchain), static_cast<int>(info.presentMode));
    }
}

void AnywhereDeviceStrategy::notify_destroy_swapchain(
    const VkSwapchainKHR& swapchain) {
    const auto lock = std::scoped_lock{this->mutex};
    if (this->current_swapchain_ == swapchain) {
        this->current_swapchain_ = VK_NULL_HANDLE;
        if (monitor) {
            monitor->set_swapchain(VK_NULL_HANDLE);
        }
    }
}

void AnywhereDeviceStrategy::notify_acquire(
    const VkSwapchainKHR& /*swapchain*/,
    const DeviceClock::time_point& /*time*/) {
    // The acquire signal isn't currently used by the async monitor path.
}

void AnywhereDeviceStrategy::notify_present(
    const VkPresentInfoKHR& /*present*/) {

    // Collect submission spans from all queues. This happens on the app
    // thread (after the driver present call returns). We enqueue to the
    // monitor and return — GPU completion wait, feedback polling, and
    // pacing sleep all happen asynchronously on the monitor thread.
    auto work = [&]() -> auto {
        auto work = std::vector<std::unique_ptr<SubmissionSpan>>{};
        const auto device_lock = std::shared_lock{this->device.mutex};
        for (const auto& iter : this->device.queues) {
            const auto& queue = iter.second;

            // Only track graphics queues to avoid pulling async compute/
            // transfer work into our timings.
            if (!(queue->properties.queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
                continue;
            }

            const auto strategy =
                dynamic_cast<AnywhereQueueStrategy*>(queue->strategy.get());
            assert(strategy);

            const auto queue_lock = std::scoped_lock{strategy->mutex};
            if (strategy->submission_span) {
                work.emplace_back(std::move(strategy->submission_span));
                strategy->submission_span.reset();
            }
        }
        return work;
    }();

    // If there is no GPU work to pace, skip pacing for this frame.
    if (work.empty()) {
        if (this->device.instance.layer.should_debug) {
            std::fprintf(stderr, "[LowLatency] Anywhere: notify_present — no spans, skipping pace\n");
        }
        return;
    }

    // Hand off to the monitor. enqueue_work blocks if the monitor is
    // still processing the previous frame (back-pressure), then wakes
    // the monitor and returns.
    const auto effective = effective_min_delay(
        std::chrono::nanoseconds{0}, // no app-supplied min delay
        this->device.instance.layer.fps_limit);
    if (this->device.instance.layer.should_debug) {
        std::fprintf(stderr, "[LowLatency] Anywhere: notify_present — %zu span(s), enqueueing to monitor\n",
                     work.size());
    }
    monitor->enqueue_work(std::move(work), effective);
}

} // namespace low_latency
