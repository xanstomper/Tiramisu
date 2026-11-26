# Hermes — a native tool-using agent guest

Hermes is a freestanding Win64 PE that Tiramisu maps **in-process** on the
console. The agent loop, JSON codec, HTTP client, shell and file tools run on
the Xbox; only model inference leaves the LAN. It draws its own D3D11 terminal
and runs an interactive REPL, so launching it stays in a usable session instead
of flashing back to the shelf.


```
Xbox (Tiramisu UWP host, AppContainer)
  └── hermes.exe (PE64, no CRT)
        ├── tools: list_dir, read_file, write_file, append_file,
        │          make_dir, stat, delete_file, edit_file, search_files,
        │          list_tree, write_code, scaffold_project (real Win32 file APIs,
        │          multi-language coding)
        ├── tools: shell (built-in pwd/ls/cat/mkdir/rm/echo) and sysinfo
        ├── ChatGPT-style REPL over a D3D11 terminal (keyboard input)
        ├── OpenAI-compatible tool-calling loop
        └── plain HTTP  ──────────────►  PC relay (TLS + provider key)
                                                └── api.openai.com / any
                                                    OpenAI-compatible API
```

Why a relay: Tiramisu has no Schannel bridge yet, so a guest cannot terminate
TLS itself. The relay is stdlib-only Python and runs on any PC on the LAN.

## Files

| Path | Purpose |
| --- | --- |
| `src/guest/hermes.c` | the agent and interactive session (single TU, no libc) |
| `src/guest/hermes_ui.c` | D3D11 terminal: grid, scrollback, line editor, history |
| `src/guest/font8x8.h` | public-domain 8x8 glyphs used by the screen |
| `src/guest/gsprobe.c` | diagnostic PE: dumps the guest GS/TEB fields to `C:\gsprobe.txt` |
| — | CPython 3.13.5 embeddable was used only as a probe; it is not shipped |
| `tools/hermes_relay.py` | PC inference relay, OpenAI proxy or `--mock` |
| `tools/build-hermes.sh` | cross-build + upload zip |
| `C:\hermes\config.ini` on console | `endpoint=`, `model=`, `task=`, `workspace=`, `token=`, `max_steps=` |
| `C:\hermes\hermes.log` | append-only run log |
| `C:\hermes\status.json` | live step/phase for remote polling |
| `C:\hermes\out\answer.md` | final assistant message |
| `C:\hermes\workspace\` | default tool root |

Config is read from **beside the exe first** (`Z:\hermes\config.ini`, i.e. the
uploaded zip), then `C:\hermes\config.ini`. That keeps an uploaded package
self-contained while still allowing an on-console override.

## Build and deploy

```bash
# 1. relay on the PC (mock first proves the loop; no key needed)
python3 tools/hermes_relay.py --port 8790 --mock
# or, real inference:
HERMES_API_KEY=sk-... python3 tools/hermes_relay.py --port 8790

# 2. build the guest + zip (endpoint points at the PC)
RELAY_URL=http://<pc-ip>:8790/v1/chat/completions tools/build-hermes.sh

# 3. upload and launch through Tiramisu's LAN server
curl -X PUT --data-binary @out/hermes-pkg/hermes.zip \
     http://<xbox-ip>:9127/upload/hermes.zip
curl -X POST http://<xbox-ip>:9127/api/launch/hermes

# 4. read the result from the console (Device Portal, no auth on this unit)
#    LocalAppData -> \LocalState\drive_c\hermes\out\answer.md
```

If the PC firewall is default-deny, allow the relay port from the console
subnet only, e.g. `ufw allow from 10.0.0.0/24 to any port 8790 proto tcp`.

## Console screen

GDI text is record-only in this host (`TextOutW` logs "glyphs not drawn"), and
shaders add a second failure surface. `hermes_ui.c` therefore rasterises the
whole frame on the CPU (8x8 font, 80-column grid, scrollback) and uploads it
with `ID3D11DeviceContext::UpdateSubresource` straight into the host
swapchain's backbuffer, then calls `Present`. Only dirty frames are presented,
and the cursor is drawn by inverting one cell.

Input is real: the guest owns the window, so `WM_CHAR` reaches the line editor
(USB keyboard on the console, any keyboard under Wine).

| Key | Action |
| --- | --- |
| Enter | send the line to the agent |
| Up / Down | recall history |
| Left / Right, Home / End | move the caret |
| Backspace, Delete | edit |
| PgUp / PgDn | scroll the transcript |

Built-in commands: `/help`, `/clear` (or `/new`), `/quit` (or `/exit`). A
request runs the full loop — relay call, tool calls printed as `> tool`, tool
output shown indented, then the final answer — and keeps the conversation in a
persistent JSON history until `/clear`. The status bar shows `READY`,
`THINKING`, `REQUEST`, `TOOLS` or `ERROR`; the bottom rows list the tool set and
key help.

## Safety

* Tool paths are confined to the workspace; `..` components are rejected.
* Read/write budgets are capped (4 MiB read, 60 KB returned to the model).
* The agent never calls `ExitProcess`; `/quit` returns to the loader and keeps
  the host shelf alive.
* No `CreateProcess`: `shell` is a built-in command interpreter (`pwd`, `ls`,
  `cat`, `mkdir`, `rm`, `echo`, `sysinfo`, `help`) implemented with the same
  confined file APIs, so it cannot launch host processes.

## Verification performed

* Wine + Xvfb, keyboard driven with `xdotool`: typed a request, watched the
  line editor echo it, then the mock relay's 3-tool loop printed
  `> list_dir`, `> write_file`, `> read_file` with their outputs and the final
  answer; `hermes-proof.txt` was created in the Wine workspace.
* Wine + Xvfb against a second mock that requests `sysinfo` then
  `shell pwd`/`ls`: `sysinfo` reported `os=10.0.19045`, pid, uptime, workspace,
  module path and the tool list; `shell` returned `C:\hermes\workspace` and
  `[file] 60  hermes-proof.txt`. This exercises `RtlGetVersion` (ntdll bridge)
  through the real guest import.
* Console (Series S, SystemOS 26100.9662): Hermes was re-deployed and booted
  on the current releases, 0.2.4.13 and 0.2.4.14 (`Xan.Tiramisu`); in both the
  Device Portal screenshot shows the terminal rendered at 1920x1080 with
  `READY`, the model/relay/workspace block, the prompt cursor and the tool/key
  footer, while `api/status` reports `guest_running: true` and
  `drive_c\hermes\hermes.log` has `ui ready=1` / `winsock ready`.
* `gsprobe` on 0.2.4.14 proved the guest GS base and stack limit are real (see
  the opencode section), which rules out the offline `%gs` fault as the
  on-console cause.
* Offline: `tests/test_engine` covers the `FindFirstFileA`/`FindNextFileA`
  wrappers (135 PASS, ALL PASS); `test_conformance` stays ALL PASS with the
  kernel32 ABI entries.
* The tool-schema JSON is validated before sending: a missing brace in the
  hand-written `delete_file`/`shell`/`sysinfo` entries produced `relay http
  400`; the request body is now parsed by `json.loads` cleanly (3310 bytes for
  a one-line session).
* Artwork: Nous Research `logo-dark.png` from the public hermes-agent repo,
  uploaded per game as `cover.jpg` / `hero.jpg`.

## Real inference

The relay is provider-agnostic (`HERMES_UPSTREAM` + `HERMES_API_KEY` or
`HERMES_API_KEY_FILE`). At the time of this work the locally configured
opencode providers were unusable: `vyceai` returned 503 (model unavailable),
`freeinference` was paused, the `opencode-go` keys had expired and the plain
`opencode` key had no balance. Set a working key and point the agent's
`config.ini` at the relay; the tool schema is already OpenAI-compatible.

## Why the real Hermes Agent (Nous Research) cannot run here

The upstream agent is `NousResearch/hermes-agent`: a Python package installed by
`install.ps1` as a **git checkout + `uv`-managed CPython venv** (`uv` 0.13.5,
Python 3.11+, Textual/prompt_toolkit front-ends, Rust C extensions such as
pydantic-core, `ssl`, `sqlite3`). "Native" therefore means *CPython running
inside the Tiramisu guest*, plus a real console for the TUI. Both were measured
on 0.2.4.14 rather than assumed:

1. **CPython itself dies.** `python-3.13.5-embed-amd64` was uploaded as a game
   (`hello.py` writing `sys.version` to `C:\pyhello.txt`) and launched. The log
   shows the image map, `.pdata` registration, the CRT imports binding (18
   runtime + math/stdio/locale/heap + 15 kernel32), then
   `SetUnhandledExceptionFilter(filter=000001DE88DE1964)` (CPython's own crash
   handler), `GetModuleHandleW(NULL)`, and immediately
   `real-exit: kernel32!TerminateProcess called from 0x7ff820720804` — the
   fatal path — followed by a host restart. `C:\pyhello.txt` is never written,
   so `Py_Main` never reached interpreter initialisation. The same binary in
   the offline harness (`tests/out/test_engine python.exe ...`) exits 139 with
   no named crash report.
2. **The C runtime is synthesised, not real.** That offline run shows 24 import
   slots resolved to fail-visible thunks, including `memset`, `memcpy`,
   `_initterm`, `_set_app_type`, `_configure_wide_argv`, `exit` — VCRUNTIME140
   and `ucrtbase` are stubs here (games bundle their own CRT next to the exe,
   so they never notice). Anything that links the system CRT cannot run.
3. **There is no console at all.** `GetConsoleMode`, `WriteConsoleW` and
   `SetStdHandle` all report `no console (AppContainer); lasterr=6`;
   `AllocConsole` only records a headless flag and `WriteFile` to stdout is a
   silent no-op. A TUI needs a screen buffer, cursor, resize, ANSI/VT parsing and
   keyboard records; none of that exists.

So booting the real Hermes TUI natively is not a packaging or launch-argument
problem: it needs a genuine C runtime and a real console subsystem inside the
runtime first. Until then this repository ships `hermes.exe` — a native Win64
guest written against the same bridge games use, rendering its own D3D11
terminal — rather than a shim pretending to be the upstream agent.

## Native opencode

opencode is MIT and provider-agnostic, and its server/OpenAPI surface would fit
this guest well, so the real Windows binary (`opencode-windows-x64-baseline`
v1.19.6) was tested natively rather than through a shim. It does not run yet.

On the 0.2.4.14 release it gets much further than the offline harness suggests:
`opencode.exe --version` and `serve --port 4096 --hostname 0.0.0.0` both map,
the 429-symbol import table binds, `GetCommandLineW` shows the right argv, Bun
creates its `AppData\{Local,Roaming}\opencode` directories, reserves a 1 GB
heap, wakes its 10 worker threads and opens `Z:\nul`. Then the app dies
silently: port 4096 never listens, no opencode log is written, and the LAN
server goes down with it.

An offline run (`tests/out/test_engine /tmp/oc/opencode.exe ...`) faults at
`0x1451d05ac` on `movq %gs:0x10, %r11` (Bun's stack probe) because the Linux
test process has no guest TEB. On the console that is **not** the failure:
`src/guest/gsprobe.c` (a 2.5 KB freestanding PE) reads
`gs_08_stackbase=000000d2f4000000`, `gs_10_stacklimit=000000d2f3fb6000`,
`rsp=000000d2f3fbe8f0`, i.e. GS and the guest stack limit are valid. What is
missing is the next layer Bun needs after stdio: Tiramisu logs
`SetStdHandle -> no console (AppContainer); lasterr=6` three times just before
the process goes away, so a guest console/terminal device (and the libuv
primitives layered on it) is the concrete gap, not GS.

The tile stays off the shelf until that runtime work lands; the host-side
`ProcessPrng`, `RtlWaitOnAddress`, `RtlAddGrowableFunctionTable` and
`GetCurrentThreadStackLimits` bridges added along the way are kept
(compile-verified) for it.

## Related fixes in this change

* `FindFirstFileA`/`FindNextFileA` bridges in `bridge_kernel32.cpp` (ANSI
  wrappers over the W family) because guest `C:\` paths must go through the
  VFS, not the host's real filesystem.
* `cover_art.cpp` no longer accepts the first Steam store result for a folder
  name: it requires a shared 3+ character word. The synthesizer had guessed
  AppID 3264440 (Hello Kitty Island Adventure) for the folder `hermes`.
