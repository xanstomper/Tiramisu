// tiramisu/src/trt/trt.h
// Tiramisu Runtime (trt) public API — Wine-style in-process PE64 loader
// for UWP hosts (Xbox Dev Mode) with a pluggable memory backend and
// import resolver so the same core runs in offline tests on Linux.
#ifndef TIRAMISU_TRT_H
#define TIRAMISU_TRT_H

#include "pe.h"

#ifdef __cplusplus
extern "C" {
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
#define TRT_MAX_TLS_CALLBACKS 64

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
void  trt_guest_tls_bind_host_slot(void** host_tls, void** guest_tls);

// Map a PE64 from a file image. Read-only view of [file, file+file_size).
// Stages: validate -> alloc -> headers+sections -> relocs -> imports -> TLS.
// Pages stay RW until trt_seal_image().
int trt_map_pe(const trt_backend* be, const trt_resolver* rs,
                const u8* file, size_t file_size, trt_image** out);

// Flip sections to final protections (code -> RX). Call once after
// relocation/import/TLS writes are complete. Returns TRT_OK or TRT_ERR_SEAL.
int trt_seal_image(const trt_backend* be, trt_image* img);

// ---- boot audit (offline gate for REAL games) ----------------------------
// Parses a PE64's import + delay-import tables IN FILE SPACE (no mapping,
// no execution, the file bytes are never modified) and pushes every
// module!symbol through the REAL resolver — the same code path trt_map_pe
// uses to bind imports. On return *missing lists every "module!symbol"
// that failed to resolve, in table order.
// Returns TRT_OK when the audit ran (check *missing to decide bootability);
// TRT_ERR_* for malformed PEs / usage errors.
int trt_audit_imports(const u8* file, size_t file_size,
                      void* missing_out /* std::vector<std::string>* */);

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

typedef struct trt_named_fn {
  const char* name;
  void* fn;
} trt_named_fn;

void trt_module_register(const char* name, trt_route route,
                          const trt_named_fn* fns, int fn_count);
trt_route trt_module_route(const char* name);
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
int trt_bridge_register_dbghelp(void);
int trt_bridge_register_dstorage(void);
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

#ifdef __cplusplus
}
#endif

#endif // TIRAMISU_TRT_H
