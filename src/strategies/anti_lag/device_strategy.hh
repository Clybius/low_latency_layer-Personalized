#ifndef STRATEGIES_ANTI_LAG_DEVICE_STRATEGY_HH_
#define STRATEGIES_ANTI_LAG_DEVICE_STRATEGY_HH_

#include "presentation_pacer.hh"
#include "strategies/device_strategy.hh"

#include "device_clock.hh"

#include <vulkan/vulkan.h>

#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>

namespace low_latency {

class DeviceContext;

class AntiLagDeviceStrategy final : public DeviceStrategy {
  private:
    std::shared_mutex mutex{};
    // If this is nullopt don't track the submission.
    std::optional<std::uint64_t> frame_index{};
    bool is_enabled{};

    // Most recently created swapchain. We bind it to the Tier 2 pacer so
    // the past-presentation-timing poll (driven from notify_update) can
    // query the driver for actual present times. Set in
    // notify_create_swapchain, cleared in notify_destroy_swapchain.
    VkSwapchainKHR current_swapchain_{VK_NULL_HANDLE};

    // Last time we polled past presentation timings. Throttled to ~8 ms
    // to avoid hammering the driver every frame.
    DeviceClock::time_point last_feedback_poll_{};

    std::unique_ptr<PresentationPacer> pacer;
    DeviceClock::time_point release_prev{};

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
