# Celeste Gameplay Verification Plan — Tiramisu / Xbox Series S

Status: PREP — written 2026-11-20 ~18:35 EST. No console touch. For the test agent to run when Celeste boots to the title screen (the hang is fixed; "invoking managed Main" no longer freezes).

Ben's bar: boot + menu + playable. This plan defines each step, the exact inputs to send, what to expect, and what counts as a pass.

## 0. Prerequisites (before starting)

- Celeste booted to the title screen (the hang is fixed; the title screen is visible on a TV screenshot, or the log shows FNA "GAME DISPLAYED").
- The app server is up on :9127 (GET /api/status returns running_game=celeste).
- The pair code: read the 8-digit code from the TV Setup screen control hint ("/controls \<code\>") via a portal screenshot BEFORE launching Celeste (the code is generated at app start and persists until restart, so read it while the Setup screen is up).

## 1. Input reference — how the test agent drives the game

### 1a. Web control API (the remote control path)

- Base: http://10.0.0.23:9127 (the app server; same host/port as the other /api endpoints).
- Pair: POST /api/controls/pair, body = the 8-digit code (raw text, no JSON). Response body (200) = a 32-hex-char token. 5 bad codes → 1-min lockout (429).
- Input: POST /api/controls/input, header `Authorization: <token>` (raw token, NO "Bearer " prefix), body = event lines (plain text, max 2048 bytes, max 128 events/request, max 512 queued).
- Leave: POST /api/controls/leave with the auth header (releases the session, resets all inputs).
- Event line format: `<kind> <first> <second>` (space-separated, one per line).
  - `k <vk> <down>` — keyboard: vk = Windows virtual-key code (8-254), down = 1 (press) / 0 (release).
  - `m <dx> <dy>` — relative mouse move (pixels, -2048..2048).
  - `a <x> <y>` — absolute mouse position (0-1000 normalized screen coords).
  - `b <btn> <down>` — mouse button: btn 0-4 (0=left), down 1/0.
  - `w <delta> <ignored>` — wheel: delta must be 120 or -120.
  - `t <char> <ignored>` — text input: char = Unicode code point (32-65535).
  - `r 0 0` — reset: releases all keys/buttons.
- **2-second idle reset (CRITICAL):** if no input POST arrives for >2s, the host injects a reset on the next drain — all held keys/buttons are released. Sustained holds (e.g., holding Right to run, holding C to climb) REQUIRE a re-post at least every ~2s. The input helper must re-send the held state (or a no-op event) on a <2s cadence. A key-down followed by 2s of silence = the key is released.
- **Host-intercepted keys (never reach the guest):** F8 (0x77) always (toggles cursor mode); numpad keys (0x60-0x69, 0x6B, 0x6D, 0x6E) always (host mouse). Arrows (0x25-0x28), Enter (0x0D), Space (0x20), RCtrl (0xA3), Apps (0x5D), PgUp/PgDn (0x21/0x22) are intercepted ONLY if F8 cursor mode is on — it defaults OFF; NEVER press F8 via web controls.
- **No gamepad over the web API.** The web control API has no gamepad event kind. The host polls real Xbox controllers via XInputGetState (4 slots) → trt_input_pad_state_n → FNA GetPadState. Gamepad verification requires a physical controller (Ben). Keyboard is the test agent's path.

### 1b. Celeste's controls (defaults)

Keyboard (the test agent's path):

| Action | Key | VK (dec) | VK (hex) |
| Move left | Left arrow | 37 | 0x25 |
| Move right | Right arrow | 39 | 0x27 |
| Move up / menu up | Up arrow | 38 | 0x26 |
| Move down / menu down | Down arrow | 40 | 0x28 |
| Jump | Z | 90 | 0x5A |
| Dash | X | 88 | 0x58 |
| Climb / grab (hold) | C | 67 | 0x43 |
| Confirm | Enter | 13 | 0x0D |
| Confirm (alt) | Z | 90 | 0x5A |
| Back / pause | Esc | 27 | 0x1B |
| Back (alt) | X | 88 | 0x58 |

Gamepad (Xbox — for Ben's hands-on verification; the test agent cannot send these):

| Action | Button |
| Move | Left stick / D-pad |
| Jump | A |
| Dash | X |
| Climb / grab (hold) | Right Trigger (RT) |
| Confirm | A |
| Back | B |
| Pause | Start (Menu) |

### 1c. How input reaches Celeste (the chain)

- Web 'k' events → trt_input_key_event2 → WM_KEYDOWN/WM_KEYUP messages → FNA NativePollEvent (the managed game polls the message queue) → Celeste's keyboard state.
- Web 'm'/'a'/'b' events → trt_mouse_* state → FNA NativeGetMouseState / NativeGetMouseDelta.
- Real controller → host XInputGetState poll → trt_input_pad_state_n → FNA NativeGetPadState (thumbs, buttons, triggers) + vibration back via XInputSetState.
- Mouse is not used for Celeste gameplay (keyboard/gamepad only); the mouse matters only for the launcher UI (not needed — launch via /api/launch).

## 2. Step-by-step verification

### Step 0 — Pair + launch (prereq)

1. Screenshot the Setup screen (portal /ext/screenshot); read the 8-digit pair code from the control hint ("/controls \<code\>").
2. Pair: POST /api/controls/pair with the code → save the token.
3. Launch: POST /api/launch/celeste → 202 queued. Confirm running_game=celeste in /api/status.
4. Wait for the title screen (see Step 1). Do NOT send input until the title screen is visible (early input may be eaten by the boot sequence).

### Step 1 — Title screen check

- What to expect: the Celeste logo over the mountain backdrop, with a "press any key/button" prompt (first boot) or the main menu. The screen should be fully rendered (not black, not a frozen Setup screen).
- Screenshot point: portal /ext/screenshot after the title screen appears. Save as shot-\<date\>-celeste-title.png in the goal hidden_files.
- Pass: logo + mountain + prompt/menu visible. Fail: black screen, frozen Setup screen, crash (app "Not running" in Dev Home), or a hang (log frozen).
- If the title screen shows a "press any key" prompt, send one key press (e.g., Enter down+up) to advance to the main menu.

### Step 2 — Menu navigation

- The main menu (first run, no save): "New Game" (or "Begin") is the top option; below it "Options", etc. (exact items depend on save state — on a fresh install there is no Continue).
- Send: Down arrow (press+release) → the selection should move down one item. Screenshot. Send Up arrow → selection moves back up. Screenshot.
- Send: Enter (or Z) on "New Game" → advances to file select (3 save slots). Screenshot.
- Send: Esc (or X) → backs out to the main menu. Screenshot.
- Pass: the selection visibly moves with each arrow press; Enter advances; Esc backs out. Fail: no response to arrows (input not reaching the guest — check pairing/token/cursor mode/2s reset), or the menu doesn't advance on Enter.

### Step 3 — Starting a level (new game)

- From the main menu: Enter on "New Game" → file select → Enter on an empty slot → the game starts Chapter 1: Forsaken City (intro cutscene: Madeline arrives at the mountain, the Granny scene).
- The intro is a short cutscene (dialogue boxes). Advance dialogue with Enter/Z (or wait — some lines auto-advance). Screenshot the intro.
- After the intro, gameplay begins: Madeline stands in the first screen of Chapter 1. Screenshot — this is the "gameplay started" evidence.
- Pass: Chapter 1 loads, the intro plays, Madeline is standing in the first screen and the game is waiting for input. Fail: black screen after file select, crash on level load, or a hang.

### Step 4 — Gameplay checks (Chapter 1)

Run these in order, screenshotting after each:

1. **Movement:** hold Right arrow (re-post the held state every <2s — see the 2s reset rule) for ~2s → Madeline runs right. Release. Hold Left ~2s → Madeline runs left. Screenshot each. Pass: Madeline visibly moves in the pressed direction.
2. **Jump:** press Z (down+up) → Madeline jumps. Screenshot mid-jump (send the screenshot request immediately after the press). Pass: Madeline leaves the ground. Repeat 2-3x to confirm reliability.
3. **Dash:** the dash is taught partway through Chapter 1 (after the first few screens — the game grants it at the dash tutorial). Walk/jump right through the first screens until the dash tutorial appears (the game shows a prompt). Then press X mid-air → Madeline dashes (a burst of speed in the facing direction, with a white trail). Screenshot. Pass: the dash fires (Madeline lunges + the dash trail is visible). Note: the dash has one charge per landing (it refills on touching ground).
4. **Death/respawn:** jump into the first pit (or spikes, when they appear) → Madeline dies (a short death animation) → instant respawn at the screen's start. Screenshot after respawn. Pass: death + respawn works, no crash, no soft-lock.
5. **Screen transition:** walk right off a screen edge → the next screen loads seamlessly (no loading screen). Screenshot. Pass: the next screen appears, Madeline is controllable.
6. **Pause menu:** press Esc → the pause menu opens (Resume, Restart Chapter, Save & Quit, Options). Screenshot. Press Esc (or Enter on Resume) → returns to gameplay. Pass: the pause menu opens and closes cleanly.
7. **Climb (Chapter 2):** the climb/grab mechanic unlocks in Chapter 2 (Old Site) — it CANNOT be tested in Chapter 1 (pressing C there does nothing). After completing Chapter 1 (or via Chapter Select if a save with Chapter 2 unlocked exists), in Chapter 2: hold C against a wall → Madeline grabs and climbs (Up/Down to climb, stamina drains). Pass: Madeline grabs the wall and climbs while C is held. NOTE: if the run is time-boxed, climb is the ONE check that may be deferred to a follow-up run — the core 'playable' bar (below) does not require it on the first pass, but it must be verified before the game is called fully playable.

### Step 5 — What 'playable' means concretely

All of these must hold:

- [ ] Title screen renders and the menu responds to input.
- [ ] A new game starts and Chapter 1 loads without a crash or hang.
- [ ] Movement (left/right), jump, and dash all respond to the correct keys.
- [ ] Death/respawn works; screen transitions load; the pause menu opens and closes.
- [ ] No soft-locks: the game never stops responding to input for >5s (excluding the 2s input-reset behavior, which is a host artifact, not a game soft-lock).
- [ ] Framerate feel: judge from repeated screenshots during movement — Madeline should move smoothly without visible hitching or long freezes. If the host logs frame times, note them. (Celeste targets 60fps on PC; through the bridge, "smooth enough to play" is the bar — visible stuttering that makes jumps mistime is a fail.) Perf analysis (celeste-interpreter-perf.md, 18:32 EST) expects 60fps in light scenes, 30-50fps in busy scenes (slow-motion, not slideshow) — the interpreter is ~10-15x slower than JIT on the managed update loop, but rendering (D3D11) and audio (FMOD) run natively at full speed. If frame times are logged, record avg + p95 (timestamp Present).
- [ ] Input responsiveness: a key press should be visible in the next screenshot (within ~1s of the press). Systematic lag (presses taking seconds to register) is a fail.
- [ ] Climb verified in Chapter 2 (may be a follow-up run — see Step 4.7).
- Audio is NOT verifiable remotely (no audio capture path) — Ben verifies music/SFX by ear. The FMOD bridge is real (verified offline), so audio is expected to work.

### Step 6 — Failure modes to watch for

- **Input not registering:** keys sent but nothing happens. Check in order: (1) paired? (re-pair if the token expired — the token clears on /api/controls/leave or app restart); (2) cursor mode on? (F8 was pressed — press F8 again to turn it off; never press F8 during gameplay); (3) 2s reset — was the last POST >2s ago? (re-post the held state); (4) is the guest actually running? (/api/status running_game=celeste); (5) is the app focused? (a CoreWindow deactivation releases all input — relaunch the app).
- **Rendering glitches:** black screen (game running but nothing drawn — check the log for FNA3D errors), missing sprites, wrong colors, a frozen frame. Screenshot + log tail for the report.
- **Audio missing:** can't verify remotely — note it for Ben to check by ear.
- **Crash on level load / screen transition:** app "Not running" in Dev Home, or the log stops. Note the exact transition (which chapter/screen) + the last log lines.
- **Soft-lock:** the game stops responding but the app/server is alive (log frozen, /api/status still responds). Distinguish from a hang (server dead) — report which.
- **Dash not firing:** the dash isn't available until the Chapter 1 tutorial grants it — don't report it as a failure before the tutorial. Similarly, climb does nothing in Chapter 1 (Chapter 2 mechanic).
- **Stuck keys:** if a key appears stuck (Madeline keeps moving after release), send `r 0 0` (reset) + POST /api/controls/leave, then re-pair.

## 3. Evidence to capture (for Ben)

- Screenshot: title screen (shot-\<date\>-celeste-title.png).
- Screenshot: main menu + file select.
- Screenshot: Chapter 1 first screen with Madeline controllable.
- Screenshot: mid-jump + mid-dash.
- Screenshot: pause menu.
- Log tail around each step (from /api/logs) if anything fails.
- Save screenshots to ~/workspace/goals/tiramisu-xbox-pe64-runtime/hidden_files/ on the Muse VM (via the base64 download route) and attach the title + gameplay shots when reporting the milestone to Ben.

## 4. Input helper (recommended)

Write a small Python helper (send-celeste-input.py) on mineplex that:

- Pairs once (reads the code from an env var or arg, POSTs /api/controls/pair, saves the token).
- Sends a list of events (POST /api/controls/input with the Authorization header).
- Supports holds: a `hold <vk> <seconds>` mode that re-posts the key-down every 1s (beating the 2s reset) and sends key-up at the end.
- Supports taps: a `tap <vk>` mode (down, 100ms sleep, up).
- Logs every POST + response code (for debugging input failures).

## 5. Sources (code-verified 2026-11-20)

- Web control API: ~/tiramisu-brown-merge/src/host/webxfer.cpp (parse_control_events ~898, handle_controls ~927, reset_web_controls ~130, webxfer_control_events ~1825) + src/host/webxfer.h (web_control_event ~74).
- Input consumer: ~/tiramisu-brown-merge/src/host/main.cpp (apply_web_controls ~1976, handle_host_cursor_key ~296, is_numpad_cursor_vk ~273, is_f8_cursor_vk ~284, poll_host_gamepads ~379, g_kbd_mouse_mode default false ~104).
- FNA input: ~/tiramisu-brown-merge/src/trt/fna_platform.cpp (PollEvent ~69, GetMouseState ~78, GetPadState ~108, registration ~183).
- Pad state: ~/tiramisu-brown-merge/src/trt/input.cpp (trt_input_pad_state_n ~427) + src/trt/input.h (~40-73).
- Celeste's default controls: the game's standard defaults (keyboard: arrows move, Z jump, X dash, C climb, Esc pause; gamepad: stick/D-pad move, A jump, X dash, RT climb, Start pause). Not read from game files (the game copy was never inspected beyond transport metadata).
- Interpreter perf: ~/tiramisu-gow-skye/docs/progress/celeste-interpreter-perf.md (18:32 EST) — likely playable, 60fps light / 30-50fps busy; AOT compilation is the mitigation if too slow.
