// tiramisu/src/trt/trt.h
// Tiramisu Runtime (trt) public API — Wine-style in-process PE64 loader
// for UWP hosts (Xbox Dev Mode) with a pluggable memory backend and
// import resolver so the same core runs in offline tests on Linux.
#ifndef TIRAMISU_TRT_H
#define TIRAMISU_TRT_H

#include "pe.h"

#ifdef __cplusplus
extern "C" {
// Fault attribution for the fault loggers: 1 when addr belongs to a loaded
// guest image (the title's fault), 0 for a host/unknown address.
int trt_fault_is_guest(const void* addr);
#endif

// ---- guest ABI ----
// Functions the GUEST calls directly (imported by the guest, or reached
// through a COM vtable) must use the Win64 calling convention. On the
// console host that is the default; on a Linux test host the compiler
// would use SysV and every argument would arrive in the wrong register.
// ms_abi makes the code guest-correct on any host, which also removes the
// hand-written trampolines the test harness used to need.
#if defined(_WIN32)
#  define TRT_GUEST_ABI
#elif defined(__x86_64__) && defined(__clang__)
#  define TRT_GUEST_ABI __attribute__((ms_abi))
#else
#  define TRT_GUEST_ABI
#endif

// ---- protection bits (backend-translated) ----
#define TRT_PROT_R 1u
#define TRT_PROT_W 2u
#define TRT_PROT_X 4u

// ---- platform backend: what the loader needs from the host process ----
// UWP discipline: alloc returns RW memory, never RWX; exec pages exist only
// after seal flips them RX (VirtualProtectFromApp). Zero RWX, ever.
typedef struct trt_backend {
  // size in bytes, page-aligned internally; returns NULL on failure
  void* (*alloc)(size_t bytes, u64 preferred_base /*0 = anywhere*/);
  // change protection of [base, base+bytes); returns 0 on success
  int   (*protect)(void* base, size_t bytes, unsigned prot);
  void  (*release)(void* base, size_t bytes);
  void  (*log)(const char* line);           // line without trailing \n
  // register the guest's .pdata unwind ranges with the OS unwinder —
  // REQUIRED for C++ exceptions in guest code (x64: RtlAddFunctionTable).
  // Return 0 on success. NULL = unsupported (test backend).
  int   (*register_unwind)(void* image_base, const void* pdata, u32 count);
} trt_backend;

// ---- import resolver: module+symbol -> callable address ----
// Return NULL to fail the load with TRT_ERR_IMPORT (name recorded).
typedef struct trt_resolver {
  void* user;
  void* (*resolve)(void* user, const char* module, const char* symbol,
                   u16 ordinal /*0 = by name*/);
} trt_resolver;

// ---- mapped image ----
#define TRT_MAX_TLS_CALLBACKS 256

typedef struct trt_image {
  u8*  base;              // actual load base
  u64  preferred_base;    // ImageBase from headers
  u64  size_of_image;
  u64  entry_rva;         // 0 = none
  u64  tls_callback_rvas[TRT_MAX_TLS_CALLBACKS]; // RVAs into the image
  int  tls_callback_count;
  u32  section_count;
  char last_detail[256];  // human-readable detail of last error/stage
  // STAGE 7: the guest TEB (ThreadLocalStoragePointer at gs:[0x58]) built
  // for images with a TLS directory; the guest gs base points here while
  // guest code runs (host gs is restored after). NULL = no guest TLS.
  void* guest_teb;
  // STAGE 7: exception directory (.pdata) for RtlLookupFunctionEntry scans
  void* pdata;            // image-relative pointer (base+pdr_va)
  u32   pdata_count;      // number of RUNTIME_FUNCTION entries
  // M7 Phase 1 (PE32): set for 32-bit images only, zero for PE64.
  int  is_pe32;            // 1 = PE32 image (32-bit guest; load-only in Phase 1)
  u32  guest_base;         // guest-VA load base (guest address space, not host)
  u32  reloc_fixups;       // HIGHLOW/DIR64 fixups applied by the reloc pass
  u32  pe32_trap_count;    // IAT/delay slots bound to trap addresses (PE32)
  u32  pe32_guestva_count; // IAT slots bound to sibling guest-VA exports (PE32)
  u32  tls_data_rva;       // PE32 TLS raw-data RVA (0 = no TLS dir)
  u32  tls_data_size;      // PE32 TLS raw-data size
  u32  tls_zero_fill;      // PE32 TLS zero-fill size
  u32  tls_index_rva;      // PE32 TLS index-slot RVA (slot left unwritten: the
                           // Phase 2 emulator assigns the FS-based TLS index)
 } trt_image;

// STAGE 7: the shared process guest TEB (gs:[0x08] StackLimit, gs:[0x10]
// StackBase, gs:[0x30] Self, gs:[0x58] ThreadLocalStoragePointer). One TEB
// per process: every image's TLS block is visible to every other image's
// code, and guest->bridge->guest dispatches keep working. Created on first
// use (acquire) — images with no TLS directory still need it installed.
void* trt_guest_teb_acquire(u32* out_index);
void* trt_guest_teb_shared(void);
int   trt_guest_tls_register(const u8* raw, size_t raw_size, size_t total_size,
                             u32* out_index);
void  trt_guest_tls_sync_thread(void);
void** trt_guest_tls_array(void);
void  trt_guest_tls_bind_host_slots(void** host_tls, void** guest_tls);

// Map a PE64 from a file image. Read-only view of [file, file+file_size).
// Stages: validate -> alloc -> headers+sections -> relocs -> imports -> TLS.
// Pages stay RW until trt_seal_image().
// M7 Phase 1: a PE32 file (machine 0x014c, magic 0x10b) routes to
// trt_map_pe32 automatically; anything else that is neither PE64 nor PE32
// fails TRT_ERR_NOT_PE64.
int trt_map_pe(const trt_backend* be, const trt_resolver* rs,
                const u8* file, size_t file_size, trt_image** out);

// Map a PE32 (32-bit) image: same stages as trt_map_pe, with 4-byte thunks,
// HIGHLOW relocations against a GUEST-VA base (independent of the host
// mapping), a 32-bit TLS directory, and trap-address import binding (a
// 32-bit IAT slot cannot hold a 64-bit pointer: bound imports get a
// reserved guest-VA trap address, see TRT_PE32_TRAP_BASE in pe.h; imports
// from a sibling 32-bit DLL bind to the export's guest VA instead).
// LOAD-ONLY: nothing in the image executes (no x86 engine until Phase 2);
// the entry point and TLS callbacks are collected, never called.
int trt_map_pe32(const trt_backend* be, const trt_resolver* rs,
                 const u8* file, size_t file_size, trt_image** out);

// ---- M7 Phase 1: 32-bit trap bindings ----
// One registry entry per unique bound module!symbol (deduped across the
// process); trap N lives at TRT_PE32_TRAP_BASE + 4*N. The registry is the
// audit trail for the 32-bit import binds and the Phase 2 dispatcher's
// input (it maps a trap address back to the module!symbol + 64-bit bridge
// address). Process-global, never reset.
int trt_pe32_trap_count(void);
// Describe binding `index` (0..count-1). Any out pointer may be NULL.
// Returns 0 on success, -1 for an out-of-range index.
int trt_pe32_trap_info(int index, char* mod_out, size_t mod_cap,
                       char* sym_out, size_t sym_cap, u16* ordinal_out,
                       u32* trap_va_out);
// 1 iff `guest_va` is inside the reserved trap range.
int trt_pe32_is_trap(u32 guest_va);

// Bind a symbol the guest resolved AT RUNTIME (GetProcAddress) and return the
// guest VA to hand back: a trap address, which the x86 trap dispatcher turns
// into a bridge call with the guest's own stacked arguments. Deduped with the
// static import binder, so an imported-and-looked-up symbol shares one trap.
// Returns 0 when the trap table is full.
u32 trt_pe32_bind_dynamic(const char* module, const char* symbol, u32 ordinal,
                          void* host_fn);
// Release all guest-VA reservations (called by trt_images_reset between
// corpus PEs; images already mapped keep their recorded guest_base).
void trt_pe32_guest_reset(void);

// Flip sections to final protections (code -> RX). Call once after
// relocation/import/TLS writes are complete. Returns TRT_OK or TRT_ERR_SEAL.
int trt_seal_image(const trt_backend* be, trt_image* img);
// Undo the seal (all sections RW). Only the legacy 32-bit runner needs it,
// for the data copy-back into an image that trt_seal_image already made RX.
// The caller re-seals immediately: the image is never RWX.
int trt_unseal_image(const trt_backend* be, trt_image* img);

// ---- boot audit (offline gate for REAL games) ----------------------------
// Parses a PE's import + delay-import tables IN FILE SPACE (no mapping,
// no execution, the file bytes are never modified) and pushes every
// module!symbol through the REAL resolver — the same code path trt_map_pe
// uses to bind imports. On return *missing lists every "module!symbol"
// that failed to resolve, in table order.
// Handles PE64 (8-byte thunks) and PE32 (4-byte thunks) alike.
// Returns TRT_OK when the audit ran (check *missing to decide bootability);
// TRT_ERR_* for malformed PEs / usage errors.
int trt_audit_imports(const u8* file, size_t file_size,
                      void* missing_out /* std::vector<std::string>* */);

// ---- universal engine: import-surface enumeration (Phase 2) --------------
// Enumerate every import (direct + delay-load) from a PE file. Fills `up`
// (bounded by up_cap). Returns the number of entries written, or -1 if the
// file is not a readable PE. This is the "pull APIs to run any PC game"
// primitive: dump the game's full import surface.
int trt_pe_enumerate_imports(const char* path, trt_universal_profile* up,
                             uint32_t up_cap);

// free the image and its mapping
void trt_unmap(const trt_backend* be, trt_image* img);

const char* trt_errstr(int code);

// ---- module table: routing policy + builtin function registration ----
typedef enum {
  TRT_ROUTE_MISSING = 0, // module not registered: import from it fails
  TRT_ROUTE_SYSTEM = 1,  // console ships the real DLL; host binds it
  TRT_ROUTE_BUILTIN = 2, // Tiramisu implements it (function table below)
  TRT_ROUTE_STUB = 3     // exports exist, log when called, return 0
} trt_route;

// How much of a Win32 symbol Tiramisu actually implements. This is the
// number that predicts whether a title will work, and it is reported per
// title by tools/support-matrix.sh. "recorded" means the call is accepted
// and logged but has no real effect; "named-silent" means the call returns
// a truthful empty answer (or a named failure) with no payload.
typedef enum {
  TRT_IMPL_REAL = 0,        // real semantics, real result
  TRT_IMPL_RECORDED = 1,    // call accepted + logged, no real effect
  TRT_IMPL_NAMED_SILENT = 2,// truthful empty answer / named failure
  // A module Tiramisu claims to implement (BUILTIN route) does not export the
  // symbol. This is a genuine gap: the loader would fail. It used to fall
  // through to REAL in the coverage report, which is how a row could read
  // 100% while a title still could not bind.
  TRT_IMPL_UNRESOLVED = 3
} trt_impl_class;

typedef struct trt_named_fn {
  const char* name;
  void* fn;
  unsigned char impl_class; // TRT_IMPL_*, default 0 (REAL) for `{name, fn}`
} trt_named_fn;

void trt_module_register(const char* name, trt_route route,
                          const trt_named_fn* fns, int fn_count);
trt_route trt_module_route(const char* name);
// Override the implementation class of an already-registered symbol.
// Bridges call this at boot for functions whose behavior is recorded or
// named-silent. Returns 1 when the symbol was found and marked.
int trt_module_mark_impl(const char* module, const char* symbol,
                         trt_impl_class cls);
// Resolve a module!symbol through the same path the loader uses and report
// its implementation class. Returns 1 when the symbol is resolvable (and
// fills *cls_out); 0 when it is not. SYSTEM routes and game-dir exports are
// REAL; STUB routes are NAMED_SILENT. Used by the support-matrix report.
int trt_symbol_class(const char* module, const char* symbol, int* cls_out);
// Introspection over every registered module!symbol (conformance suite and
// support reporting). trt_module_export_at returns 0 when index is past the
// end. impl_class is the registered TRT_IMPL_* (0 = REAL), route the
// TRT_ROUTE_* of the owning module.
int trt_module_export_count(void);
int trt_module_export_at(int index, char* module, int module_cap, char* symbol,
                         int symbol_cap, int* impl_class, int* route);
// How many arguments this bridge pops for module!symbol, or -1 when there is
// no ABI entry. -1 is not harmless: the x86 trap dispatcher falls back to a
// 4-arg guess, which is how a 1-arg stdcall once ate OpenTTD's return address.
// The conformance suite requires every registered BUILTIN symbol to answer.
int trt_abi_arg_count(const char* module, const char* symbol);
// Offline bind-simulation (corpus dry-run): would this module!symbol bind?
// BUILTIN -> iff the named fn is registered; STUB/SYSTEM -> always yes
// (STUB binds fail-visible, SYSTEM binds via the console host). Unknown
// module or unknown BUILTIN symbol -> 0.
int trt_module_query(const char* module, const char* symbol);
int trt_module_query_ordinal(const char* module, u16 ordinal);
void trt_resolver_default(trt_resolver* out);
typedef void* (*trt_system_load)(const char* module, const char* symbol);
void trt_set_system_loader(trt_system_load fn);
// resolve one module!symbol through the module table (the resolver's
// implementation; exposed for the boot audit)
void* trt_resolve_module(void* user, const char* module, const char* symbol,
                         u16 ordinal);
// Optional runtime lookup. Only an actual registered/native export is returned.
void* trt_resolve_export(const char* module, const char* symbol, u16 ordinal);
extern void trt_stub_thunk(void);

// Register the module coverage a real PC game imports (SYSTEM/STUB routes).
// Never overwrites a module the bridges already registered.
void trt_modules_register_defaults(void);

// bridge + vfs boot (declared for hosts/tests; defined in bridge_kernel32.cpp)
void trt_bridge_boot(const trt_backend* be);
int trt_bridge_register_kernel32(void);
void trt_bridge_new_thread_id(u32 id);
void trt_vfs_mount(const char* drive_prefix, const char* host_path);
// Disc images: a read-only drive served straight out of a .iso/.bin/.img.
// Files are extracted lazily into the cache root; the image is never written.
struct trt_iso;
void trt_vfs_set_iso_cache_root(const char* host_dir);
int trt_vfs_mount_image(const char* drive_prefix, const char* host_image);
void trt_vfs_unmount_image(const char* drive_prefix);
struct trt_iso* trt_vfs_image_for_guest_utf16(const u16* guest_path,
                                             char* rel_out, size_t rel_cap);
int trt_vfs_is_image_utf16(const u16* guest_path);
int trt_vfs_image_materialize(const u16* guest_path, char* host_out,
                              size_t host_cap, int* is_dir_out);
void trt_vfs_set_cwd(const char* guest_dir);
int trt_vfs_get_cwd(char* out, size_t out_cap);
int trt_vfs_normalize_guest_utf16(const u16* guest_path, char* out,
                                  size_t out_cap);
int trt_vfs_resolve_utf16(const u16* guest_path, char* out, size_t out_cap);
int trt_vfs_resolve_utf16_rw(const u16* guest_path, int for_write, char* out,
                             size_t out_cap);
void trt_vfs_resolve_case_insensitive(char* path, size_t cap);
int trt_vfs_resolve_z_pair_utf16(const u16* guest_path, char* ov_out,
                                 size_t ov_cap, char* ro_out, size_t ro_cap,
                                 char* rel_out, size_t rel_cap);
int trt_vfs_is_z_deleted_rel(const char* z_rel, int is_child);
int trt_vfs_is_z_opaque_rel(const char* z_rel);
void trt_vfs_mark_z_deleted_utf16(const u16* guest_path, int is_deleted,
                                  int is_dir);
int trt_vfs_delete_file_utf16(const u16* guest_path, u32* win_err_out);
int trt_vfs_remove_dir_utf16(const u16* guest_path, u32* win_err_out);
int trt_vfs_ensure_parents(const char* host_path);
int trt_vfs_mkdir_p(const char* host_dir);
int trt_vfs_stat(const char* host_path, int* is_dir, u64* size_out);
void trt_bridge_set_exe_path(const char* zpath);
void trt_bridge_set_cmd_args(const char* args);
void trt_bridge_set_cwd(const char* guest_dir);
// STAGE 5 boot-determinism bridges (wininet/psapi/iphlpapi/imm32/crypt32/
// dbghelp+dbgcore): honest-runtime surfaces for DLLs whose console presence
// is unproven — bound BUILTIN so a game boots and the calls fail NAMED
int trt_bridge_register_wininet(void);
int trt_bridge_register_psapi(void);
int trt_bridge_register_iphlpapi(void);
int trt_bridge_register_imm32(void);
int trt_bridge_register_crypt32(void);
int trt_bridge_register_prng(void);
int trt_bridge_register_dbghelp(void);
// UTF-16 -> UTF-8 helper (paths, strings)
int trt_utf16_to_utf8(const u16* in, char* out, size_t out_cap);
// the bytes behind a guest file handle (the memory-mapping bridge)
void* trt_file_bytes(void* handle, size_t* size);
int trt_file_retain(void* handle);
u64 trt_thread_handle_for_current(void);  // real handle for GetCurrentThread() duplicates
int trt_retain_sync_handle(u64 handle);  // DuplicateHandle retain for 0x5Bxxxxxx handles
void trt_fls_on_thread_exit(void);       // invoke non-null FLS callbacks on exiting thread
void trt_queue_io_apc(void* fn, u64 err, u64 bytes, void* overlapped);
int trt_file_release(void* handle);
int trt_file_stat_size(void* handle, u64* size_out);
// Host descriptor + real size behind a guest file handle (bridge_kernel32.cpp).
// -1 when the handle is not a regular host file. bridge_mem.cpp uses it to
// file-back read-only views (map the SSD's page cache instead of copying it
// into committed RAM).
int trt_file_host_fd(void* handle, u64* size_out);
int trt_file_pread(void* handle, u64 offset, void* dst, size_t bytes,
                   size_t* read_out);
int trt_file_pwrite(void* handle, u64 offset, const void* src, size_t bytes,
                    size_t* written_out);
int trt_bridge_register_user32(void);
// host pump: called from the builtin GetMessageW while the guest blocks, so
// CoreWindow events + XInput keep flowing after the guest owns the thread
void trt_set_pump(void (*fn)(void));

// ---- STAGE 7 EXE-entry boot harness hooks ----
// TEST-HARNESS ONLY. The offline EXE boot harness installs these to catch
// guest process-exit calls (ExitProcess / TerminateProcess) and fatal guest
// exceptions (RaiseException) by longjmp-ing back into the harness. The
// console host NEVER installs them: there ExitProcess really ends the
// process (UWP semantics unchanged). NULL (default) on every host build.
extern void (*trt_boot_exit_trap)(int code);   // ExitProcess/TerminateProcess
extern void (*trt_boot_raise_trap)(u32 code);  // RaiseException (no SEH walk)

// TEST-HARNESS ONLY, spawn model: the offline EXE boot harness installs
// this so CreateProcessW becomes a REAL forked child (PyInstaller's onefile
// parent must re-spawn its own exe). The hook forks, re-boots the inherited
// image in the child, waits, records the exit code, and answers through
// pid_out (>=0 success). NULL (default) on every host build — there the
// documented console behavior holds: ERROR_ACCESS_DENIED, fail named.
extern void (*trt_boot_spawn_hook)(const char* exe_path, const char* cmdline,
                                   int* pid_out);
extern int trt_child_pid;         // >=0 once a modeled child exists
extern int trt_child_running;     // 1 while the modeled child is alive
extern u32 trt_child_exit_code;   // valid once not running
// how many messages the builtin user32 pump has actually dispatched
// (removed from the ring) — the boot report's "N messages dispatched"
unsigned trt_msg_dispatched(void);

// Dispatch a host hardware fault (Linux SIGSEGV/SIGBUS/SIGILL/SIGFPE/SIGTRAP
// or Windows EXCEPTION_POINTERS*) to registered guest VectoredExceptionHandlers
// (and SEH .pdata handlers on Linux). Returns 1 if execution should resume at the
// updated machine context; 0 if unhandled.
int trt_dispatch_hw_fault(int sig, void* siginfo, void* ucontext);
int TRT_STDCALL trt_dispatch_win32_exception(void* ep);

// runtime Steam AppID override (read from steam_appid.txt when launching)
void trt_steam_set_appid(u32 app_id);

// ---- Steam account + library (real, from the user's own installation) ----
// Set by the host after a Steam OpenID sign-in (see steam_auth.h). Passing
// null/empty means "nobody signed in". A SteamID is validated before use.
void trt_steam_set_identity(const char* steamid64, const char* persona);
void trt_steam_clear_identity(void);
u64 trt_steam_current_id(void);

// The AppIDs the user's own steamapps/appmanifest_*.acf files name.
void trt_steam_set_owned_appids(const u32* appids, int count);
int trt_steam_is_owned(u32 app_id);
int trt_steam_owned_app_count(void);

// One call for boot: load the persisted identity AND scan the local Steam
// library, then install both into the bridge. Returns the app count found.
int trt_steam_load_account(const char* state_dir);

// .218 DIAG: gap API-call trace (Sekiro silent death) + Ultrapool
// main-thread trace after frame=1. Implemented in bridge_kernel32.cpp.
//   gap_trace_record(name) -- record one bridged call (name is a static
//     literal) into the last-32 ring; consecutive duplicates collapse.
//   gap_trace_dump_if_changed() -- watchdog-driven (main.cpp,
//     snapshot-only): dumps the last 8 ring entries at most once per
//     second while calls arrive, plus one final dump ~500ms after the
//     last call, so the final calls before a silent death are logged.
//   gap_trace_note_frame1(tid) -- remember the thread that presented
//     frame=1 (the main-thread proxy); gap_trace_frame1_tid() reads it.
//   gap_trace_note_main_cv_wait(cv, ret) -- called from the CV wait
//     path when the frame-1 thread waits: logs the first sight of
//     each distinct CV with the guest return address (the wait-site)
//     + the main image base, and sampled re-waits with the tex-fail
//     count so far. Returns 1 when the wait was counted.
// .219 DIAG: wall-clock gap trace + guest-CPU proof (Sekiro spin),
//   syscall trap on ntdll/kernel32 wait functions, AFAFC0 wait-site
//   + tex-fail correlation (Ultrapool). Implemented in
//   bridge_kernel32.cpp (gap ring, trap) and bridge_thread.cpp
//   (SwitchToThread record); the tex-fail note is called from
//   src/host/vk_backend.hpp; the CPU times are fed from the
//   watchdog in main.cpp (snapshot-only).
//   gap_trace_set_guest_cpu_ms(guest_ms, proc_ms) -- watchdog feeds
//     the guest main-thread CPU time + whole-process CPU time (ms)
//     each tick; the next gap dump prints them, so advancing CPU
//     with zero new bridged calls proves spinning (vs dead/blocked).
//   gap_trace_note_tex_fail() -- count one CreateTexture2D failure;
//     returns the running total (for the correlation line).
//   gap_trace_tex_fails() / gap_trace_main_cv_waits() -- counters
//     for the tex-fail correlation lines.
//   trt_syscall_trap_install() -- INT3+VEH trap on the 6 most likely
//     ntdll/kernel32 wait functions (armed lazily when the gap goes
//     quiet for 2s of wall time; guest callers only, host callers
//     restore the byte and re-arm on the next watchdog tick).
typedef struct trt_launch_api_entry {
  const char* name;  // static runtime API name
  u32 repeats;
} trt_launch_api_entry;
void gap_trace_begin_launch(void);
typedef struct trt_sync_launch_stats {
  u32 wait_completed, wait_timed_out, wait_failed;
  u32 event_signaled, event_failed;
} trt_sync_launch_stats;
typedef void (*trt_guest_worker_fault_fn)(u32 code);
void trt_set_guest_worker_fault_reporter(trt_guest_worker_fault_fn reporter);
void trt_sync_reset_launch_stats(void);
void trt_sync_get_launch_stats(trt_sync_launch_stats* stats);
u32 gap_trace_launch_tail(trt_launch_api_entry* entries, u32 capacity, u64* calls);
void gap_trace_record(const char* name);
void gap_trace_dump_if_changed(void);
void gap_trace_note_frame1(u32 tid);
u32 gap_trace_frame1_tid(void);
int gap_trace_note_main_cv_wait(void* cv, void* ret);
void gap_trace_set_guest_cpu_ms(u64 guest_ms, u64 proc_ms);
u64 gap_trace_note_tex_fail(void);
u64 gap_trace_tex_fails(void);
u64 gap_trace_main_cv_waits(void);
void trt_syscall_trap_install(void);

// Memory budget governor surface (bridge_membudget.cpp)
u64 trt_mem_budget_bytes(void);
void trt_mem_budget_set_mb(u32 mb);
u32 trt_mem_budget_get_mb(void);
u64 trt_mem_budget_committed(void);
u64 trt_mem_budget_mapped(void);
u64 trt_mem_budget_peak(void);
u64 trt_mem_budget_refusals(void);
u64 trt_mem_budget_va_bytes(void);
u64 trt_mem_budget_reserved(void);
u64 trt_mem_budget_va_peak(void);
u64 trt_mem_budget_va_refusals(void);
int trt_mem_budget_charge_commit(u64 bytes);
void trt_mem_budget_discharge_commit(u64 bytes);
int trt_mem_budget_charge_map(u64 bytes);
void trt_mem_budget_discharge_map(u64 bytes);
void trt_mem_budget_report(char* line, unsigned cap);
void trt_mem_budget_reset(void);

// ESRAM budget-ledger surface (bridge_esram.cpp; accounting only)
void trt_esram_init(u32 size_mb);
int trt_esram_is_active(void);
int trt_esram_should_place(u32 bind_flags, u64 byte_size);
int trt_esram_alloc(u64 bytes, const char* debug_name);
void trt_esram_free(u64 bytes);
int trt_esram_get_stats(u64* out_used_bytes, u64* out_total_bytes, u64* out_alloc_count);
void trt_esram_reset(void);

#ifdef __cplusplus
}
#endif

#endif // TIRAMISU_TRT_H
