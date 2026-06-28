#include "device_strategy.hh"
#include "device_context.hh"
#include "fps_limiter.hh"
#include "layer_context.hh"
#include "monitor.hh"

#include "queue_strategy.hh"

#include <chrono>
#include <vulkan/vulkan_core.h>

namespace low_latency {

AntiLagDeviceStrategy::AntiLagDeviceStrategy(DeviceContext& device)
    : DeviceStrategy(device),
      monitor(std::make_unique<AntiLagMonitor>(device)) {}

AntiLagDeviceStrategy::~AntiLagDeviceStrategy() {}

void AntiLagDeviceStrategy::notify_update(const VkAntiLagDataAMD& data) {
    auto lock = std::unique_lock{this->mutex};

    this->is_enabled = data.mode != VK_ANTI_LAG_MODE_OFF_AMD;
    const auto min_delay = [&]() -> std::chrono::microseconds {
        using namespace std::chrono;
        if (!data.maxFPS) {
            return 0us;
        }
        return duration_cast<microseconds>(1s) / data.maxFPS;
    }();

    if (!data.pPresentationInfo || !is_enabled) {
        return;
    }

    // If we're at the present stage, stop collecting submissions by making
    // our frame_index nullopt.
    if (data.pPresentationInfo->stage == VK_ANTI_LAG_STAGE_PRESENT_AMD) {
        this->frame_index.reset();
        return;
    }
    // If we're at the input stage, start marking submissions as relevant.
    this->frame_index.emplace(data.pPresentationInfo->frameIndex);

    lock.unlock();

    // Collect submission spans from all queues. This happens on the app
    // thread (same as before), but we no longer await GPU completion or
    // run the pacer here. Instead we enqueue to the monitor and return
    // immediately so the app can overlap CPU work on the next frame.
    auto work = [&]() -> auto {
        auto work = std::vector<std::unique_ptr<SubmissionSpan>>{};
        const auto device_lock = std::shared_lock{this->device.mutex};
        for (const auto& iter : this->device.queues) {
            const auto& queue = iter.second;

            const auto strategy =
                dynamic_cast<AntiLagQueueStrategy*>(queue->strategy.get());
            assert(strategy);

            const auto queue_lock = std::scoped_lock{strategy->mutex};
            work.emplace_back(std::move(strategy->submission_span));
            strategy->submission_span.reset();
        }
        return work;
    }();

    // Hand off to the monitor. enqueue_work blocks if the monitor is
    // still processing the previous frame (back-pressure), then wakes
    // the monitor and returns. GPU completion wait, feedback polling,
    // and pacing sleep all happen asynchronously on the monitor thread.
    const auto effective = effective_min_delay(
        std::chrono::duration_cast<std::chrono::nanoseconds>(min_delay),
        this->device.instance.layer.fps_limit);
    monitor->enqueue_work(std::move(work), effective);
}

bool AntiLagDeviceStrategy::should_track_submissions() {
    const auto lock = std::shared_lock{this->mutex};

    if (!this->is_enabled) {
        return false;
    }

    // Don't track submissions if our frame index is nullopt!
    if (!this->frame_index.has_value()) {
        return false;
    }

    return true;
}

void AntiLagDeviceStrategy::notify_create_swapchain(
    const VkSwapchainKHR& swapchain, const VkSwapchainCreateInfoKHR& /*info*/) {
    const auto lock = std::scoped_lock{this->mutex};
    this->current_swapchain_ = swapchain;
    if (monitor) {
        monitor->set_swapchain(swapchain);
    }
}

void AntiLagDeviceStrategy::notify_destroy_swapchain(
    const VkSwapchainKHR& swapchain) {
    const auto lock = std::scoped_lock{this->mutex};
    if (this->current_swapchain_ == swapchain) {
        this->current_swapchain_ = VK_NULL_HANDLE;
        if (monitor) {
            monitor->set_swapchain(VK_NULL_HANDLE);
        }
    }
}

void AntiLagDeviceStrategy::notify_acquire(
    const VkSwapchainKHR& /*swapchain*/, const DeviceClock::time_point& /*time*/) {
    // The acquire signal isn't currently used by the async monitor path.
    // The hook is kept so vkAcquireNextImageKHR is intercepted consistently
    // across both backends; future back-pressure detection can read this state.
}

} // namespace low_latency
