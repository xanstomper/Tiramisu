# Celeste hang analysis — why is everything frozen (offline, 2026-11-20 ~18:35 EST)

Scope: read-only analysis of `/tmp/opencode/t114.log` (3,595 lines, the .112/.113/.114
hangs) + `src/trt/mono_host.cpp` + `src/host/host_log.cpp` + `src/host/webxfer.cpp` +
`src/host/main.cpp` in `~/tiramisu-brown-merge`. No console touch, no builds.

## The symptom set (all four freeze at the same instant, right after "invoking managed Main")

1. Log freezes (byte-identical, last line "invoking managed Main", 3 versions).
2. Watchdog thread NEVER logs once (0 occurrences of "mono watchdog" in 3,595 lines).
3. UI loop heartbeat (every 60s) stops — .114 session shows only "up 0s", no "up 60s".
4. `/api/status` goes empty (server stops responding).

## Ranked diagnosis

### H1 (TOP) — Heap-lock freeze: a thread stuck inside the CRT allocator

**Mechanism.** A thread — most likely the guest thread, inside Mono/SGen/FNA
allocation during managed Main — is stuck inside malloc/free/new/delete (most
likely traversing a heap free-list corrupted into a cycle, i.e. an infinite
loop inside the allocator, or a heap operation that never returns). The CRT
heap lock is held. Every other thread that allocates blocks:
- watchdog: `mono_log` → `log_line` → `ring_push` allocates a `std::string`
  (`host_log.cpp:60-66`).
- UI heartbeat: `log_line(hb)` → `ring_push` → same allocation.
- `/api/status`: `json_escape` allocates `std::string`s (`webxfer.cpp:1092-1117`).
- log file: no thread can complete `log_line`, so the file freezes.

**Why this is the top hypothesis.** The CRT heap lock is the ONLY single lock
common to all four frozen paths. `g_file_mu` is shared by the watchdog, the UI
loop and the log — but NOT by `/api/status`, which takes `g_mu` (webxfer's own
state mutex) and never calls `log_line` (`webxfer.cpp:1092-1117`). A held
`g_file_mu` therefore CANNOT explain the server dying; the heap lock can.
No crash dump is consistent (an infinite loop doesn't crash). TV frozen is
consistent (UI loop blocked). Reproducible at the same point three times is
consistent (managed Main starts allocating → hits the corruption).

**Code locations.**
- `src/host/host_log.cpp:60-66` — `ring_push` allocates `std::string` under `g_ring_mu`.
- `src/host/webxfer.cpp:1092-1117` — `/api/status` takes `g_mu`, `json_escape` allocates.
- `src/trt/mono_host.cpp:456-466` — watchdog: `mono_log` → `log_line` → `ring_push`.

**Fix.**
1. Find the heap corruption (debug heap / Application Verifier / pageheap on a
   dev build; or add heap-validation calls around the FNA bridge + Mono alloc
   paths). Suspect areas: the RWX-fallback patches (29/30) touch allocation
   bookkeeping; the FNA native bridge is called from managed code.
2. Defensive: make the logging path allocation-free — `ring_push` with a
   fixed-size `char[221]` ring instead of `std::string`; `json_escape` with a
   stack buffer.
3. The tracing build's writer MUST ALSO be allocation-free (no `std::string`,
   no malloc in the trace path) or it will hang on the heap lock exactly like
   `log_line` does.

**What the tracing build would show.** If the trace writer is allocation-free
and mutex-free, it will KEEP WRITING during the hang (watchdog + guest markers
appear in tiramisu.log) — CONFIRMING the hang is in the log/heap path (the
trace bypasses it). If the trace ALSO freezes, the hang is below the trace
(process-wide: CPU starvation, suspension, or a lower-level lock) — and the
trace's last line pinpoints where. ADD a heap-health probe to the trace: the
watchdog thread does a periodic small malloc/free — if the malloc hangs, the
heap lock is the culprit; if it succeeds, the heap is fine and the block is
elsewhere.

### H2 — Log deadlock (g_file_mu held by the guest thread stuck in log_line)

**Mechanism.** The guest thread, inside `mono_jit_exec`, enters `log_line`
via Mono's print/log handler (managed `Console.WriteLine` or a Mono runtime
message) and gets stuck inside `log_line` (in `fflush`/`fputs`) while holding
`g_file_mu`. The watchdog, UI heartbeat, and any other logger block on
`g_file_mu` forever.

**Evidence.** (a) Mono's handlers DO route through `log_line`:
`mono_print_to_log` / `mono_log_to_log` / `mono_unhandled_hook` / `mono_veh`
all call `mono_log` → `g_log` → `log_line` (`mono_host.cpp:143-166`, handlers
installed at `mono_host.cpp:273-275`). (b) `MONO_LOG_LEVEL=debug` is set
(`mono_host.cpp:337`), so Mono logs frequently. (c) The watchdog + heartbeat +
log freeze are all consistent with a held `g_file_mu`. (d) No managed console
output exists in the log — Main hangs before printing anything, consistent
with hanging inside the first log call.

**Gaps (why this is #2, not #1).** (a) NO identified mechanism for
`fflush`/`fputs` to hang for 75s+ — fflush doesn't normally block, the disk
isn't full, and the log is 261K (far below the 8 MB rotation cap, so
`rotate_locked` is not involved). The "stuck in log_line" step is unproven.
(b) Does NOT explain `/api/status` going empty (it doesn't use `g_file_mu`).
Under H2 the server dying needs a separate, independent cause.

**Code locations.**
- `src/host/host_log.cpp:96-104` — `log_line` (takes `g_file_mu`, `fflush` on
  every line).
- `src/trt/mono_host.cpp:143-158` — Mono handlers → `mono_log`.
- `src/trt/mono_host.cpp:456-466` — watchdog → `mono_log`.

**Fix.** (1) Make `log_line` non-blocking (`try_lock` with a timeout; on
timeout, drop the line or write to the trace file instead of blocking).
(2) Remove `fflush` from the hot path (flush periodically or on a background
thread). (3) Route the watchdog + heartbeat through the mutex-free trace, not
`log_line`.

**What the tracing build would show.** If H2 is right, the mutex-free trace
(own FILE*, own mutex) will keep writing (watchdog + guest markers appear),
CONFIRMING the log mutex is the block. The guest markers will show Main
progressing (or stuck at a specific FNA call). If the trace shows the guest
stuck at a specific point AND the watchdog markers flowing, H2 is confirmed
and the guest hang is a separate (managed) hang.

### H3 — Guest-side mono_valloc_aligned NULL (patch 30 missing from .112-.114)

**Mechanism.** During managed Main, Mono calls `mono_valloc_aligned(RWX)`
(code-manager aligned chunk). Patch 30 (the aligned RWX fallback) is NOT in
.112/.113/.114 — it was created 18:11 EST, after those builds; the lib was
rebuilt 18:14. Direct RWX `VirtualAlloc` is denied → returns NULL → the
caller either crashes (no dump seen — unlikely) or enters a retry/wait loop.

**Evidence.** (a) Patch 30 exists and postdates .112-.114. (b) Patch 29 (the
unaligned fallback) works — .112 gets past `mono_jit_init`. (c) The hang
starts right after "invoking managed Main" (when the interpreter first needs
code/trampolines).

**Gaps.** (a) Doesn't explain the watchdog/server/UI freeze (a guest-side NULL
doesn't block other threads unless it holds a shared lock). (b) Unproven that
`mono_valloc_aligned` is called during Main in interpreter mode. (c) A NULL
return usually crashes, not hangs. Note patch 30 itself has a leak: if the
RW→RWX flip fails it sets `aligned = NULL` without freeing the RW pages, and
`g_assert(aligned)` follows (abort if asserts are on, silent NULL if off).

**Fix.** Include patch 30 in the next Mono rebuild (the current lib at 18:14
already has it — verify via the build log). Audit `mono_valloc_aligned`
callers for NULL checks.

**What the tracing build would show.** If H3 is right AND the tracing build
uses the NEW lib (with patch 30), the hang may DISAPPEAR (Main proceeds past
the aligned alloc). If the hang persists with patch 30, H3 is refuted.

### H4 (LOW) — Watchdog thread never started

**Against.** `std::thread` works elsewhere in the app (guest thread
`main.cpp:2885`, server listener `webxfer.cpp:1709`). A `std::thread`
constructor throw would propagate out of `trt_mono_run` (extern "C") —
typically process termination — or be caught and logged by
`run_guest_trapped`. Neither happened (process alive, no error line). Explains
only the watchdog, not the UI/server/log. The watchdog thread almost certainly
EXISTS and is BLOCKED.

**What the tracing build would show.** If the tracing build's watchdog (same
`std::thread` pattern) logs markers, thread creation works and H4 is refuted.

## Refuted

- **TerminateThread poisoning g_file_mu.** The force-kill fired ONCE at log
  line 2819 ("host: FORCE-KILL guest thread (join timeout)"), but logging
  continued for 700+ lines after it, and the .112/.113/.114 hangs are in
  LATER sessions (fresh processes, fresh mutexes). The code itself warns about
  this (`main.cpp:1087-1088`), but it is not what happened here.
- **FNA internal call logging/waiting.** `fna_platform.cpp` has NO log calls
  and NO Dispatcher/Wait/CoreWindow calls. The guest thread's only log path
  during `mono_jit_exec` is Mono's handlers. No guest→UI-dispatcher deadlock.
- **Plain managed hang (alone).** A managed hang on the guest thread would
  leave the watchdog + server alive. The watchdog silence + server death
  require a second mechanism. (The guest IS hung, but that alone doesn't
  explain the system-wide freeze.)
- **UWP suspension.** Foreground app, low memory (113 MB at launch), and the
  hang reproduces at the same point three times. A system suspension would not
  be this reproducible or this precisely timed.

## Answers to the four questions

(a) **Where could the guest thread hold g_file_mu during mono_jit_exec?**
Via Mono's print/log handlers (`mono_print_to_log`, `mono_log_to_log`,
`mono_host.cpp:143-158`), called from managed `Console.WriteLine` or Mono
runtime messages. Mono's logging DOES route through `log_line` (confirmed).
`MONO_LOG_LEVEL=debug` + `MONO_LOG_MASK=gc,thread` are set
(`mono_host.cpp:336-337`) — but ZERO gc/thread lines appear in the log, so
that diagnostic is not firing (compiled out, read too late, or no events).
The guest thread CAN be inside `log_line` holding `g_file_mu` — but WHY it
would get STUCK there (fflush hanging) is unidentified. That is H2's gap.

(b) **Does the watchdog thread actually start on UWP?** Almost certainly YES
(see H4). It exists and is BLOCKED (on `g_file_mu`, `g_ring_mu`, or the heap
lock). It is created at `mono_host.cpp:456`, right after "invoking managed
Main" is logged; first marker due at +3s; none ever appears in 75s+.

(c) **What else could freeze log + server + watchdog SIMULTANEOUSLY?** The
CRT heap lock (H1) — the only single lock common to all four paths. `g_file_mu`
alone cannot explain `/api/status` (it takes `g_mu`, never logs). A thread
stuck inside malloc/free freezes every allocating thread.

(d) **Is the watchdog's silence consistent with the log-deadlock hypothesis?**
YES for the watchdog/UI/log (all use `log_line` → `g_file_mu`), but NO for
the server (`/api/status` doesn't use `g_file_mu`). The log-deadlock
hypothesis is INCOMPLETE — it explains 3 of 4 symptoms. The heap-lock
hypothesis (H1) explains all 4.

## Recommendations for the tracing build (critical)

1. The trace writer MUST be allocation-free (no `std::string`, no malloc, no
   CRT heap in the trace path) — otherwise it hangs on the heap lock exactly
   like `log_line`. Use a fixed-size stack buffer + `WriteFile` (or `fwrite`
   to a pre-opened FILE*) with no allocation.
2. The trace writer MUST use its own mutex (not `g_file_mu`/`g_ring_mu`) —
   as planned.
3. ADD a heap-health probe: the watchdog thread does a periodic small
   malloc/free — if the malloc hangs, the heap lock is the culprit; if it
   succeeds, the heap is fine and the block is elsewhere.
4. Include patch 30 in the Mono lib for the tracing build (tests H3 at the
   same time).
5. ADD a `try_lock` + timeout diagnostic to `log_line`: if `g_file_mu` can't
   be acquired within ~5s, write a marker to the trace file instead of
   blocking — this directly tests H2.
6. Guest markers should bracket each FNA internal call and each Main phase
   (before/after), so the trace shows exactly where Main stops.
