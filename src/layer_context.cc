#include "layer_context.hh"

#include <cstdlib> // for env var
#include <string>
#include <string_view>

namespace low_latency {

static bool parse_bool_env(const auto& name) {
    const auto env = std::getenv(name);
    return env && std::string_view{env} == "1";
}

static double parse_double_env(const auto& name) {
    const auto env = std::getenv(name);
    if (!env) {
        return 0.0;
    }
    try {
        return std::stod(env);
    } catch (...) {
        return 0.0;
    }
}

LayerContext::LayerContext()
    : should_expose_reflex(parse_bool_env(REFLEX_ENV)),
      should_spoof_nvidia(parse_bool_env(SPOOF_NVIDIA_ENV)),
      fps_limit(parse_double_env(FPS_LIMIT_ENV)) {}

LayerContext::~LayerContext() {}

} // namespace low_latency
