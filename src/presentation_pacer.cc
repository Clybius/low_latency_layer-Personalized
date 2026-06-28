#include "presentation_pacer.hh"

#include "device_context.hh"
#include "display_deadline_pacer.hh"
#include "predictive_queue_pacer.hh"

namespace low_latency {

std::unique_ptr<PresentationPacer>
make_presentation_pacer(const DeviceContext& device) {
    if (device.physical_device.present_timing_mode != PresentTimingMode::none) {
        return std::make_unique<DisplayDeadlinePacer>(device);
    }
    return std::make_unique<PredictiveQueuePacer>(device);
}

} // namespace low_latency
