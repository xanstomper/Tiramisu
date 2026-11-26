// tiramisu/src/trt/pe.h
// Self-contained PE32/PE64 structures and loader error codes.
// Deliberately windows.h-free so the loader compiles and tests on any host.
#ifndef TIRAMISU_PE_H
#define TIRAMISU_PE_H

#include <stddef.h>
#include <stdint.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int64_t  i64;
typedef int16_t  i16;
typedef int32_t  i32;

// __stdcall exists only on Windows targets. STAGE 6 correction: these
// builtin bridge functions are the ADDRESSES the runtime resolver hands to
// real guest code (images.cpp trt_resolve_full -> IAT), so on a Linux test
// host they must carry the Win64 (ms_abi) convention — a SysV body called
// through a guest IAT receives its arguments in the wrong registers and
// reads garbage pointers (discovered live: baselib.dll's DllMain called
// LoadLibraryW and the "path" arrived in rcx, which SysV code never reads).
// Win64↔SysV forwarding is the trampolines' job only for SYNTHETIC test
// guests; runtime bridge surfaces are called by the guest directly.
//
// ABI RULE (STAGE 6b, regression-proof): every DECLARATION of a
// TRT_STDCALL function MUST itself be TRT_STDCALL — a SysV call into an
// ms_abi body reads garbage args (measured live under gdb:
// bridge_thread.cpp declared trt_Sleep/trt_GetTickCount64 plain, the
// internal call passed 1 in %edi, the ms_abi body read %rcx = garbage,
// and Sleep(1) slept 41.8 days; ms_abi callees also write 32 bytes of
// shadow space over a SysV caller's frame locals). Prefer declaring the
// function ONCE in trt.h / a bridge header and including it everywhere.
#ifndef TRT_STDCALL
#  if defined(_WIN32)
#    define TRT_STDCALL __stdcall
#  elif defined(__x86_64__) && defined(__clang__)
#    define TRT_STDCALL __attribute__((ms_abi))
#  else
#    define TRT_STDCALL
#  endif
#endif

// ---- error codes (fail-visible: every failure names its stage) ----
enum {
  TRT_OK = 0,
  TRT_ERR_BAD_DOS = 1,        // not MZ / e_lfanew out of file
  TRT_ERR_BAD_PE = 2,         // not PE\0\0
  TRT_ERR_NOT_PE64 = 3,       // machine or optional magic mismatch
  TRT_ERR_NO_SECTIONS = 4,    // NumberOfSections == 0 or absurd
  TRT_ERR_SIZE_OVERFLOW = 5,  // SizeOfImage missing / over trt limit
  TRT_ERR_SECTION_BOUNDS = 6, // section hdr points outside file/image
  TRT_ERR_NO_BASE = 7,        // alloc failed at preferred base w/o relocs
  TRT_ERR_ALLOC = 8,          // backend alloc failed
  TRT_ERR_RELOC_BADTYPE = 9,  // unknown relocation type
  TRT_ERR_RELOC_BOUNDS = 10,  // reloc entry outside image
  TRT_ERR_IMPORT = 11,        // unresolvable import (last_error_detail)
  TRT_ERR_TLS_BOUNDS = 12,    // TLS callbacks array outside image
  TRT_ERR_SEAL = 13,          // final protect() failed
  TRT_ERR_DELAY_IMPORT = 14,  // delay-load dir present (unprocessed in v0)
  TRT_ERR_BAD_ARGS = 15,
  TRT_ERR_MANAGED = 16,  // .NET assembly (COM descriptor present); no CLR here
  TRT_ERR_MONO = 17,          // Mono embedding failure (see trt_mono_last_error)
  TRT_ERR_FNA3D = 18,         // FNA3D host-device handoff failure (see trt_fna3d_last_error)
  TRT_ERR_PE32_EXECUTION = 19 // interpreter stopped before normal completion
};

// min/max PE32/PE64 header offsets/sizes we rely on (SDK-free constants)
#define TRT_PE_MACHINE_X64 0x8664u
#define TRT_PE_MACHINE_I386 0x014cu
#define TRT_PE_OPT64_MAGIC 0x020bu
#define TRT_PE_OPT32_MAGIC 0x010bu
#define TRT_PE_MAX_IMAGE_BYTES (2ull * 1024ull * 1024ull * 1024ull) // 2 GB cap

// ---- M7 Phase 1: 32-bit trap addresses ----
// A 32-bit IAT slot cannot hold a 64-bit pointer, so a bound 32-bit import
// gets a reserved guest-VA trap address instead of a host address. The
// range 0xF0000000..0xF003FFFF (256 KiB = 65536 slots x 4 bytes) sits in
// the guest kernel-reserved top of the 32-bit VA space: no image ever maps
// there (guest images live below 0x70000000), so a jump/call to a trap
// address faults visibly until the Phase 2 emulator installs its trap
// dispatcher. Trap N = BASE + 4*N; N indexes the binding registry in
// pe_loader.cpp (module!symbol recorded at bind time). Phase 1 is
// LOAD-ONLY: the traps are not callable, by design.
#define TRT_PE32_TRAP_BASE 0xF0000000u
#define TRT_PE32_TRAP_MAX  65536u
#define TRT_PE32_TRAP_END  (TRT_PE32_TRAP_BASE + 4u * TRT_PE32_TRAP_MAX)

// Data-directory array offset within the optional header differs by width:
// the PE32 optional header is 96 bytes before the directories, PE32+ is
// 112 (PE32 has BaseOfData + a 32-bit ImageBase where PE32+ has a 64-bit
// ImageBase; the layouts converge again at SizeOfImage). Any code that
// reads directories[] from a file whose width is not known statically MUST
// use trt_pe_directories() (defined below, after trt_data_directory),
// never the 64-bit struct layout.
#define TRT_PE32_OPT_DIRS_OFF 96
#define TRT_PE64_OPT_DIRS_OFF 112

#pragma pack(push, 1)

typedef struct trt_dos_header {
  u16 e_magic;      // 'MZ' = 0x5A4D
  u16 e_cblp; u16 e_cp; u16 e_crlc; u16 e_cparhdr; u16 e_minalloc;
  u16 e_maxalloc; u16 e_ss; u16 e_sp; u16 e_csum; u16 e_ip; u16 e_cs;
  u16 e_lfarlc; u16 e_ovno; u16 e_res[4]; u16 e_oemid; u16 e_oeminfo;
  u16 e_res2[10];
  u32 e_lfanew;     // offset of NT headers
} trt_dos_header;

typedef struct trt_file_header {
  u16 machine;            // 0x8664
  u16 number_of_sections;
  u32 time_date_stamp;
  u32 pointer_to_symbol_table;
  u32 number_of_symbols;
  u16 size_of_optional_header;   // +16 (IMAGE_FILE_HEADER is 20 bytes)
  u16 characteristics;
} trt_file_header;

typedef struct trt_data_directory {
  u32 virtual_address;    // RVA
  u32 size;
} trt_data_directory;

#define TRT_PE_DIR_EXPORT 0
#define TRT_PE_DIR_IMPORT 1
#define TRT_PE_DIR_EXCEPTION 3
#define TRT_PE_DIR_RELOC  5
#define TRT_PE_DIR_TLS    9
#define TRT_PE_DIR_DELAY_IMPORT 13
#define TRT_PE_DIR_COM_DESCRIPTOR 14  // .NET CLR header (managed assemblies)
#define TRT_PE_NUM_DIRECTORIES 16

// Width-aware data-directory accessor: `optional_header` points at the
// optional header (NT headers + 24); `magic` selects the layout (0x10b =
// PE32, dirs at +96; 0x20b = PE32+, dirs at +112). Anything else is
// treated as PE32+ (callers validate the magic first).
static inline const trt_data_directory* trt_pe_directories(
    const void* optional_header, u16 magic) {
  const u8* base = (const u8*)optional_header;
  if (magic == TRT_PE_OPT32_MAGIC)
    return (const trt_data_directory*)(base + TRT_PE32_OPT_DIRS_OFF);
  return (const trt_data_directory*)(base + TRT_PE64_OPT_DIRS_OFF);
}

typedef struct trt_optional_header64 {
  u16 magic;                 // 0x020b
  u8  major_linker; u8 minor_linker;
  u32 size_of_code; u32 size_of_initialized_data; u32 size_of_uninitialized_data;
  u32 address_of_entry_point;      // RVA
  u32 base_of_code;
  u64 image_base;                  // preferred load address
  u32 section_alignment; u32 file_alignment;
  u16 os_major; u16 os_minor; u16 image_major; u16 image_minor;
  u16 subsys_major; u16 subsys_minor; u32 win32_version;
  u32 size_of_image; u32 size_of_headers;
  u32 check_sum; u16 subsystem; u16 dll_characteristics;
  u64 size_of_stack_reserve; u64 size_of_stack_commit;
  u64 size_of_heap_reserve; u64 size_of_heap_commit;
  u32 loader_flags; u32 number_of_rva_and_sizes;
  trt_data_directory directories[TRT_PE_NUM_DIRECTORIES];
} trt_optional_header64;

typedef struct trt_nt_headers64 {
  u32 signature;              // 'PE\0\0'
  trt_file_header file_header;
  trt_optional_header64 optional_header;
} trt_nt_headers64;

// ---- PE32 (32-bit) optional header: ImageBase and the stack/heap sizes
// are u32, and BaseOfData exists (PE32+ drops it). Total 224 bytes with
// the 16 data directories; the directories start at offset 96.
typedef struct trt_optional_header32 {
  u16 magic;                 // 0x010b
  u8  major_linker; u8 minor_linker;
  u32 size_of_code; u32 size_of_initialized_data; u32 size_of_uninitialized_data;
  u32 address_of_entry_point;      // RVA
  u32 base_of_code;
  u32 base_of_data;                // PE32 only
  u32 image_base;                  // preferred load address (guest VA)
  u32 section_alignment; u32 file_alignment;
  u16 os_major; u16 os_minor; u16 image_major; u16 image_minor;
  u16 subsys_major; u16 subsys_minor; u32 win32_version;
  u32 size_of_image; u32 size_of_headers;
  u32 check_sum; u16 subsystem; u16 dll_characteristics;
  u32 size_of_stack_reserve; u32 size_of_stack_commit;
  u32 size_of_heap_reserve; u32 size_of_heap_commit;
  u32 loader_flags; u32 number_of_rva_and_sizes;
  trt_data_directory directories[TRT_PE_NUM_DIRECTORIES];
} trt_optional_header32;

typedef struct trt_nt_headers32 {
  u32 signature;              // 'PE\0\0'
  trt_file_header file_header;
  trt_optional_header32 optional_header;
} trt_nt_headers32;

typedef struct trt_section_header {
  u8  name[8];
  u32 virtual_size;         // in-memory size (>= raw for BSS)
  u32 virtual_address;      // RVA
  u32 size_of_raw_data;     // in-file size
  u32 pointer_to_raw_data;  // in-file offset
  u32 pointer_to_relocations; u32 pointer_to_linenumbers;
  u16 number_of_relocations; u16 number_of_linenumbers;
  u32 characteristics;
} trt_section_header;

// section characteristics bits we care about
#define TRT_SCN_CNT_CODE   0x00000020u
#define TRT_SCN_CNT_DATA   0x00000040u
#define TRT_SCN_CNT_BSS    0x00000080u
#define TRT_SCN_MEM_EXEC   0x20000000u
#define TRT_SCN_MEM_READ   0x40000000u
#define TRT_SCN_MEM_WRITE  0x80000000u

// ---- base relocations ----
typedef struct trt_reloc_block {
  u32 page_rva;     // RVA of page these entries apply to
  u32 block_size;   // total bytes incl. header
  // u16 entries follow: type<<12 | offset_in_page
} trt_reloc_block;

#define TRT_RELOC_ABSOLUTE 0u
#define TRT_RELOC_HIGHLOW  3u
#define TRT_RELOC_DIR64    10u

// ---- imports ----
typedef struct trt_import_descriptor {
  u32 original_first_thunk; // RVA of INT (hint/name table)
  u32 time_date_stamp;
  u32 forwarder_chain;
  u32 name;                 // RVA of module name (ASCIIZ)
  u32 first_thunk;          // RVA of IAT
} trt_import_descriptor;

// ---- UNIVERSAL engine: auto import-surface extraction ----
struct trt_universal_profile;  // defined below: the import-surface feed
// Enumerate every import (direct + delay-load) from a PE file. Fills `up`
// (bounded by up_cap). Returns the number of entries written, or -1 if the
// file is not a readable PE. This is the "pull APIs to run any PC game"
// primitive: dump the game's full import surface.
#ifdef __cplusplus
extern "C" {
#endif
int trt_pe_enumerate_imports(const char* path, trt_universal_profile* up,
                             uint32_t up_cap);
#ifdef __cplusplus
}
#endif

// ---- delay imports (delay-load directory) ----
// Modern linkers set attributes bit 0 ("RVA based"): every field below is an
// RVA. Legacy images use VAs instead - the loader handles both.
typedef struct trt_delayload_descriptor {
  u32 attributes;
  u32 name_rva;         // RVA (or VA) of the DLL name
  u32 module_handle_rva;// RVA (or VA) of the HMODULE slot
  u32 iat_rva;          // RVA (or VA) of the delay-load IAT
  u32 int_rva;          // RVA (or VA) of the delay-load INT
  u32 bound_iat_rva;
  u32 unload_iat_rva;
  u32 time_stamp;
} trt_delayload_descriptor;

// thunk value: bit63 set => import by ordinal (low 16 bits), else RVA of
// IMAGE_IMPORT_BY_NAME { u16 hint; char name[]; }

// ---- UNIVERSAL engine: import surface enumeration (Phase 2) ----
// One entry per importable symbol. This is the "pull APIs" feed for the
// universal engine — dump every import of a PE (direct + delay) as a stable
// list, so the universal guest can service/re-synthesize them dynamically.
struct trt_universal_import {
  char module[256];         // lower-case, NUL-terminated
  char symbol[256];         // lower-case, NUL-terminated; empty for ordinal-only
  uint16_t ordinal;         // for ordinal-only imports
  u32 symbol_offset;        // file offset of symbol name (0 if none)
  uint32_t name_len;        // length of module/symbol names used for sizing
};
struct trt_universal_profile {
  u16 machine;              // TRT_PE_MACHINE_X64 / I386
  u16 subsystem;
  bool is_managed;
  trt_universal_import imports[1024];
  uint32_t import_count;
};

// ---- TLS ----
typedef struct trt_tls_directory64 {
  u64 start_address_of_raw_data;   // VAs (relocated at load)
  u64 end_address_of_raw_data;
  u64 address_of_index;
  u64 address_of_callbacks;        // VA of NULL-terminated callback ptr array
  u32 size_of_zero_fill; u32 characteristics;
} trt_tls_directory64;

// PE32 TLS directory: same fields, 32-bit VAs; the callback array holds
// 4-byte VAs. The VAs are GUEST VAs (relocated by the 32-bit delta).
typedef struct trt_tls_directory32 {
  u32 start_address_of_raw_data;
  u32 end_address_of_raw_data;
  u32 address_of_index;
  u32 address_of_callbacks;        // VA of NULL-terminated u32 callback array
  u32 size_of_zero_fill; u32 characteristics;
} trt_tls_directory32;

#pragma pack(pop)

#endif // TIRAMISU_PE_H
