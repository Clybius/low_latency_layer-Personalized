#include "device_strategy.hh"
#include "device_context.hh"
#include "queue_strategy.hh"

#include <chrono>
#include <mutex>
#include <vulkan/utility/vk_struct_helper.hpp>
#include <vulkan/vulkan_core.h>

namespace low_latency {

LowLatency2DeviceStrategy::LowLatency2DeviceStrategy(DeviceContext& device)
    : DeviceStrategy(device) {}

LowLatency2DeviceStrategy::~LowLatency2DeviceStrategy() {}

void LowLatency2DeviceStrategy::notify_create_swapchain(
    const VkSwapchainKHR& swapchain, const VkSwapchainCreateInfoKHR& info) {

    // VK_NV_low_latency2 allows a swapchain to be created with the low latency
    // mode already on via VkSwapchainLatencyCreateInfoNV.
    // Default to enabled - if the app is using VK_NV_low_latency2 at all it
    // wants pacing. VkSwapchainLatencyCreateInfoNV can override this, but
    // apps like CS2 recreate swapchains without it (apparent app bug).
    auto was_low_latency_requested = true;

    if (const auto slci =
            vku::FindStructInPNextChain<VkSwapchainLatencyCreateInfoNV>(
                info.pNext);
        slci) {

        was_low_latency_requested = slci->latencyModeEnable;
    }

    const auto lock = std::scoped_lock{this->mutex};
    const auto iter = this->swapchain_monitors.emplace(swapchain, this->device);
    iter.first->second.update_params(was_low_latency_requested,
                                     std::chrono::microseconds{0});
    iter.first->second.attach_swapchain(swapchain);
    iter.first->second.set_present_mode(info.presentMode);
}

void LowLatency2DeviceStrategy::notify_destroy_swapchain(
    const VkSwapchainKHR& swapchain) {

    const auto lock = std::scoped_lock{this->mutex};

    this->swapchain_monitors.erase(swapchain);
}

void LowLatency2DeviceStrategy::notify_latency_sleep_mode(
    const VkSwapchainKHR& swapchain,
    const VkLatencySleepModeInfoNV* const info) {

    const auto lock = std::shared_lock{this->mutex};

    const auto iter = this->swapchain_monitors.find(swapchain);
    if (iter == std::end(this->swapchain_monitors)) {
        return;
    }

    using namespace std::chrono;
    if (info) {
        iter->second.update_params(info->lowLatencyMode,
                                   microseconds{info->minimumIntervalUs});
    } else {
        iter->second.update_params(false, 0us);
    }
}

void LowLatency2DeviceStrategy::submit_swapchain_present_id(
    const VkSwapchainKHR& swapchain, const std::uint64_t& present_id) {

    // Iterate through all queues and grab any work that's associated with this
    // present_id. Turn it into a vector of work that we give to our swapchain
    // monitor.
    auto work = [&]() -> std::vector<std::unique_ptr<SubmissionSpan>> {
        auto work = std::vector<std::unique_ptr<SubmissionSpan>>{};
        const auto lock = std::shared_lock{this->device.mutex};
        for (const auto& queue_iter : this->device.queues) {
            const auto& queue = queue_iter.second;

            const auto strategy =
                dynamic_cast<LowLatency2QueueStrategy*>(queue->strategy.get());
            assert(strategy);

            if (strategy->is_out_of_band.load(std::memory_order::relaxed)) {
                continue;
            }

            // Need the lock now - we're modifying it.
            const auto strategy_lock = std::unique_lock{strategy->mutex};
            const auto iter = strategy->submission_spans.find(present_id);
            if (iter == std::end(strategy->submission_spans)) {
                continue;
            }

            // Make sure we clean it up from the present as well.
            work.push_back(std::move(iter->second));
            strategy->submission_spans.erase(iter);
        }
        return work;
    }();

    const auto lock = std::scoped_lock{this->mutex};
    const auto iter = this->swapchain_monitors.find(swapchain);
    if (iter == std::end(this->swapchain_monitors)) {
        return;
    }
    // Notify our monitor that this work has to be completed before they signal
    // whatever semaphore is currently sitting in it.
    iter->second.attach_work(std::move(work));
}

void LowLatency2DeviceStrategy::notify_latency_sleep_nv(
    const VkSwapchainKHR& swapchain, const VkLatencySleepInfoNV& info) {

    const auto lock = std::scoped_lock{this->mutex};

    const auto semaphore_signal =
        SemaphoreSignal{info.signalSemaphore, info.value};

    const auto iter = this->swapchain_monitors.find(swapchain);
    if (iter == std::end(this->swapchain_monitors)) {
        // If we can't find the swapchain we have to signal the semaphore
        // anyway. We must *never* discard these semaphores without signalling
        // them first.
        semaphore_signal.signal(this->device);
        return;
    }
    iter->second.notify_semaphore(semaphore_signal);
}

void LowLatency2DeviceStrategy::notify_acquire(
    const VkSwapchainKHR& swapchain, const DeviceClock::time_point& time) {
    const auto lock = std::shared_lock{this->mutex};
    const auto iter = this->swapchain_monitors.find(swapchain);
    if (iter == std::end(this->swapchain_monitors)) {
        return;
    }
    iter->second.record_acquire(time);
}

std::optional<DeviceClock::time_point>
LowLatency2DeviceStrategy::get_target_present(
    const VkSwapchainKHR& swapchain) const {
    const auto lock = std::shared_lock{this->mutex};
    const auto iter = this->swapchain_monitors.find(swapchain);
    if (iter == std::end(this->swapchain_monitors)) {
        return std::nullopt;
    }
    return iter->second.get_target_present();
}

FrameMarkers& LowLatency2DeviceStrategy::get_or_create_marker_record(
    std::uint64_t present_id) {
    for (auto& rec : this->marker_ring_) {
        if (rec.present_id == present_id) {
            return rec;
        }
    }
    auto rec = FrameMarkers{};
    rec.present_id = present_id;
    this->marker_ring_.push_back(rec);
    while (this->marker_ring_.size() > MARKER_RING_SIZE) {
        this->marker_ring_.pop_front();
    }
    return this->marker_ring_.back();
}

void LowLatency2DeviceStrategy::notify_set_latency_marker(
    std::uint64_t present_id, VkLatencyMarkerNV marker) {
    const auto marker_idx = static_cast<std::size_t>(marker);
    if (marker_idx >= 8) {
        return;
    }
    // Out-of-band markers (8+) and TRIGGER_FLASH (7) are not stored.
    if (marker == VK_LATENCY_MARKER_TRIGGER_FLASH_NV) {
        return;
    }

    const auto now_us = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            DeviceClock::now().time_since_epoch())
            .count());

    const auto lock = std::scoped_lock{this->marker_mutex_};
    auto& rec = this->get_or_create_marker_record(present_id);
    rec.marker_us[marker_idx] = now_us;

    // Update the per-stage EWMA for diagnostic purposes. We use the
    // inter-marker interval (the time since the previous marker on the
    // same presentID) so the EWMA tracks each pipeline stage's duration.
    if (marker_idx > 0 && marker_idx < STAGE_EWMA_COUNT) {
        const auto prev_us = rec.marker_us[marker_idx - 1];
        if (prev_us != 0 && now_us > prev_us) {
            const auto interval = static_cast<double>(now_us - prev_us);
            if (this->stage_ewma_seeded[marker_idx]) {
                this->stage_ewma_us[marker_idx] =
                    STAGE_EWMA_ALPHA * interval +
                    (1.0 - STAGE_EWMA_ALPHA) * this->stage_ewma_us[marker_idx];
            } else {
                this->stage_ewma_us[marker_idx] = interval;
                this->stage_ewma_seeded[marker_idx] = true;
            }
        }
    }
}

std::uint32_t LowLatency2DeviceStrategy::copy_latency_timings(
    const VkLatencyTimingsFrameReportNV* in_reports,
    std::uint32_t count,
    VkLatencyTimingsFrameReportNV* out_reports) {
    if (in_reports == nullptr || out_reports == nullptr || count == 0) {
        return 0;
    }
    const auto lock = std::scoped_lock{this->marker_mutex_};
    auto written = std::uint32_t{0};
    for (std::uint32_t i = 0; i < count; ++i) {
        out_reports[i] = in_reports[i];
        const auto present_id = in_reports[i].presentID;
        for (const auto& rec : this->marker_ring_) {
            if (rec.present_id != present_id) {
                continue;
            }
            out_reports[i].inputSampleTimeUs = rec.marker_us[0];
            out_reports[i].simStartTimeUs = rec.marker_us[1];
            out_reports[i].simEndTimeUs = rec.marker_us[2];
            out_reports[i].renderSubmitStartTimeUs = rec.marker_us[3];
            out_reports[i].renderSubmitEndTimeUs = rec.marker_us[4];
            out_reports[i].presentStartTimeUs = rec.marker_us[5];
            out_reports[i].presentEndTimeUs = rec.marker_us[6];
            // gpuRenderStartTimeUs/End are set by the GPU timestamp spans
            // when this presentID's work is awaited (see SwapchainMonitor).
            // We don't have them here — leave as 0.
            break;
        }
        ++written;
    }
    return written;
}

} // namespace low_latency
