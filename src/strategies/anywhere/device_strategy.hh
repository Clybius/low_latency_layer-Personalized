#ifndef STRATEGIES_ANYWHERE_DEVICE_STRATEGY_HH_
#define STRATEGIES_ANYWHERE_DEVICE_STRATEGY_HH_

#include "device_clock.hh"
#include "strategies/device_strategy.hh"

#include <vulkan/vulkan.h>

#include <memory>
#include <shared_mutex>

namespace low_latency {

class DeviceContext;
class AnywhereMonitor;

// Device strategy for ANYWHERE mode. Collects SubmissionSpans from all
// queues at present time and hands them to the AnywhereMonitor for async
// pacing. Unlike AntiLagDeviceStrategy, there is no app-side enable/disable
// toggle — tracking is always active.
class AnywhereDeviceStrategy final : public DeviceStrategy {
  private:
    std::shared_mutex mutex{};

    VkSwapchainKHR current_swapchain_{VK_NULL_HANDLE};
    VkPresentModeKHR present_mode_{VK_PRESENT_MODE_FIFO_KHR};

    std::unique_ptr<AnywhereMonitor> monitor;

  public:
    explicit AnywhereDeviceStrategy(DeviceContext& device);
    virtual ~AnywhereDeviceStrategy();

  public:
    virtual void
    notify_create_swapchain(const VkSwapchainKHR& swapchain,
                            const VkSwapchainCreateInfoKHR& info) override;
    virtual void
    notify_destroy_swapchain(const VkSwapchainKHR& swapchain) override;

    void notify_acquire(const VkSwapchainKHR& swapchain,
                        const DeviceClock::time_point& time) override;

    // Called from QueuePresentKHR (after forwarding to the driver).
    // Collects SubmissionSpans from all queues and enqueues them to the
    // monitor. Blocks if the monitor is still processing the previous
    // frame (back-pressure).
    void notify_present(const VkPresentInfoKHR& present);
};

} // namespace low_latency

#endif
