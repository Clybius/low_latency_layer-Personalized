#include "device_strategy.hh"
#include "device_context.hh"
#include "display_deadline_pacer.hh"
#include "fps_limiter.hh"
#include "layer_context.hh"
#include "present_timing_probe.hh"
#include "presentation_pacer.hh"

#include "queue_strategy.hh"

#include <chrono>
#include <vulkan/vulkan_core.h>

namespace low_latency {

AntiLagDeviceStrategy::AntiLagDeviceStrategy(DeviceContext& device)
    : DeviceStrategy(device), pacer(make_presentation_pacer(device)) {}

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

    // Throttled past-presentation-timing poll. Drives the Tier 2 PI loop
    // by feeding (target, actual) pairs into the pacer. Run before the
    // pace() call below so the feedback is consumed this frame.
    if (this->current_swapchain_ != VK_NULL_HANDLE &&
        this->device.physical_device.present_timing_mode !=
            PresentTimingMode::none) {
        constexpr auto POLL_PERIOD = std::chrono::milliseconds{8};
        const auto now = DeviceClock::now();
        if (now - this->last_feedback_poll_ >= POLL_PERIOD) {
            this->last_feedback_poll_ = now;
            const auto swapchain = this->current_swapchain_;
            const auto& dev = this->device;
            auto* dlp = dynamic_cast<DisplayDeadlinePacer*>(this->pacer.get());
            (void)poll_past_presentation_timings(
                dev, swapchain,
                [dlp](std::uint64_t target_ns, std::uint64_t actual_ns) {
                    if (dlp) {
                        dlp->record_present_feedback(target_ns, actual_ns);
                    }
                });
        }
    }

    lock.unlock();

    // We need to collect all queue submission and wait on them in this thread.
    // Input stage needs to wait for all queue submissions to complete.
    const auto work = [&]() -> auto {
        auto work = std::vector<std::unique_ptr<SubmissionSpan>>{};
        const auto device_lock = std::shared_lock{this->device.mutex};
        for (const auto& iter : this->device.queues) {
            const auto& queue = iter.second;

            const auto strategy =
                dynamic_cast<AntiLagQueueStrategy*>(queue->strategy.get());
            assert(strategy);

            // Grab it from the queue, don't hold the lock.
            const auto queue_lock = std::scoped_lock{strategy->mutex};
            work.emplace_back(std::move(strategy->submission_span));
            strategy->submission_span.reset();
        }
        return work;
    }();

    // Wait on outstanding work to complete AND capture GPU work interval.
    // Previously we discarded the return value of await_completed() — now we
    // aggregate the (start, end) pair across all spans for the pacer.
    auto timing = FrameTiming{};
    timing.release_prev = this->release_prev;
    auto first = true;
    for (const auto& submission_span : work) {
        if (!submission_span) { // Can still be null here.
            continue;
        }
        const auto span_pair = submission_span->await_completed();
        if (first) {
            timing.gpu_start = span_pair.first;
            timing.gpu_end = span_pair.second;
            first = false;
        } else {
            if (span_pair.first < timing.gpu_start) {
                timing.gpu_start = span_pair.first;
            }
            if (span_pair.second > timing.gpu_end) {
                timing.gpu_end = span_pair.second;
            }
        }
    }

    const auto effective = effective_min_delay(
        std::chrono::duration_cast<std::chrono::nanoseconds>(min_delay),
        this->device.instance.layer.fps_limit);
    const auto release = this->pacer->pace(timing, effective);
    this->release_prev = release;
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

// Anti-Lag tracks the most recent swapchain so the past-presentation-timing
// poll (driven from notify_update) can query the driver. The pacer also
// learns about the swapchain for Tier 2 refresh-cycle / PI feedback.
void AntiLagDeviceStrategy::notify_create_swapchain(
    const VkSwapchainKHR& swapchain, const VkSwapchainCreateInfoKHR& /*info*/) {
    const auto lock = std::scoped_lock{this->mutex};
    this->current_swapchain_ = swapchain;
    if (this->pacer) {
        this->pacer->set_swapchain(swapchain);
    }
}

void AntiLagDeviceStrategy::notify_destroy_swapchain(
    const VkSwapchainKHR& swapchain) {
    const auto lock = std::scoped_lock{this->mutex};
    if (this->current_swapchain_ == swapchain) {
        this->current_swapchain_ = VK_NULL_HANDLE;
    }
}

void AntiLagDeviceStrategy::notify_acquire(
    const VkSwapchainKHR& /*swapchain*/, const DeviceClock::time_point& /*time*/) {
    // AL pacing is synchronous on the notify_update thread, so the acquire
    // signal isn't used to drive the pacer here. The hook is implemented
    // so that vkAcquireNextImageKHR is intercepted consistently across
    // both backends; future back-pressure detection can read this state.
}

} // namespace low_latency
