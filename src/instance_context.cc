#include "instance_context.hh"

#include "layer_context.hh"

#include <utility>

namespace low_latency {

InstanceContext::InstanceContext(const LayerContext& parent_context,
                                 const VkInstance& instance,
                                 const VkInstanceCreateInfo& create_info,
                                 VkuInstanceDispatchTable&& vtable)
    : layer(parent_context), instance(instance), vtable(std::move(vtable)) {
    // The decoupled-simulation heuristic is removed; pacing is now
    // app-agnostic via PresentationPacer (GPU-timestamp-driven).
    (void)create_info;
}

InstanceContext::~InstanceContext() {}

} // namespace low_latency
