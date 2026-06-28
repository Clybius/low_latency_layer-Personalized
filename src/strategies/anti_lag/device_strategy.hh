#ifndef STRATEGIES_ANTI_LAG_DEVICE_STRATEGY_HH_
#define STRATEGIES_ANTI_LAG_DEVICE_STRATEGY_HH_

#include "device_clock.hh"
#include "strategies/device_strategy.hh"

#include <vulkan/vulkan.h>

#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>

namespace low_latency {

class DeviceContext;
class AntiLagMonitor;

class AntiLagDeviceStrategy final : public DeviceStrategy {
  private:
    std::shared_mutex mutex{};
    // If this is nullopt don't track the submission.
    std::optional<std::uint64_t> frame_index{};
    bool is_enabled{};

    // Most recently created swapchain. Forwarded to the monitor so the
    // past-presentation-timing poll (driven from the monitor thread) can
    // query the driver for actual present times.
    VkSwapchainKHR current_swapchain_{VK_NULL_HANDLE};

    // Per-device async pacing monitor. Owns the pacer and a dedicated
    // thread that handles GPU completion wait, feedback polling, and
    // pacing sleep. AntiLagUpdateAMD queues work here and returns
    // immediately — the next INPUT call provides back-pressure.
    std::unique_ptr<AntiLagMonitor> monitor;

  public:
    explicit AntiLagDeviceStrategy(DeviceContext& device);
    virtual ~AntiLagDeviceStrategy();

  public:
    virtual void
    notify_create_swapchain(const VkSwapchainKHR& swapchain,
                            const VkSwapchainCreateInfoKHR& info) override;
    virtual void
    notify_destroy_swapchain(const VkSwapchainKHR& swapchain) override;

  public:
    void notify_update(const VkAntiLagDataAMD& data);

    void notify_acquire(const VkSwapchainKHR& swapchain,
                        const DeviceClock::time_point& time) override;

    bool should_track_submissions();
};

} // namespace low_latency

#endif
