#ifndef STRATEGIES_LOW_LATENCY2_DEVICE_STRATEGY_HH_
#define STRATEGIES_LOW_LATENCY2_DEVICE_STRATEGY_HH_

#include "strategies/device_strategy.hh"
#include "swapchain_monitor.hh"

#include "device_clock.hh"

#include <array>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <unordered_map>
#include <vulkan/vulkan_core.h>

namespace low_latency {

class DeviceContext;

// Per-presentID marker timings recorded by SetLatencyMarkerNV. Indexed by
// VkLatencyMarkerNV (0..7) — each entry is the host clock time in
// microseconds when the app called the corresponding marker, or 0 if the
// app never set that marker for this presentID.
struct FrameMarkers final {
    std::uint64_t present_id{};
    std::array<std::uint64_t, 8> marker_us{};
};

class LowLatency2DeviceStrategy final : public DeviceStrategy {
  private:
    mutable std::shared_mutex mutex{};
    std::unordered_map<VkSwapchainKHR, SwapchainMonitor> swapchain_monitors{};

    // Per-presentID marker record ring. The app calls SetLatencyMarkerNV
    // 7 times per presentID (one per VkLatencyMarkerNV value), and the
    // record is built up incrementally. GetLatencyTimingsNV then reads
    // them back. The ring is bounded so a long-running session doesn't
    // grow unbounded.
    static constexpr std::size_t MARKER_RING_SIZE = 64;
    std::mutex marker_mutex_{};
    std::deque<FrameMarkers> marker_ring_{};

    // Per-stage EWMAs for diagnostic. Each entry is the smoothed
    // inter-marker interval (in microseconds). Diagnostic only — not used
    // to drive the pacer.
    static constexpr std::size_t STAGE_EWMA_COUNT = 7;
    std::array<double, STAGE_EWMA_COUNT> stage_ewma_us{};
    std::array<bool, STAGE_EWMA_COUNT> stage_ewma_seeded{};

    FrameMarkers& get_or_create_marker_record(std::uint64_t present_id);
    static constexpr double STAGE_EWMA_ALPHA = 0.20;

  public:
    explicit LowLatency2DeviceStrategy(DeviceContext& device);
    virtual ~LowLatency2DeviceStrategy();

  public:
    virtual void
    notify_create_swapchain(const VkSwapchainKHR& swapchain,
                            const VkSwapchainCreateInfoKHR& info) override;
    virtual void
    notify_destroy_swapchain(const VkSwapchainKHR& swapchain) override;

  public:
    void submit_swapchain_present_id(const VkSwapchainKHR& swapchain,
                                     const std::uint64_t& present_id);

    void notify_latency_sleep_mode(const VkSwapchainKHR& swapchain,
                                   const VkLatencySleepModeInfoNV* const info);

    void notify_latency_sleep_nv(const VkSwapchainKHR& swapchain,
                                 const VkLatencySleepInfoNV& info);

    void notify_acquire(const VkSwapchainKHR& swapchain,
                        const DeviceClock::time_point& time) override;

    // Record a latency marker for a presentID. Called from the
    // SetLatencyMarkerNV hook. Updates the per-stage EWMAs.
    void notify_set_latency_marker(std::uint64_t present_id,
                                   VkLatencyMarkerNV marker);

    // Fill in timings for the requested presentIDs. Called from
    // GetLatencyTimingsNV. Returns the number of records actually
    // populated (capped at the caller's buffer size).
    std::uint32_t copy_latency_timings(
        const VkLatencyTimingsFrameReportNV* in_reports,
        std::uint32_t count,
        VkLatencyTimingsFrameReportNV* out_reports);

    // Returns the most recent target present time chosen by the pacer for
    // this swapchain, or nullopt if the swapchain is unknown / not yet
    // paced. Caller must take this strategy's shared_mutex for reads.
    std::optional<DeviceClock::time_point>
    get_target_present(const VkSwapchainKHR& swapchain) const;
};

} // namespace low_latency

#endif
