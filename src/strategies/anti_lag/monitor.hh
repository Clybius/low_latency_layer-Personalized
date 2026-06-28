#ifndef STRATEGIES_ANTI_LAG_MONITOR_HH_
#define STRATEGIES_ANTI_LAG_MONITOR_HH_

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

// Per-device monitor thread that handles pacing asynchronously for the
// VK_AMD_anti_lag path. Ports the SwapchainMonitor (NV Reflex) thread model
// to Anti-Lag: GPU completion waiting, feedback polling, and pacing-run
// sleep all happen on a dedicated thread so AntiLagUpdateAMD can return
// immediately. Queue depth is limited to 1 via internal back-pressure —
// the next AntiLagUpdateAMD INPUT call blocks until the previous frame's
// pacing cycle has finished.
class AntiLagMonitor final {
  private:
    const DeviceContext& device;
    std::unique_ptr<PresentationPacer> pacer;

    std::mutex mutex;
    std::condition_variable_any cv;

    // Single-frame work pipeline. The monitor processes one frame at a
    // time; work_in_progress gates the next enqueue.
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
    explicit AntiLagMonitor(const DeviceContext& device);
    AntiLagMonitor(const AntiLagMonitor&) = delete;
    AntiLagMonitor(AntiLagMonitor&&) = delete;
    AntiLagMonitor& operator=(const AntiLagMonitor&) = delete;
    AntiLagMonitor& operator=(AntiLagMonitor&&) = delete;
    ~AntiLagMonitor();

    // Queue work for the monitor thread. Blocks the caller if the monitor
    // is still processing the previous frame (back-pressure). Returns
    // after waking the monitor — the caller can overlap CPU work on the
    // next frame while GPU completion and pacing happen asynchronously.
    void enqueue_work(std::vector<std::unique_ptr<SubmissionSpan>> spans,
                      const DeviceClock::duration& min_delay);

    void set_swapchain(VkSwapchainKHR sc);
    void set_present_mode(VkPresentModeKHR mode);
    bool feed_present_feedback(std::uint64_t target_ns,
                               std::uint64_t actual_ns);
};

} // namespace low_latency

#endif
