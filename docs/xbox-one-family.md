# Xbox One / One S / One X support scaffolding

Status: **Offline-tested with 73 profile and memory checks. Hardware validation is pending for Xbox One, One S, and One X.**

Tiramisu now has detection, host-profile defaults, and a 32 MB ESRAM budget ledger for the three last-generation Xbox consoles. The Series S remains the only console with on-device proof. Treat the One-family entries below as engineering targets, not measured support.

## 1. Hardware Envelopes & Architectural Matrix

| Console | Family ID | Codename | CPU | GPU | RAM & Bandwidth | Dev Mode Budget | ESRAM status |
|---|---|---|---|---|---|---|---|
| Xbox One (2013) | `one` | Durango | 8-core AMD Jaguar @ 1.75 GHz (AVX2=0) | GCN 1.1, ~1.31 TFLOPS (12 CUs) | 8 GB DDR3, ~68 GB/s | target 5120 MB | 32 MB budget ledger (driver owns residency) |
| Xbox One S (2016) | `one_s` | Edmonton | 8-core AMD Jaguar @ 1.75 GHz (AVX2=0) | GCN 1.1, ~1.40 TFLOPS (+7.1% HDR) | 8 GB DDR3, ~68 GB/s | target 5120 MB | 32 MB budget ledger (driver owns residency) |
| Xbox One X (2017) | `one_x` | Scorpio | 8-core AMD Jaguar Evolved @ 2.3 GHz (AVX2=0) | GCN 4.0 (Polaris), ~6.0 TFLOPS (40 CUs) | 12 GB GDDR5, 326 GB/s unified | target 5120 MB | None (Unified GDDR5) |
| Xbox Series S | `series_s` | Lockhart | 8-core AMD Zen 2 @ 3.6 GHz (AVX2=1) | RDNA 2, ~4.0 TFLOPS (20 CUs) | 10 GB GDDR6, 224 GB/s | target 5120 MB | None (Unified GDDR6) |
| Xbox Series X | `series_x` | Anaconda | 8-core AMD Zen 2 @ 3.8 GHz (AVX2=1) | RDNA 2, ~12.15 TFLOPS (52 CUs) | 16 GB GDDR6, 560 GB/s | target 5120 MB | None (Unified GDDR6) |

### Key Architectural Insights:
1. **Instruction Set & Execution Parity:** All 5 consoles execute x86-64 native instructions directly without JIT or CPU emulation. Jaguar lacks AVX2 (`CPUID(7,0): EBX bit 5 == 0`), which enables instant hardware-level detection in native C++.
2. **Memory bandwidth & ESRAM accounting:** Durango and Edmonton pair a 68 GB/s DDR3 bus with 32 MB of on-die ESRAM. Tiramisu tracks render-target and depth-stencil candidates against a 32 MB ledger, but UWP D3D11 does not expose explicit ESRAM allocation; the driver decides actual residency. Scorpio has no ESRAM ledger because it uses unified GDDR5.
3. **Dev Mode memory budget:** the host targets 5120 MB when the title is treated as a Game (`expandedResources` capability). The actual concession must be measured per console and OS build; it is not assumed here.

## 2. Implemented Features & Subsystems

### 2.1 Hardware Auto-Detection Engine (`src/host/console_detect.h`, `.cpp`)
- Multi-tier detection: CPUID fingerprinting (AMD Family 16h Jaguar vs 17h/19h Zen 2, AVX2 support bit), physical RAM thresholds (8 GB vs 12 GB vs 10 GB vs 16 GB), WinRT `EasClientDeviceInformation` model/SKU matching, and Device Portal tweak configuration.

### 2.2 32 MB ESRAM Budget Ledger (`src/trt/bridge_esram.h`, `.cpp`)
- Tracks candidate render targets (e.g. 1080p RGBA8, 8.1 MB) and depth buffers (D24S8, 8.1 MB) against a 32 MB budget and reports over-capacity spillover. This is **accounting only**: the driver owns actual ESRAM residency, and read-only textures/audio are not moved by this ledger.

### 2.3 Startup Crash Guard & Early Logging (`src/host/main.cpp`)
- Top-level Vectored Exception Handler installed at process entry (`AddVectoredExceptionHandler(1, startup_veh_filter)`) writing crash telemetry to `LocalState\tiramisu_startup_crash.log`.
- Pre-COM/DirectX disk logging in `wWinMain` via `longan::log_open(package_install_path())`.
- Direct3D 11 feature level negotiation with fallback from `11_1` through `11_0`, `10_1`, and `10_0` (`src/host/swapchain.cpp`).
- Native GameInput absence handling with automatic fallback to XInput + CoreWindow pointer on older Windows 10 Xbox OS builds.

At Play, the guest memory governor caps the configured budget at the available app memory. It uses the Dev Mode process limit and subtracts the current host working set. If the OS query is unavailable, Play uses the configured profile budget.

### 2.4 Tooling & Offline Gates
- `tools/console_type.py`: Alias resolution, hardware profiling, and tweak generation for `one` (Durango), `one_s` (Edmonton), `one_x` (Scorpio), `series_s`, and `series_x`.
- `tests/py_tools/test_console_type.py`: Offline unit coverage for the added generations and alias tables.

## 3. D3D11 feature-level clamp (2026-11-25)

`swapchain_create()` no longer hard-codes a request list. `console_feature_levels(gen)` in
`console_detect.cpp` returns the D3D11 levels for the detected family and the swapchain
requests exactly those:

| Family | D3D11 levels requested | Why |
|---|---|---|
| `one` (Durango) | 11_0, 10_1, 10_0 | GCN 1.1: no 11_1 tiled-resource caps |
| `one_s` (Edmonton) | 11_0, 10_1, 10_0 | Same GCN 1.1 part, higher clock (data bug fixed 2026-11-25 — the table claimed 11_1) |
| `one_x` (Scorpio) | 11_1, 11_0, 10_1, 10_0 | GCN 4.0 |
| `series_s` / `series_x` | 11_1, 11_0, 10_1, 10_0 | Table reports 12_0/12_1 for the D3D12 path; those are not D3D11 levels, so they clamp |

The choice is logged at device creation:
`[swapchain] console=Xbox One S (one_s) last_gen=1 avx2=0 esram=32MB budget=5120MB d3d11_levels=3 top=0xb000`.

## 4. Not verified

No One, One S, or One X hardware has been used. Still unmeasured: activation, input fallback, D3D11 feature levels, the actual Dev Mode memory concession, ESRAM ledger behavior under a real frame, and crash-guard behavior on older OS builds.
