#include "swapchain_monitor.hh"
#include "device_context.hh"
#include "display_deadline_pacer.hh"
#include "fps_limiter.hh"
#include "layer_context.hh"
#include "present_timing_probe.hh"
#include "presentation_pacer.hh"

#include <chrono>
#include <functional>

namespace low_latency {

SwapchainMonitor::SwapchainMonitor(const DeviceContext& device)
    : device(device), pacer(make_presentation_pacer(device)),
      monitor_worker(std::bind_front(&SwapchainMonitor::do_monitor, this)) {}

SwapchainMonitor::~SwapchainMonitor() {}

void SwapchainMonitor::update_params(const bool was_low_latency_requested,
                                     const std::chrono::microseconds delay) {

    const auto lock = std::scoped_lock{this->mutex};

    this->was_low_latency_requested = was_low_latency_requested;
    this->present_delay = delay;
}

void SwapchainMonitor::do_monitor(const std::stop_token stoken) {
    for (;;) {
        auto lock = std::unique_lock{this->mutex};
        this->cv.wait(lock, stoken,
                      [&]() { return !this->pending_signals.empty(); });

        // Stop only if we're stopped and we have nothing to signal.
        if (stoken.stop_requested() && this->pending_signals.empty()) {
            break;
        }

        // Grab the most recent semaphore. When work completes, signal it.
        const auto pending_signal = std::move(this->pending_signals.front());
        this->pending_signals.pop_front();

        // If we're stopping, signal the semaphore and don't worry about work
        // actually completing. But we MUST drain them, or we get a hang.
        if (stoken.stop_requested()) {
            pending_signal.semaphore_signal.signal(this->device);
            continue;
        }

        // Grab mutex protected present delay before we sleep - doesn't matter
        // if it's 'old'.
        const auto delay = this->present_delay;
        lock.unlock();

        // Wait for work to complete AND capture the GPU work interval.
        // Previously we discarded the return value of await_completed() — now
        // we aggregate the (start, end) pair across all spans for the pacer.
        auto timing = FrameTiming{};
        timing.release_prev = this->release_prev;
        auto first = true;
        for (const auto& submission_span : pending_signal.submission_spans) {
            if (!submission_span) {
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

        // Throttled past-presentation-timing poll. Drives the Tier 2 PI loop
        // by feeding (target, actual) pairs into the pacer. The poll runs on
        // the monitor thread (not the app thread) because the driver may do
        // work to gather past timings and we don't want to add latency to the
        // app's present call.
        this->poll_feedback_locked();

        // Don't need to worry about locking for the pacer as it's only
        // accessed here.
        // Apply the layer-imposed FPS cap (max with the app's present_delay).
        const auto effective = effective_min_delay(
            std::chrono::duration_cast<std::chrono::nanoseconds>(delay),
            this->device.instance.layer.fps_limit);
        const auto release = this->pacer->pace(timing, effective);
        this->release_prev = release;

        pending_signal.semaphore_signal.signal(this->device);
    }
}

void SwapchainMonitor::attach_swapchain(VkSwapchainKHR swapchain) {
    // Forwards to pacer; no-op for Tier 1, sets up refresh cycle for Tier 2.
    // The first non-null swapchain wins; subsequent calls are idempotent.
    this->pacer->set_swapchain(swapchain);
    this->pacer->set_present_mode(this->present_mode);
}

void SwapchainMonitor::set_present_mode(VkPresentModeKHR mode) {
    this->present_mode = mode;
    this->pacer->set_present_mode(mode);
}

void SwapchainMonitor::record_acquire(const DeviceClock::time_point& time) {
    const auto lock = std::scoped_lock{this->acquire_mutex};
    this->last_acquire_time_ = time;
    this->has_acquire_ = true;
}

std::optional<DeviceClock::time_point>
SwapchainMonitor::last_acquire() const {
    const auto lock = std::scoped_lock{this->acquire_mutex};
    if (!this->has_acquire_) {
        return std::nullopt;
    }
    return this->last_acquire_time_;
}

void SwapchainMonitor::poll_feedback_locked() {
    constexpr auto POLL_PERIOD = std::chrono::milliseconds{8};
    const auto now = DeviceClock::now();
    if (now - this->last_feedback_poll < POLL_PERIOD) {
        return;
    }
    this->last_feedback_poll = now;

    auto* dlp = dynamic_cast<DisplayDeadlinePacer*>(this->pacer.get());
    if (!dlp) {
        return;
    }
    const auto swapchain = dlp->swapchain_handle();
    if (swapchain == VK_NULL_HANDLE) {
        return;
    }
    (void)poll_past_presentation_timings(
        this->device, swapchain,
        [this](std::uint64_t target_ns, std::uint64_t actual_ns) {
            this->feed_present_feedback(target_ns, actual_ns);
        });
}

std::optional<DeviceClock::time_point>
SwapchainMonitor::get_target_present() const {
    auto* dlp = dynamic_cast<const DisplayDeadlinePacer*>(this->pacer.get());
    if (!dlp) {
        return std::nullopt;
    }
    return dlp->current_target_present();
}

void SwapchainMonitor::notify_semaphore(
    const SemaphoreSignal& semaphore_signal) {

    auto lock = std::unique_lock{this->mutex};

    // Signal immediately if reflex is off or it's a no-op submit.
    if (!this->was_low_latency_requested) {
        semaphore_signal.signal(this->device);
        return;
    }

    this->pending_signals.emplace_back(PendingSignal{
        .semaphore_signal = semaphore_signal,
        .submission_spans = std::move(this->pending_submission_spans),
    });
    this->pending_submission_spans.clear();

    lock.unlock();
    this->cv.notify_one();
}

void SwapchainMonitor::attach_work(
    std::vector<std::unique_ptr<SubmissionSpan>> submission_spans) {

    const auto lock = std::scoped_lock{this->mutex};
    if (!this->was_low_latency_requested) {
        return;
    }
    this->pending_submission_spans = std::move(submission_spans);
}

bool SwapchainMonitor::feed_present_feedback(std::uint64_t target_ns,
                                             std::uint64_t actual_ns) {
    if (auto* dlp = dynamic_cast<DisplayDeadlinePacer*>(this->pacer.get())) {
        dlp->record_present_feedback(target_ns, actual_ns);
        return true;
    }
    return false;
}

} // namespace low_latency
