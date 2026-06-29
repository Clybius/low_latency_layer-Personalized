#ifndef STRATEGIES_ANYWHERE_MONITOR_HH_
#define STRATEGIES_ANYWHERE_MONITOR_HH_

#include "device_clock.hh"
#include "submission_span.hh"

#include <vulkan/vulkan.h>

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace low_latency {

class DeviceContext;
class PresentationPacer;
class DisplayDeadlinePacer;

// Per-device async pacing monitor for ANYWHERE mode. Structurally identical
// to AntiLagMonitor: owns a dedicated jthread that awaits GPU completion,
// runs the PresentationPacer, and handles past-presentation-timing feedback
// polling. Queue depth is limited to 1 via internal back-pressure — the next
// enqueue_work call blocks until the previous frame's pacing cycle finishes.
class AnywhereMonitor final {
  private:
    const DeviceContext& device;
    std::unique_ptr<PresentationPacer> pacer;

    std::mutex mutex;
    std::condition_variable_any cv;

    // Single-frame work pipeline. Processes one frame at a time;
    // work_in_progress gates the next enqueue.
    std::optional<std::vector<std::unique_ptr<SubmissionSpan>>> pending_spans;
    DeviceClock::duration min_delay{};
    bool work_in_progress{false};

    DeviceClock::time_point release_prev{};

    DeviceClock::time_point last_feedback_poll{};
    VkSwapchainKHR swapchain{VK_NULL_HANDLE};

    std::jthread monitor_worker;
    void do_monitor(std::stop_token stoken);
    void poll_feedback_locked();

  public:
    explicit AnywhereMonitor(const DeviceContext& device);
    AnywhereMonitor(const AnywhereMonitor&) = delete;
    AnywhereMonitor(AnywhereMonitor&&) = delete;
    AnywhereMonitor& operator=(const AnywhereMonitor&) = delete;
    AnywhereMonitor& operator=(AnywhereMonitor&&) = delete;
    ~AnywhereMonitor();

    void enqueue_work(std::vector<std::unique_ptr<SubmissionSpan>> spans,
                      const DeviceClock::duration& min_delay);

    void set_swapchain(VkSwapchainKHR sc);
    void set_present_mode(VkPresentModeKHR mode);
    bool feed_present_feedback(std::uint64_t target_ns,
                               std::uint64_t actual_ns);
};

} // namespace low_latency

#endif
