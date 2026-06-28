#ifndef STRATEGIES_DEVICE_STRATEGY_HH_
#define STRATEGIES_DEVICE_STRATEGY_HH_

#include "device_clock.hh"

#include <vulkan/vulkan.h>
#include <vulkan/vulkan_core.h>

namespace low_latency {

class DeviceContext;

class DeviceStrategy {
  protected:
    DeviceContext& device;

  public:
    explicit DeviceStrategy(DeviceContext& device);
    virtual ~DeviceStrategy();

  public:
    virtual void
    notify_create_swapchain(const VkSwapchainKHR& swapchain,
                            const VkSwapchainCreateInfoKHR& info) = 0;
    virtual void notify_destroy_swapchain(const VkSwapchainKHR& swapchain) = 0;

    // Called when the app acquires the next image from a swapchain. Gives
    // the pacer a "ready to start" signal — if acquire_time > gpu_start
    // for the previous frame, the GPU had to wait for the swapchain image
    // (CPU-bound app); if gpu_start > acquire_time, the app was ready
    // before the GPU (GPU-bound). Strategies use this to detect
    // back-pressure and avoid over-sleeping on the release path.
    virtual void notify_acquire(const VkSwapchainKHR& /*swapchain*/,
                                const DeviceClock::time_point& /*time*/) {}
};

} // namespace low_latency

#endif