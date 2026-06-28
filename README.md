# low_latency_layer-Personalized (Clyb's Fork)

> Fork of [Korthos-Software/low_latency_layer](https://github.com/Korthos-Software/low_latency_layer) — a hardware-agnostic Vulkan implicit layer implementing `VK_NV_low_latency2` and `VK_AMD_anti_lag` device extensions.

> [!IMPORTANT]
> **As of v0.2.0, this layer is opt-in.** Set `LOW_LATENCY_LAYER=1` in your environment to enable it globally. Alternatively, this environment variable may be added to per-game Steam launch options to enable it on a case-by-case basis. Set `DISABLE_LOW_LATENCY_LAYER=1` to forcefully disable the layer (takes precedence).
> **AI Sloppa Notice.** This fork was partially re-written using various OSS models.

A C++23 implicit Vulkan layer that reduces click-to-photon latency by implementing both AMD and NVIDIA's latency reduction technologies in a hardware-agnostic manner.

By providing hardware-agnostic implementations of the `VK_NV_low_latency2` and `VK_AMD_anti_lag` device extensions, this layer brings Reflex and Anti-Lag capabilities to AMD and Intel GPUs. When paired with [dxvk-nvapi](https://github.com/jp7677/dxvk-nvapi/) to forward the relevant calls, it bypasses the need for official driver-level support.

The layer also eliminates a hardware support disparity as considerably more applications support NVIDIA's Reflex than AMD's Anti-Lag.

## Fork Changes (vs. Korthos-Software upstream)

This fork evolves the original layer with substantial architectural improvements:

### Pacing System

The layer now features a two-tier presentation pacing architecture:

- **Tier 1 — PredictiveQueuePacer** (always available): Just-in-time release using GPU-timestamp-driven closed-loop EWMA control. Predicts render duration and CPU-to-GPU dispatch lag, releasing the next frame so GPU work starts when the previous frame's GPU work ends — minimizing render queue depth. Adaptive alpha (hitch/noisy/nominal/stable) with hitch rejection prevents transient spikes from destabilizing the estimate.

- **Tier 2 — DisplayDeadlinePacer** (when `VK_EXT_present_timing` or `VK_GOOGLE_display_timing` is available): Vblank-anchored PI loop that wraps the Tier 1 pacer. Aligns release to the display's refresh boundary, with an optional PI feedback loop driven by `vkGetPastPresentationTiming*` to correct for sustained release-to-actual-present error. VRR-capable (falls back to cadence pacing when `refreshInterval==0`). Supports MAILBOX/IMMEDIATE present modes by skipping the vblank grid.

Both backends use a hybrid sleep strategy: coarse `sleep_for` with a calibrated spin tail sized to the `DeviceClock` error bound, ensuring precise wake-up timing.

### FPS Limiter

`LOW_LATENCY_LAYER_FPS_LIMIT` imposes a layer-wide frame-rate cap enforced at the release point (start of next frame) for low latency — never at present time. The layer can only lower FPS below the app's own cap; it will never raise FPS above it.

### DeviceClock

A C++ `Clock` concept implementation calibrated via `vkGetCalibratedTimestampsKHR`. Maintains a periodic calibration thread (every second) that synchronizes GPU device timestamps with `CLOCK_MONOTONIC`. Exposes `error_bound_duration()` so the hybrid sleeper adapts its spin budget to the actual calibration noise floor of each device.

### Present Timing Probe

Queries `VK_EXT_present_timing` or `VK_GOOGLE_display_timing` for:
- Refresh cycle duration (powers Tier 2 vblank grid math)
- Past presentation timing feedback (`targetTime` vs `actualPresentTime`), polled at ~8ms intervals from the monitor thread, feeding the Tier 2 PI loop.

### Latency Marker Tracking (`SetLatencyMarkerNV` / `GetLatencyTimingsNV`)

Full implementation of NVIDIA's marker ring buffer with diagnostic per-stage EWMAs (7 pipeline stages). Markers are recorded per-presentID in a bounded ring buffer (64 entries) and read back by `GetLatencyTimingsNV`. GPU render timestamps are populated from the SubmissionSpan timestamps we already collect.

### SwapchainMonitor

Per-swapchain dedicated monitor thread that hosts the pacer, manages pending timeline semaphore signals, and polls past-presentation feedback. Semaphores are never dropped un-signalled — the monitor guarantees forward progress even during teardown when `QueueNotifyOutOfBandNV` is used.

### Forwarding Unhandled Extensions

When the layer is loaded but the application doesn't request the advertised extension, Vulkan calls (e.g., `AntiLagUpdateAMD`, `LatencySleepNV`, `SetLatencyMarkerNV`) are forwarded to the underlying driver rather than intercepted. This allows the layer to coexist with applications that use these extensions natively.

### QueueSubmit2 / GPDP2 / GPDF2 Dispatch

`QueueSubmit2` and `QueueSubmit2KHR` are dispatched to the variant the caller invoked, and `GetPhysicalDeviceFeatures2`/`GetPhysicalDeviceProperties2` are dispatched through the correct KHR vs. core path. This fixes hard crashes with applications that call the KHR variants directly (observed in Proton/DXVK environments).

### NVIDIA GPU Spoofing

`LOW_LATENCY_LAYER_SPOOF_NVIDIA=1` reports the device as an NVIDIA GeForce RTX 5090 (vendor ID `0x10DE`, device ID `0x2B85`) regardless of actual hardware. Not recommended as a first resort — prefer `DXVK_CONFIG="dxgi.hideAmdGpu = True"`, as this option is known to break Proton's FSR4 upgrade path.

### Code Modernization

- Vulkan Utility Libraries integration (`vku::InitDispatchTable`, `vku::FindStructInPNextChain`, `vku::AddToPnext`, `vku::safe_VkDeviceCreateInfo`)
- All constructors marked `explicit`, operators return references, functions are `noexcept`
- Thread safety: shared/unique mutexes, atomic relaxed loads, scoped locks throughout
- Timestamp injection now uses `ResetQueryPoolEXT` + `CmdWriteTimestamp2KHR` (sync2 path) instead of the legacy `vkCmdResetQueryPool` + `vkCmdWriteTimestamp`
- `SubmissionSpan` aggregates per-queue timestamp handles into head/tail pairs for reduced memory and faster await

# Dependencies

- [CMake](https://cmake.org): A cross-platform, open-source build system generator.
- [Vulkan Headers](https://github.com/KhronosGroup/Vulkan-Headers): Vulkan header files and API registry.
- [Vulkan Utility Libraries](https://github.com/KhronosGroup/Vulkan-Utility-Libraries): Library to share code across various Vulkan repositories.

# Building from Source and Installation

Clone this repo.

```
    $ git clone https://github.com/Clybius/low_latency_layer-Personalized.git
    $ cd low_latency_layer-Personalized
```

Create an out-of-tree build directory and install.

> [!WARNING]
> Install your distro's `vulkan-headers`, `vulkan-utility-libraries`, and `cmake` packages before proceeding. Missing dependencies are the most common cause of build errors.

```
    $ cmake -B build ./
    $ cmake --build build
    $ sudo cmake --install build
```

# Usage and Configuration

By default, the layer exposes the `VK_AMD_anti_lag` device extension. Provided the layer was enabled with `LOW_LATENCY_LAYER=1`, Linux native applications like *Counter-Strike 2* will work out-of-the-box, allowing you to toggle AMD's Anti-Lag in its menus. You can further customize the layer's behavior using the environment variables listed below.

| Variable | Description |
| :--- | :--- |
| `LOW_LATENCY_LAYER` | Set to `1` to enable the layer. Required for any functionality. |
| `DISABLE_LOW_LATENCY_LAYER` | Set to `1` to force-disable the layer. Takes precedence over `LOW_LATENCY_LAYER`. |
| `LOW_LATENCY_LAYER_REFLEX` | Set to `1` to expose `VK_NV_low_latency2` instead of `VK_AMD_anti_lag`. Provides Reflex support instead of Anti-Lag 2 (default). |
| `LOW_LATENCY_LAYER_FPS_LIMIT` | Set to a positive number (e.g. `120`) to impose a layer-wide frame-rate cap. The cap is enforced at the release point (start of the next frame) for low latency, not at present time. The layer can only lower FPS below the app's own cap — it will never raise FPS above it. `0` or unset disables the layer cap. |
| `LOW_LATENCY_LAYER_SPOOF_NVIDIA` | Set to `1` to report the device as an NVIDIA GeForce RTX 5090 to the application, regardless of actual hardware. Not recommended as a first resort — prefer `DXVK_CONFIG="dxgi.hideAmdGpu = True"`, as this option is known to break Proton's FSR4 upgrade path. |

When providing Reflex support for Proton-based applications, try `LOW_LATENCY_LAYER_REFLEX=1` on its own first. If the Reflex option does not appear in-game, add `DXVK_CONFIG="dxgi.hideAmdGpu = True"`. If this does not expose Reflex you can try `PROTON_FORCE_NVAPI=1` and/or `LOW_LATENCY_LAYER_SPOOF_NVIDIA=1` - however both are known to break Proton's FSR4 upgrade path (`PROTON_FSR4_UPGRADE` / `PROTON_FSR4_RDNA3_UPGRADE`).

**Steam launch options example:**
```
LOW_LATENCY_LAYER=1 LOW_LATENCY_LAYER_REFLEX=1 DXVK_CONFIG="dxgi.hideAmdGpu = True" %command%
```

The 'Boost' mode of Reflex is supported but is functionally identical to 'On' — the layer treats both modes identically.

## How It Works

The layer intercepts Vulkan calls at three levels:

1. **Extension advertisement** — `EnumerateDeviceExtensionProperties` injects either `VK_AMD_anti_lag` or `VK_NV_low_latency2` into the device extension list, making the application believe the GPU natively supports the extension.

2. **Timestamp injection** — `QueueSubmit`/`QueueSubmit2`/`QueueSubmit2KHR` are intercepted and GPU timestamp queries (TOP_OF_PIPE + BOTTOM_OF_PIPE) are injected as command buffers bracketing the application's work. These timestamps flow through a pool of pre-allocated query chunks, enabling per-submission GPU timing without additional driver overhead.

3. **Pacing** — The pacer (Tier 1 or Tier 2) awaits GPU work completion via `SubmissionSpan::await_completed()`, computes a latency-optimal release point (using EWMA predictions of render duration and CPU dispatch lag), and sleeps the release thread until that point. In Reflex mode, timeline semaphores are signalled at the chosen release time; in Anti-Lag mode, `AntiLagUpdateAMD` blocks until the release target.

### Architecture

```
App calls QueueSubmit/QueueSubmit2
       │
       ▼
 Layer injects timestamp CBs (start/end)
       │
       ▼
 QueueStrategy::notify_submit() records SubmissionSpan per presentID
       │
       ▼
 App calls QueuePresentKHR
       │
       ▼
 QueueStrategy::notify_present() pairs presentID → swapchain
       │
       ▼
 DeviceStrategy collects all queue SubmissionSpans for the frame
       │
       ▼
 SubmissionSpan::await_completed() blocks until GPU work finishes
       │
       ▼
 PresentationPacer::pace() computes release point
   ├─ Tier 1: PredictiveQueuePacer (EWMA-driven JIT)
   └─ Tier 2: DisplayDeadlinePacer (vblank-anchored PI loop)
       │
       ▼
 hybrid_sleep_until(release, spin_budget)
       │
       ▼
 Semaphore signalled (Reflex) / AntiLagUpdateAMD returns (Anti-Lag)
```

### Tiered Present-Timing Backend

The pacer automatically selects the best available backend:

| Tier | Extension Required | Algorithm | Sleep Strategy |
|---|---|---|---|
| 1 | None (always available) | EWMA predictive JIT release | Hybrid sleep w/ adaptive spin budget |
| 2 | `VK_EXT_present_timing` or `VK_GOOGLE_display_timing` | Vblank-anchored PI loop | Same as Tier 1 + PI correction |

Tier 2 activates when the driver supports either present-timing extension. The refresh cycle is queried once at swapchain creation. Past-presentation feedback is polled at ~8ms intervals from the monitor thread (not the app thread) to avoid adding latency to the app's present call.

# License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.
