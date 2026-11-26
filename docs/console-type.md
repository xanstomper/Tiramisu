# Console-type autodetect (Series X/S, Xbox One, Xbox One S, Xbox One X)

`tools/console_type.py` asks Device Portal (`/api/os/info`) what console it is
on and emits a small “tiramisu tweak” the user can apply before a session.
In addition, Tiramisu features native on-device hardware autodetect (`src/host/console_detect.h` / `.cpp`)
via CPUID (AMD Jaguar Family 16h vs Zen 2 Family 17h/19h, AVX2 flag), physical RAM sizing,
and Device Portal configuration.

```sh
tools/console_type.py detect --console <ip> [--user U --password P]
tools/console_type.py detect --console <ip> --apply   # also writes LocalState/console_tweak_<family>.json
```

## Detection

Primary key: `Platform.Model` from `/api/os/info`.
Fallbacks: `ConsoleType`, `ProductId`, `ConsoleModel`, `OSModel`, `MachineName`, WinRT `EasClientDeviceInformation`, CPUID.
Aliases live in `tools/console_type.py::MODEL_ALIASES`:

| Console | Family | Codename | Memory Budget | ESRAM ledger |
|---|---|---|---|---|
| Xbox Series X | `series_x` | Anaconda | target 5120 MB | None (Unified GDDR6) |
| Xbox Series S | `series_s` | Lockhart | target 5120 MB | None (Unified GDDR6) |
| Xbox One X | `one_x` | Scorpio | target 5120 MB | None (Unified 326 GB/s GDDR5) |
| Xbox One S | `one_s` | Edmonton | target 5120 MB | 32 MB budget accounting only |
| Xbox One (Original) | `one` | Durango | target 5120 MB | 32 MB budget accounting only |

Unknown models are reported honestly (`family: null`), not guessed.

## Tweaks (Host Settings Defaults)

```json
{
  "series_x": { "memory_budget_mb": 5120, "os_build": 22621, "frame_rate_cap": 60, "resolution_scale": 100 },
  "series_s": { "memory_budget_mb": 5120, "os_build": 22621, "frame_rate_cap": 60, "resolution_scale": 100 },
  "one_x":    { "memory_budget_mb": 5120, "os_build": 22621, "frame_rate_cap": 60, "resolution_scale": 100, "texture_quality": 2 },
  "one_s":    { "memory_budget_mb": 5120, "os_build": 19045, "frame_rate_cap": 60, "resolution_scale": 85,  "texture_quality": 1 },
  "one":      { "memory_budget_mb": 5120, "os_build": 19045, "frame_rate_cap": 30, "resolution_scale": 80,  "texture_quality": 1 }
}
```

### Architectural Distinctions:
- **Xbox One (Durango, 2013):** 8-core AMD Jaguar @ 1.75 GHz (AVX2=0), GCN 1.1 1.31 TF, 8 GB DDR3 + 32 MB ESRAM. Profile defaults: 80% render scale, 30 FPS cap, 5120 MB target budget, D3D11 primary rendering.
- **Xbox One S (Edmonton, 2016):** 8-core AMD Jaguar @ 1.75 GHz (AVX2=0), GCN 1.1 1.40 TF (+7.1% GPU clock for HDR), 8 GB DDR3 + 32 MB ESRAM. Profile defaults: 85% render scale, 60 FPS cap, 5120 MB target budget. The ledger only accounts ESRAM candidates; it does not control residency.
- **Xbox One X (Scorpio, 2017):** 8-core AMD Jaguar Evolved @ 2.3 GHz (AVX2=0), GCN 4.0 (Polaris 40 CUs) 6.0 TF, 12 GB GDDR5 (326 GB/s unified bandwidth). Profile defaults: 100% scale, 60 FPS cap, 5120 MB target budget, high texture quality, experimental D3D12 enabled.

## Status

- Detection/apply engine: offline-tested (`tests/py_tools/test_console_type.py`); not verified on One-family hardware.
- Native runtime governor: `trt_mem_budget_set_mb` and the `trt_esram_*` budget ledger are integrated into `src/trt/bridge_membudget.cpp` and `src/trt/bridge_esram.cpp`. ESRAM is accounting only; the driver owns residency.
- Applied automatically on host launch via `src/host/console_detect.cpp` and `src/host/main.cpp`. The 5120 MB budget and profile defaults remain unmeasured targets on One/One S/One X.
