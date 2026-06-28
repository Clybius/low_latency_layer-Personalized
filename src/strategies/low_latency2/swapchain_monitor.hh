
#ifndef SWAPCHAIN_MONITOR_HH_
#define SWAPCHAIN_MONITOR_HH_

#include "semaphore_signal.hh"
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

class SwapchainMonitor final {
  private:
    std::vector<std::unique_ptr<SubmissionSpan>> pending_submission_spans{};

    struct PendingSignal final {
        SemaphoreSignal semaphore_signal;
        std::vector<std::unique_ptr<SubmissionSpan>> submission_spans{};
    };
    std::deque<PendingSignal> pending_signals{};

    const DeviceContext& device;

    std::unique_ptr<PresentationPacer> pacer;

    // Present mode of the swapchain. Set in update_params from the
    // VkSwapchainCreateInfoKHR passed to notify_create_swapchain. Used by
    // DisplayDeadlinePacer to skip the vblank grid for MAILBOX/IMMEDIATE
    // (where there is no fixed vblank to align to).
    VkPresentModeKHR present_mode{VK_PRESENT_MODE_FIFO_KHR};

    // Held frame-to-frame across pace() calls (PredictiveQueuePacer also holds
    // its own, but this is what gets passed in; DisplayDeadlinePacer needs
    // access to refresh cycle / PI feedback so we wire it in at construction).
    DeviceClock::time_point release_prev{};

    std::mutex mutex{};
    std::chrono::microseconds present_delay{};
    bool was_low_latency_requested{};

    // Last time we polled past presentation timings from the driver. Used
    // to throttle the poll so we don't hammer the driver every frame.
    DeviceClock::time_point last_feedback_poll{};

    // Most recent vkAcquireNextImageKHR time on the app thread, recorded
    // under a small mutex. The monitor reads this to detect whether the
    // app is blocked on image acquire (CPU finished early, GPU was the
    // bottleneck) or whether the app started the next frame before the
    // previous GPU work ended (CPU-bound).
    mutable std::mutex acquire_mutex{};
    DeviceClock::time_point last_acquire_time_{};
    bool has_acquire_{false};

    std::condition_variable_any cv{};
    std::jthread monitor_worker{};

    void do_monitor(const std::stop_token stoken);
    void poll_feedback_locked();

  public:
    explicit SwapchainMonitor(const DeviceContext& device);
    SwapchainMonitor(const SwapchainMonitor&) = delete;
    SwapchainMonitor(SwapchainMonitor&&) = delete;
    SwapchainMonitor& operator=(const SwapchainMonitor&) = delete;
    SwapchainMonitor& operator=(SwapchainMonitor&&) = delete;
    ~SwapchainMonitor();

  public:
    void update_params(const bool was_low_latency_requested,
                       const std::chrono::microseconds delay);

    void notify_semaphore(const SemaphoreSignal& semaphore_signal);

    void attach_work(std::vector<std::unique_ptr<SubmissionSpan>> submissions);

    // Bind the swapchain to the pacer. For Tier 2 this queries the refresh
    // cycle and seeds the vblank anchor; for Tier 1 this is a no-op.
    void attach_swapchain(VkSwapchainKHR swapchain);

    // Set the present mode of the swapchain. Used by DisplayDeadlinePacer
    // to skip the vblank grid for MAILBOX/IMMEDIATE (no fixed vblank to
    // align to).
    void set_present_mode(VkPresentModeKHR mode);

    // Read the present mode. Exposed for diagnostic / future use.
    VkPresentModeKHR get_present_mode() const { return this->present_mode; }

    // Record a "ready to start" signal from vkAcquireNextImageKHR. The
    // monitor thread reads this to detect back-pressure (app waiting for
    // the swapchain image, meaning the GPU is the bottleneck).
    void record_acquire(const DeviceClock::time_point& time);

    // Read the most recent acquire time recorded by the app, or nullopt
    // if no acquire has happened yet. Safe to call concurrently with the
    // app thread.
    std::optional<DeviceClock::time_point> last_acquire() const;

  public:
    // Read-only views for the present-timing feedback sampler (Tier 2).
    // Returns false if the underlying pacer is not a DisplayDeadlinePacer.
    bool feed_present_feedback(std::uint64_t target_ns, std::uint64_t actual_ns);

    // Returns the most recent target present time chosen by the pacer, or
    // nullopt if no pacer or no frame paced yet. Safe to call concurrently
    // with the monitor thread.
    std::optional<DeviceClock::time_point> get_target_present() const;
};

} // namespace low_latency

#endif
