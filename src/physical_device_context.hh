#ifndef PHYSICAL_DEVICE_CONTEXT_HH_
#define PHYSICAL_DEVICE_CONTEXT_HH_

#include "instance_context.hh"

#include <vulkan/vulkan.hpp>
#include <vulkan/vulkan_core.h>

#include "context.hh"
#include "device_clock.hh"

namespace low_latency {

// Which optional present-timing extension (if any) the driver supports.
// Used to decide between the Tier-1 predictive pacer and the Tier-2
// display-deadline pacer. Never required — if neither is present, Tier 1 is
// used unconditionally.
enum class PresentTimingMode : std::uint8_t {
    none = 0,
    ext_present_timing,
    google_display_timing,
};

class PhysicalDeviceContext final : public Context {
  public:
    // The extensions we need for our layer to function.
    // If the PD doesn't support this then the layer shouldn't set the anti lag
    // flag in VkGetPhysicalDevices2 (check this->supports_required_extensions).
    static constexpr auto required_extensions = {
        VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME,
        VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME,
        VK_EXT_HOST_QUERY_RESET_EXTENSION_NAME,
        VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME};

  public:
    InstanceContext& instance;
    const VkPhysicalDevice physical_device{};
    const bool supports_required_extensions{};

    std::unique_ptr<VkPhysicalDeviceProperties> properties{};
    std::unique_ptr<std::vector<VkQueueFamilyProperties>> queue_properties{};

    // Optional present-timing support (never required).
    PresentTimingMode present_timing_mode{PresentTimingMode::none};
    // Refresh cycle in DeviceClock::duration; 0 = unknown. Filled lazily
    // when Tier 2 backend asks for it.
    DeviceClock::duration refresh_interval{};
    bool is_vrr{false};

  public:
    explicit PhysicalDeviceContext(InstanceContext& instance_context,
                                   const VkPhysicalDevice& physical_device);
    virtual ~PhysicalDeviceContext();
};

} // namespace low_latency

#endif