#include "monitor.hh"

#include "device_context.hh"
#include "display_deadline_pacer.hh"
#include "physical_device_context.hh"
#include "present_timing_probe.hh"
#include "presentation_pacer.hh"

#include <chrono>
#include <functional>

namespace low_latency {

AntiLagMonitor::AntiLagMonitor(const DeviceContext& device)
    : device(device), pacer(make_presentation_pacer(device)),
      monitor_worker(std::bind_front(&AntiLagMonitor::do_monitor, this)) {}

AntiLagMonitor::~AntiLagMonitor() {}

void AntiLagMonitor::enqueue_work(
    std::vector<std::unique_ptr<SubmissionSpan>> spans,
    const DeviceClock::duration& min_delay) {

    // Back-pressure: block until the monitor has finished processing the
    // previous frame. This prevents queue-stuffing.
    {
        auto lock = std::unique_lock{mutex};
        cv.wait(lock, [this] { return !work_in_progress; });
    }

    {
        const auto lock = std::scoped_lock{mutex};
        pending_spans = std::move(spans);
        this->min_delay = min_delay;
        work_in_progress = true;
    }
    cv.notify_one();
}

void AntiLagMonitor::do_monitor(const std::stop_token stoken) {
    for (;;) {
        auto lock = std::unique_lock{mutex};
        cv.wait(lock, stoken,
                [this] { return pending_spans.has_value(); });

        if (stoken.stop_requested() && !pending_spans.has_value()) {
            break;
        }

        auto spans = std::move(*pending_spans);
        pending_spans.reset();
        auto delay = this->min_delay;
        lock.unlock();

        // Await GPU completion on each span and aggregate into a single
        // FrameTiming (min start, max end across all queue contributions).
        auto timing = FrameTiming{};
        timing.release_prev = release_prev;
        auto first = true;
        for (const auto& span : spans) {
            if (!span) {
                continue;
            }
            const auto span_pair = span->await_completed();
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

        // Throttled past-presentation-timing poll. Drives the Tier 2 PI
        // loop by feeding (target, actual) pairs into the pacer.
        poll_feedback_locked();

        // Run the pacer (which includes the adaptive sleep internally).
        release_prev = pacer->pace(timing, delay);

        // Signal completion so the next enqueue_work can proceed.
        {
            const auto lock = std::scoped_lock{mutex};
            work_in_progress = false;
        }
        cv.notify_all();
    }
}

void AntiLagMonitor::poll_feedback_locked() {
    constexpr auto POLL_PERIOD = std::chrono::milliseconds{8};
    const auto now = DeviceClock::now();
    if (now - last_feedback_poll < POLL_PERIOD) {
        return;
    }
    last_feedback_poll = now;

    if (device.physical_device.present_timing_mode == PresentTimingMode::none) {
        return;
    }
    if (swapchain == VK_NULL_HANDLE) {
        return;
    }

    auto* dlp = dynamic_cast<DisplayDeadlinePacer*>(pacer.get());
    (void)poll_past_presentation_timings(
        device, swapchain,
        [this, dlp](std::uint64_t target_ns, std::uint64_t actual_ns) {
            if (dlp) {
                dlp->record_present_feedback(target_ns, actual_ns);
            }
        });
}

void AntiLagMonitor::set_swapchain(VkSwapchainKHR sc) {
    swapchain = sc;
    pacer->set_swapchain(sc);
}

void AntiLagMonitor::set_present_mode(VkPresentModeKHR mode) {
    pacer->set_present_mode(mode);
}

bool AntiLagMonitor::feed_present_feedback(std::uint64_t target_ns,
                                           std::uint64_t actual_ns) {
    if (auto* dlp = dynamic_cast<DisplayDeadlinePacer*>(pacer.get())) {
        dlp->record_present_feedback(target_ns, actual_ns);
        return true;
    }
    return false;
}

} // namespace low_latency
