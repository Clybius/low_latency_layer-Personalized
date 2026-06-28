#ifndef FPS_LIMITER_HH_
#define FPS_LIMITER_HH_

#include "device_clock.hh"

#include <chrono>

namespace low_latency {

// Compute the effective min_delay for the PresentationPacer: the max of the
// app-supplied delay and the layer-imposed FPS cap interval.
//
// fps_limit <= 0 disables the layer cap (returns app_delay unchanged).
// Otherwise cap_interval = 1e9 / fps_limit nanoseconds, and we return
// max(app_delay, cap_interval). This means the layer can only LOWER the frame
// rate below the app's own cap, never raise it.
//
// The pacer applies this as a release-side floor
// (target_release = max(target_release, release_prev + min_delay)), which
// favors low latency: it delays the START of the next frame (keeping input
// fresh) rather than the PRESENT of a finished frame (which would add queue
// latency).
DeviceClock::duration effective_min_delay(std::chrono::nanoseconds app_delay,
                                          double fps_limit);

} // namespace low_latency

#endif
