# Game Profiles — Per-Game Settings Overrides

Tiramisu supports per-game profile overrides. When a game is launched, the host
looks for `<game_dir>/tiramisu.profile.ini` and applies any keys it contains
on top of the global settings. Missing keys fall back to the global settings.

## Profile file format

```ini
# tiramisu.profile.ini — place next to the game's .exe
# All keys are optional; only listed keys override global settings.

# ---- input ----
mouse_speed=75
wheel_speed=40
input_deadzone=10
pad_vibration=1

# ---- graphics ----
resolution_scale=100        # 50..200
frame_rate_cap=60           # 0=unlimited, 30, 60, 120
vsync=1
texture_quality=2           # DOCUMENTED BUT SETTINGS-ONLY — no bridge reads it yet;
                            # write it and nothing changes (tools/profile-audit.py)

# ---- OS personality (some engines gate on this) ----
os_major=10
os_minor=0
os_build=19045
win11_personality=0

# ---- compatibility ----
write_watch=1
memory_budget_mb=4096
audio_buffer_ms=20
shader_cache=1

# ---- launch ----
args=-windowed -novid
appid=0                    # Steam app ID (0 = none)
```

## Input

Choose the input preference once in Tiramisu Settings. Auto passes connected
devices through. Controller makes a keyboard-backed gamepad available when
no physical controller is connected. Existing explicit input overrides from
the library or dashboard still take precedence over the global preference.

## How it works

1. Host loads global settings from `LocalState/settings.ini`
2. On game launch, host checks `<game_dir>/tiramisu.profile.ini`
3. If found, applies overrides on top of global settings (only listed keys)
4. Guest bridges read the effective settings via `trt_settings_*` getters
5. Profile is re-read on every launch (no caching across sessions)

## Known game profiles

### Ultra Pool
```ini
frame_rate_cap=60
resolution_scale=100
texture_quality=2
```

### RACCOIN
```ini
frame_rate_cap=60
memory_budget_mb=4096
os_build=19045
```

## Contributing profiles
If you find settings that make a game boot or run better, submit a PR with
the profile file. Include:
- Game name and engine
- The profile contents
- What changed (boot depth, stability, performance)
- Hardware tested on (console or dev host)