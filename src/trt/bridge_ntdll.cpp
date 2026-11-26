// tiramisu/src/trt/bridge_ntdll.cpp
// Minimal honest ntdll.dll surface: Rtl unwind/status/version, file query/write,
// and ETW provider no-ops. api-ms-win-eventing-provider-* and
// api-ms-win-core-rtlsupport-* route here via apiset.cpp.
#include "bridge_ntdll.h"
#include "thunk.h"

#include <mutex>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <thread>
#include <atomic>  // .242 DIAG: NTALLOC counters
#include <unordered_set>

extern "C" {

// Reuse existing Rtl/memory implementations from bridge_kernel32 / bridge_mem.
void* TRT_STDCALL trt_RtlCaptureContext(void* ctx);
void* TRT_STDCALL trt_RtlLookupFunctionEntry(u64 ctrlpc, u64* base_out,
                                             void* history);
u64 TRT_STDCALL trt_RtlVirtualUnwind(u32 handler_type, u64 image_base,
                                     u64 ctrl_pc, void* fn_entry, void* ctx,
                                     void** handler_data, u64* establisher,
                                     void* ctx_ptrs);
void* TRT_STDCALL trt_RtlPcToFileHeader(u64 pc, u64* base_out);
u8 TRT_STDCALL trt_RtlAddFunctionTable(void* fn_table, u32 entry_count,
                                       u64 base_addr);
u8 TRT_STDCALL trt_RtlDeleteFunctionTable(void* fn_table);
void TRT_STDCALL trt_RtlUnwindEx(u64 target, u64 target_ip, void* exrec,
                                 u64 retval, void* ctx, void* history);
void TRT_STDCALL trt_RtlUnwind(u64 target, u64 target_ip, void* exrec,
                               u64 retval);
void* TRT_STDCALL trt_VirtualAlloc2(void* proc, void* base_addr, u64 size,
                                    u32 type, u32 prot, void* ext_params,
                                    u32 param_count);
int TRT_STDCALL trt_VirtualFree(void* addr, u64 bytes, u32 type);
int TRT_STDCALL trt_VirtualProtect(void* addr, u64 bytes, u32 prot, u32* old_prot);
u64 TRT_STDCALL trt_VirtualQuery(const void* addr, void* mbi_out, u64 len);
u32 TRT_STDCALL trt_GetLastError(void);
void TRT_STDCALL trt_SetLastError(u32 e);
int TRT_STDCALL trt_MultiByteToWideChar(u32 cp, u32 flags, const char* src,
                                        i32 src_len, u16* dst, i32 dst_len);
int TRT_STDCALL trt_WideCharToMultiByte(u32 cp, u32 flags, const u16* src,
                                        i32 src_len, char* dst, i32 dst_len,
                                        const char* defchar, void* used_def);

FILE* trt_file_os(void* h);
const char* trt_file_vpath(void* h);
int TRT_STDCALL trt_CloseHandle(void* h);
int TRT_STDCALL trt_VerifyVersionInfoW(void* vi, u32 type_mask, u64 cond_mask);

// Cross-bridge sync/exit primitives (defined in bridge_thread.cpp /
// bridge_kernel32.cpp). The Nt* wrappers route onto exactly these so guest
// events and waits are the SAME objects kernel32 hands out.
u64 TRT_STDCALL trt_CreateEventW(void* attrs, bool manual, bool initial,
                                  const u16* name);
int TRT_STDCALL trt_SetEvent(u64 handle);
int TRT_STDCALL trt_ResetEvent(u64 handle);
u32 TRT_STDCALL trt_WaitForSingleObjectEx(u64 handle, u32 ms, int alertable);
void TRT_STDCALL trt_ExitProcess(u32 code);
int TRT_STDCALL trt_TerminateProcess(void* proc, u32 code);
void* TRT_STDCALL trt_CreateFileW(const u16* name, u32 access, u32 share,
                                  void* sa, u32 disposition, u32 flags,
                                  void* tmpl);

namespace {

constexpr u32 k_status_ok = 0x00000000u;
constexpr u32 k_status_not_implemented = 0xC0000002u;
constexpr u32 k_status_invalid_handle = 0xC0000008u;
constexpr u32 k_status_invalid_parameter = 0xC000000Du;
constexpr u32 k_status_end_of_file = 0xC0000011u;
constexpr u32 k_status_timeout = 0x00000102u;
constexpr u32 k_status_object_name_not_found = 0xC000000Fu;
constexpr u32 k_status_object_path_not_found = 0xC000003Au;
constexpr u32 k_status_object_name_invalid = 0xC0000033u;
constexpr u32 k_status_name_collision = 0xC0000035u;
constexpr u32 k_status_sharing_violation = 0xC0000068u;
constexpr u32 k_status_write_protect = 0xC00000A2u;
constexpr u32 k_status_buffer_overflow = 0x80000005u;

u32 TRT_GUEST_ABI nt_RtlNtStatusToDosError(u32 status) {
  switch (status) {
    case 0x00000000u: return 0;     // ERROR_SUCCESS
    case 0xC0000002u: return 50;    // ERROR_NOT_SUPPORTED
    case 0xC0000008u: return 6;     // ERROR_INVALID_HANDLE
    case 0xC000000Du: return 87;    // ERROR_INVALID_PARAMETER
    case 0xC000000Fu: return 2;     // ERROR_FILE_NOT_FOUND
    case 0xC0000017u: return 8;     // ERROR_NOT_ENOUGH_MEMORY
    case 0xC0000022u: return 5;     // ERROR_ACCESS_DENIED
    case 0xC0000034u: return 2;     // ERROR_FILE_NOT_FOUND
    case 0xC0000035u: return 80;    // ERROR_FILE_EXISTS
    case 0xC000009Au: return 8;     // ERROR_NOT_ENOUGH_MEMORY
    case 0xC00000BAu: return 5;     // ERROR_ACCESS_DENIED (is directory)
    case 0xC00000BBu: return 50;    // ERROR_NOT_SUPPORTED
    case 0x80000005u: return 234;   // ERROR_MORE_DATA
    case 0x80000006u: return 18;    // ERROR_NO_MORE_FILES
    case 0x8000001Au: return 259;   // ERROR_NO_MORE_ITEMS
    default:
      if ((status & 0xFFFF0000u) == 0x80070000u) return status & 0xFFFFu;
      return 317;                   // ERROR_MR_MID_NOT_FOUND
  }
}

u32 TRT_GUEST_ABI nt_RtlGetVersion(void* lpvi) {
  if (!lpvi) return k_status_invalid_parameter;
  u32* d = (u32*)lpvi;
  const u32 sz = d[0];
  if (sz < 20) return k_status_invalid_parameter;
  memset(d + 1, 0, sz - 4);
  d[1] = 10;     // dwMajorVersion
  d[2] = 0;      // dwMinorVersion
  d[3] = 19045;  // dwBuildNumber (Win10 / Xbox Series shell era)
  d[4] = 2;      // VER_PLATFORM_WIN32_NT
  if (sz >= 284) {
    u8* b = (u8*)lpvi;
    b[282] = 1;  // wProductType = VER_NT_WORKSTATION
  }
  return k_status_ok;
}

u32 TRT_GUEST_ABI nt_RtlVerifyVersionInfo(void* vi, u32 type_mask, u64 cond_mask) {
  if (!vi) return k_status_invalid_parameter;
  return trt_VerifyVersionInfoW(vi, type_mask, cond_mask) ? k_status_ok
                                                          : 0xC0000059u;  // STATUS_REVISION_MISMATCH
}

u32 TRT_GUEST_ABI nt_NtClose(void* h) {
  if (!h || h == (void*)(uintptr_t)-1) return k_status_invalid_handle;
  return trt_CloseHandle(h) ? k_status_ok : k_status_invalid_handle;
}

struct io_status_block {
  u64 status;
  u64 info;
};

u32 TRT_GUEST_ABI nt_NtQueryInformationFile(void* h, void* iosb_ptr,
                                            void* info, u32 len, u32 cls) {
  FILE* os = trt_file_os(h);
  io_status_block* iosb = (io_status_block*)iosb_ptr;
  if (!os) {
    if (iosb) { iosb->status = k_status_invalid_handle; iosb->info = 0; }
    return k_status_invalid_handle;
  }
  if (!info) {
    if (iosb) { iosb->status = k_status_invalid_parameter; iosb->info = 0; }
    return k_status_invalid_parameter;
  }
  if (cls == 5 /* FileStandardInformation */) {
    if (len < 24) return k_status_invalid_parameter;
    const long pos = ftell(os);
    fseek(os, 0, SEEK_END);
    const i64 sz = (i64)ftell(os);
    fseek(os, pos, SEEK_SET);
    struct file_std_info {
      i64 alloc_size;
      i64 eof;
      u32 links;
      u8  delete_pending;
      u8  directory;
      u16 pad;
    };
    file_std_info* fsi = (file_std_info*)info;
    memset(fsi, 0, sizeof(*fsi));
    fsi->alloc_size = sz;
    fsi->eof = sz;
    fsi->links = 1;
    if (iosb) { iosb->status = k_status_ok; iosb->info = sizeof(*fsi); }
    return k_status_ok;
  }
  if (cls == 14 /* FilePositionInformation */) {
    if (len < 8) return k_status_invalid_parameter;
    *(i64*)info = (i64)ftell(os);
    if (iosb) { iosb->status = k_status_ok; iosb->info = 8; }
    return k_status_ok;
  }
  if (cls == 9 /* FileNameInformation */) {
    if (len < 6) return k_status_invalid_parameter;
    const char* vp = trt_file_vpath(h);
    const char* rel = (vp && vp[0] && vp[1] == ':') ? vp + 2 : (vp ? vp : "\\");
    const u32 nchars = (u32)strlen(rel);
    const u32 bytes_needed = nchars * 2;
    *(u32*)info = bytes_needed;
    u16* dst = (u16*)((u8*)info + 4);
    const u32 max_chars = (len - 4) / 2;
    const u32 copy_chars = nchars < max_chars ? nchars : max_chars;
    for (u32 i = 0; i < copy_chars; ++i) dst[i] = (u16)(u8)rel[i];
    const u32 written = 4 + copy_chars * 2;
    const u32 st = (copy_chars < nchars) ? k_status_buffer_overflow : k_status_ok;
    if (iosb) { iosb->status = st; iosb->info = written; }
    return st;
  }
  char line[144];
  snprintf(line, sizeof(line),
           "ntdll: NtQueryInformationFile(cls=%u) -> STATUS_NOT_IMPLEMENTED",
           cls);
  trt_core_log(line);
  if (iosb) { iosb->status = k_status_not_implemented; iosb->info = 0; }
  return k_status_not_implemented;
}

u32 TRT_GUEST_ABI nt_NtWriteFile(void* h, void* ev, void* apc, void* apc_ctx,
                                 void* iosb_ptr, const void* buf, u32 len,
                                 const i64* byte_offset, void* key) {
  (void)ev; (void)apc; (void)apc_ctx; (void)key;
  io_status_block* iosb = (io_status_block*)iosb_ptr;
  const intptr_t hv = (intptr_t)h;
  if (hv == -10 || hv == -11 || hv == -12) {
    if (buf && len) {
      extern void console_log_bytes(const char* buf, u32 bytes);
      console_log_bytes((const char*)buf, len);
    }
    if (iosb) { iosb->status = k_status_ok; iosb->info = len; }
    return k_status_ok;
  }
  FILE* os = trt_file_os(h);
  if (!os) {
    if (iosb) { iosb->status = k_status_invalid_handle; iosb->info = 0; }
    return k_status_invalid_handle;
  }
  if (byte_offset && *byte_offset >= 0) {
    fseek(os, (long)*byte_offset, SEEK_SET);
  }
  const size_t wrote = (buf && len) ? fwrite(buf, 1, len, os) : 0;
  if (iosb) { iosb->status = k_status_ok; iosb->info = (u64)wrote; }
  return k_status_ok;
}

// ETW: console apps have no kernel ETW trace session; register returns a valid
// nonzero token and Write verbs succeed as no-ops; EventEnabled -> 0.
u32 TRT_GUEST_ABI nt_EtwEventRegister(const void* provider_id, void* callback,
                                      void* callback_ctx, u64* reg_handle) {
  (void)provider_id; (void)callback; (void)callback_ctx;
  if (reg_handle) *reg_handle = 0x45545701ull;
  return 0;
}
u32 TRT_GUEST_ABI nt_EtwEventUnregister(u64 reg_handle) {
  (void)reg_handle;
  return 0;
}
u32 TRT_GUEST_ABI nt_EtwEventWrite(u64 reg, const void* desc, u32 count,
                                   void* data) {
  (void)reg; (void)desc; (void)count; (void)data;
  return 0;
}
u32 TRT_GUEST_ABI nt_EtwEventWriteEx(u64 reg, const void* desc, u64 filter,
                                     u32 flags, const void* act,
                                     const void* rel_act, u32 count, void* data) {
  (void)reg; (void)desc; (void)filter; (void)flags;
  (void)act; (void)rel_act; (void)count; (void)data;
  return 0;
}
u32 TRT_GUEST_ABI nt_EtwEventWriteTransfer(u64 reg, const void* desc,
                                           const void* act, const void* rel,
                                           u32 count, void* data) {
  (void)reg; (void)desc; (void)act; (void)rel; (void)count; (void)data;
  return 0;
}
u32 TRT_GUEST_ABI nt_EtwEventWriteString(u64 reg, u8 level, u64 kw,
                                         const u16* str) {
  (void)reg; (void)level; (void)kw; (void)str;
  return 0;
}
u32 TRT_GUEST_ABI nt_EtwEventSetInformation(u64 reg, u32 cls, const void* info,
                                            u32 len) {
  (void)reg; (void)cls; (void)info; (void)len;
  return 0;
}
u8 TRT_GUEST_ABI nt_EtwEventEnabled(u64 reg, const void* desc) {
  (void)reg; (void)desc;
  return 0;  // tracing disabled
}
u32 TRT_GUEST_ABI nt_EtwEventActivityIdControl(u32 ctrl, void* guid) {
  if (!guid) return 87;  // ERROR_INVALID_PARAMETER
  if (ctrl == 1 /* GET */ || ctrl == 3 /* CREATE */ || ctrl == 4 /* GET_SET */) {
    memset(guid, 0, 16);
    ((u8*)guid)[0] = 0x54;  // 'T'
  }
  return 0;
}

u32 dos_to_ntstatus(u32 e) {
  switch (e) {
    case 0: return 0x00000000u;
    case 2: return k_status_object_name_not_found;
    case 3: return k_status_object_path_not_found;
    case 5: return 0xC0000022u;    // STATUS_ACCESS_DENIED
    case 6: return k_status_invalid_handle;
    case 8: return 0xC0000017u;    // STATUS_NO_MEMORY
    case 19: return k_status_write_protect;
    case 32: return k_status_sharing_violation;
    case 80: return k_status_name_collision;
    case 87: return 0xC000000Du;   // STATUS_INVALID_PARAMETER
    case 487: return 0xC0000018u;  // STATUS_CONFLICTING_ADDRESSES
    default: return 0xC0000001u;   // STATUS_UNSUCCESSFUL
  }
}

// .242 DIAG: bridged NtAllocateVirtualMemory(Ex) totals --
// nt_NtAllocateVirtualMemoryEx routes into trt_VirtualAlloc2
// -> trt_VirtualAlloc, so the .241 VALLOC-CUM 0 already
// covers this path; but VALLOC-CUM only logs per-caller at
// 256MB crossings and is snapshot-only. These two atomics
// are the committed, always-on witness: the VIDEO-SAMPLE
// line reads them every 5s, so a zero is PROVEN zero (a
// line every 5s), not absence of a line. If they ever go
// nonzero while VALLOC-CUM stays quiet, the two paths
// disagree and the ledger is wrong.
static std::atomic<unsigned long long> g_ntalloc242_calls{0};
static std::atomic<unsigned long long> g_ntalloc242_bytes{0};

u32 TRT_GUEST_ABI nt_NtAllocateVirtualMemoryEx(void* proc, void** base_ptr,
                                              u64* region_size, u32 alloc_type,
                                              u32 protect, void* ext_params,
                                              u32 param_count) {
  if (!base_ptr || !region_size || *region_size == 0) return 0xC000000Du;
  g_ntalloc242_calls.fetch_add(1, std::memory_order_relaxed);
  g_ntalloc242_bytes.fetch_add(*region_size, std::memory_order_relaxed);
  void* p = trt_VirtualAlloc2(proc, *base_ptr, *region_size, alloc_type,
                              protect, ext_params, param_count);
  if (!p) return dos_to_ntstatus(trt_GetLastError());
  *base_ptr = p;
  *region_size = (*region_size + 4095ULL) & ~4095ULL;
  return 0;
}

u32 TRT_GUEST_ABI nt_NtAllocateVirtualMemory(void* proc, void** base_ptr,
                                            u64 zero_bits, u64* region_size,
                                            u32 alloc_type, u32 protect) {
  (void)zero_bits;
  return nt_NtAllocateVirtualMemoryEx(proc, base_ptr, region_size, alloc_type,
                                      protect, nullptr, 0);
}

u32 TRT_GUEST_ABI nt_NtFreeVirtualMemory(void* proc, void** base_ptr,
                                        u64* region_size, u32 free_type) {
  (void)proc;
  if (!base_ptr || !region_size) return 0xC000000Du;
  if (!trt_VirtualFree(*base_ptr, *region_size, free_type)) {
    return dos_to_ntstatus(trt_GetLastError());
  }
  return 0;
}

u32 TRT_GUEST_ABI nt_NtProtectVirtualMemory(void* proc, void** base_ptr,
                                           u64* region_size, u32 new_prot,
                                           u32* old_prot) {
  (void)proc;
  if (!base_ptr || !region_size) return 0xC000000Du;
  u32 tmp_old = 0;
  if (!trt_VirtualProtect(*base_ptr, *region_size, new_prot,
                          old_prot ? old_prot : &tmp_old)) {
    return dos_to_ntstatus(trt_GetLastError());
  }
  return 0;
}

void TRT_GUEST_ABI nt_RtlGetDeviceFamilyInfoEnum(u64* uap_info,
                                                u32* dev_family,
                                                u32* dev_form) {
  if (uap_info) *uap_info = 0x000A00004A650000ULL;  // 10.0.19045.0
  if (dev_family) *dev_family = 3u;                 // DEVICEFAMILYINFOENUM_DESKTOP
  if (dev_form) *dev_form = 0u;                     // DEVICEFAMILYDEVICEFORM_UNKNOWN
}

struct nt_ansi_str {
  u16 length;
  u16 max_length;
  u32 pad;
  char* buffer;
};

struct nt_unicode_str {
  u16 length;
  u16 max_length;
  u32 pad;
  u16* buffer;
};

// ---- NtCreateFile / NtReadFile: the native NT file surface, routed onto the
// same VFS the kernel32 CreateFileW bridge uses (trt_CreateFileW does the
// path resolution, image materialization, copy-on-write and handle alloc —
// NtCreateFile only translates the NT-shaped arguments onto it). ----

// OBJECT_ATTRIBUTES (x64): Length, RootDirectory, ObjectName, Attributes,
// SecurityDescriptor, SecurityQualityOfService.
struct nt_object_attr {
  u32 length;
  u32 pad;
  void* root_directory;
  nt_unicode_str* object_name;
  u32 attributes;
  u32 pad2;
  void* security_descriptor;
  void* security_qos;
};

// NT CreateDisposition -> kernel32 CreateFileW disposition.
// NT: 0 SUPERSEDE, 1 OPEN, 2 CREATE, 3 OPEN_IF, 4 OVERWRITE, 5 OVERWRITE_IF
// k32: 1 CREATE_NEW, 2 CREATE_ALWAYS, 3 OPEN_EXISTING, 4 OPEN_ALWAYS,
//      5 TRUNCATE_EXISTING
u32 nt_disp_to_k32(u32 disp) {
  switch (disp) {
    case 0: return 2;  // FILE_SUPERSEDE    -> CREATE_ALWAYS
    case 1: return 3;  // FILE_OPEN        -> OPEN_EXISTING
    case 2: return 1;  // FILE_CREATE      -> CREATE_NEW
    case 3: return 4;  // FILE_OPEN_IF     -> OPEN_ALWAYS
    case 4: return 5;  // FILE_OVERWRITE   -> TRUNCATE_EXISTING
    case 5: return 2;  // FILE_OVERWRITE_IF-> CREATE_ALWAYS
    default: return 0;
  }
}

// IOSB Information for a successful create/open (FILE_SUPERSEDED=0 .. )
u32 nt_disp_opened_info(u32 k32_disp) {
  switch (k32_disp) {
    case 1: return 2;  // FILE_CREATED
    case 2: return 3;  // FILE_OVERWRITTEN
    case 5: return 3;  // FILE_OVERWRITTEN
    default: return 1; // FILE_OPENED
  }
}

u32 TRT_GUEST_ABI nt_NtCreateFile(void** file_handle, u32 desired_access,
                                  void* obj_attr, void* iosb_ptr,
                                  const i64* allocation_size,
                                  u32 file_attributes, u32 share_access,
                                  u32 create_disposition, u32 create_options,
                                  void* ea_buffer, u32 ea_length) {
  (void)allocation_size; (void)file_attributes; (void)share_access;
  (void)ea_buffer; (void)ea_length;
  io_status_block* iosb = (io_status_block*)iosb_ptr;
  if (!file_handle || !obj_attr || !iosb) return k_status_invalid_parameter;
  *file_handle = nullptr;
  iosb->status = k_status_invalid_parameter;
  iosb->info = 0;
  nt_object_attr* oa = (nt_object_attr*)obj_attr;
  nt_unicode_str* us = oa->object_name;
  if (!us || !us->buffer || us->length < 2) {
    return k_status_invalid_parameter;
  }
  // UNICODE_STRING Length is in BYTES and the buffer is not necessarily
  // null-terminated: copy + terminate into a local narrow buffer.
  const u32 chars = us->length / 2u;
  char name[520];
  if (chars >= sizeof(name)) {
    iosb->status = k_status_object_name_invalid;
    return k_status_object_name_invalid;
  }
  for (u32 i = 0; i < chars; ++i) name[i] = (char)(u8)us->buffer[i];
  name[chars] = 0;
  // NT-style prefix: \??\ (RtlDosPathNameToNtPathName form) -> strip it.
  if (name[0] == '\\' && name[1] == '?' && name[2] == '?' && name[3] == '\\') {
    u32 o = 0;
    for (u32 i = 4; name[i]; ++i) name[o++] = name[i];
    name[o] = 0;
  }
  // RootDirectory-relative open: prepend the directory handle's VFS path.
  if (oa->root_directory && name[0] != '\\' &&
      !(name[0] && name[1] == ':')) {
    const char* base = trt_file_vpath(oa->root_directory);
    if (!base) {
      iosb->status = k_status_invalid_handle;
      return k_status_invalid_handle;
    }
    char full[520];
    snprintf(full, sizeof(full), "%s\\%s", base, name);
    for (u32 i = 0; ; ++i) {
      name[i] = full[i];
      if (!full[i]) break;
    }
  }
  if (create_options & 0x1u /*FILE_DIRECTORY_FILE*/) {
    // Directory handles are not part of the VFS file surface: fail visibly.
    char line[128];
    snprintf(line, sizeof(line),
             "ntdll: NtCreateFile(%s) FILE_DIRECTORY_FILE -> "
             "STATUS_NOT_IMPLEMENTED",
             name);
    trt_core_log(line);
    iosb->status = k_status_not_implemented;
    return k_status_not_implemented;
  }
  const u32 k32_disp = nt_disp_to_k32(create_disposition);
  if (k32_disp == 0) return k_status_invalid_parameter;
  // DesiredAccess: FILE_APPEND_DATA (0x4) alone must still open for write.
  u32 access = desired_access;
  if (access & 0x4u) access |= 0x2u;  // FILE_WRITE_DATA
  u16 wname[520];
  u32 wi = 0;
  for (; name[wi]; ++wi) wname[wi] = (u16)(u8)name[wi];
  wname[wi] = 0;
  void* h = trt_CreateFileW(wname, access, share_access, nullptr, k32_disp,
                            file_attributes, nullptr);
  if (!h || h == (void*)(uintptr_t)-1) {
    const u32 st = dos_to_ntstatus(trt_GetLastError());
    char line[160];
    snprintf(line, sizeof(line), "ntdll: NtCreateFile(%s disp=%u) -> 0x%08X",
             name, create_disposition, st);
    trt_core_log(line);
    iosb->status = st;
    return st;
  }
  *file_handle = h;
  iosb->status = k_status_ok;
  iosb->info = nt_disp_opened_info(k32_disp);
  return k_status_ok;
}

u32 TRT_GUEST_ABI nt_NtReadFile(void* h, void* ev, void* apc, void* apc_ctx,
                                void* iosb_ptr, void* buf, u32 len,
                                const i64* byte_offset, void* key) {
  (void)ev; (void)apc; (void)apc_ctx; (void)key;
  io_status_block* iosb = (io_status_block*)iosb_ptr;
  const intptr_t hv = (intptr_t)h;
  if (hv == -10 || hv == -11 || hv == -12) {
    if (iosb) { iosb->status = k_status_end_of_file; iosb->info = 0; }
    return k_status_end_of_file;
  }
  FILE* os = trt_file_os(h);
  if (!os) {
    if (iosb) { iosb->status = k_status_invalid_handle; iosb->info = 0; }
    return k_status_invalid_handle;
  }
  if (!buf && len) {
    if (iosb) { iosb->status = k_status_invalid_parameter; iosb->info = 0; }
    return k_status_invalid_parameter;
  }
  if (byte_offset && *byte_offset >= 0) {
    fseek(os, (long)*byte_offset, SEEK_SET);
  }
  const size_t got = len ? fread(buf, 1, len, os) : 0;
  if (len == 0) {
    if (iosb) { iosb->status = k_status_ok; iosb->info = 0; }
    return k_status_ok;
  }
  if (got == 0) {
    // Read at end-of-file: STATUS_END_OF_FILE, zero bytes transferred.
    if (iosb) { iosb->status = k_status_end_of_file; iosb->info = 0; }
    return k_status_end_of_file;
  }
  if (iosb) { iosb->status = k_status_ok; iosb->info = (u64)got; }
  return k_status_ok;
}

// ---- Nt event surface: routed onto the SAME sync objects kernel32's
// CreateEventW/WaitForSingleObject bridge allocates (bridge_thread.cpp) —
// an event created through NtCreateEvent is signalable by kernel32 SetEvent
// and vice versa. ----

u32 TRT_GUEST_ABI nt_NtCreateEvent(void** event_handle, u32 desired_access,
                                   void* obj_attr, u32 event_type,
                                   u8 initial_state) {
  (void)desired_access;
  if (!event_handle) return k_status_invalid_parameter;
  *event_handle = nullptr;
  // EVENT_TYPE: 0 NotificationEvent (manual-reset), 1 SynchronizationEvent
  // (auto-reset). Anything else is not an event type.
  if (event_type > 1) return k_status_invalid_parameter;
  const u16* name = nullptr;
  u16 wname[256];
  const nt_object_attr* oa = (const nt_object_attr*)obj_attr;
  if (oa) {
    const nt_unicode_str* us = oa->object_name;
    if (us && us->buffer && us->length >= 2) {
      const u32 chars = us->length / 2u;
      const u32 cap = chars < 255u ? chars : 255u;
      for (u32 i = 0; i < cap; ++i) wname[i] = us->buffer[i];
      wname[cap] = 0;
      name = wname;
    }
  }
  const u64 h = trt_CreateEventW(nullptr, event_type == 0u, initial_state != 0,
                                 name);
  if (!h) return dos_to_ntstatus(trt_GetLastError());
  *event_handle = (void*)(uintptr_t)h;
  return k_status_ok;
}

u32 TRT_GUEST_ABI nt_NtSetEvent(void* h, i32* previous_state) {
  if (!h || h == (void*)(uintptr_t)-1) return k_status_invalid_handle;
  const u64 uh = (u64)(uintptr_t)h;
  // PreviousState: probe with a zero-timeout wait (non-consuming for a
  // manual-reset event; for an auto-reset event the consumed token is
  // restored by the SetEvent below, so the end state is identical).
  i32 prev = 0;
  if (trt_WaitForSingleObjectEx(uh, 0, 0) == 0) prev = 1;
  if (!trt_SetEvent(uh)) return k_status_invalid_handle;
  if (previous_state) *previous_state = prev;
  return k_status_ok;
}

u32 TRT_GUEST_ABI nt_NtWaitForSingleObject(void* h, u8 alertable,
                                           const i64* timeout) {
  if (!h || h == (void*)(uintptr_t)-1) return k_status_invalid_handle;
  u32 ms = 0xFFFFFFFFu;  // no timeout argument -> wait forever
  if (timeout) {
    const i64 v = *timeout;
    // 100-ns units, negative = relative to now. A positive value is an
    // absolute deadline; treated as relative here (games pass relative).
    u64 units = (v < 0) ? (u64)(-v) : (u64)v;
    ms = (u32)((units + 9999u) / 10000u);
  }
  const u32 r = trt_WaitForSingleObjectEx((u64)(uintptr_t)h, ms, alertable != 0);
  if (r == 0) return k_status_ok;
  if (r == 0x102u) return k_status_timeout;  // STATUS_TIMEOUT == WAIT_TIMEOUT
  return k_status_invalid_handle;
}

u32 TRT_GUEST_ABI nt_NtSignalAndWaitForSingleObject(void* signal_handle,
                                                    void* wait_handle,
                                                    u8 alertable,
                                                    const i64* timeout) {
  if (!signal_handle || signal_handle == (void*)(uintptr_t)-1) {
    return k_status_invalid_handle;
  }
  const u32 set_r = nt_NtSetEvent(signal_handle, nullptr);
  if (set_r != k_status_ok) return set_r;
  return nt_NtWaitForSingleObject(wait_handle, alertable, timeout);
}

// ---- RaiseFailFastException: the fast-fail path is HONORED. The guest
// asked to die; the same fatal path ExitProcess/TerminateProcess uses ends
// the process (the offline harness traps it via trt_boot_exit_trap). ----
u32 TRT_GUEST_ABI nt_RaiseFailFastException(void* exrec, void* ctx,
                                            u32 flags) {
  (void)exrec; (void)ctx; (void)flags;
  trt_core_log("ntdll: RaiseFailFastException -> fail-fast honored (exit)");
  // 0xC0000409 STATUS_STACK_BUFFER_OVERRUN / FAST_FAIL_FATAL_APP_EXIT;
  // trt_ExitProcess masks to the low byte for the host exit code.
  trt_ExitProcess(0x09);
  return k_status_ok;  // unreachable when the exit path is honest
}

// ---- .212 DIAG: RtlExitUserProcess / NtTerminateProcess intercepts ----
// Neither was in the ntdll bridge table, so a guest calling them (import or
// GetProcAddress) got the REAL ntdll function and died with zero logging.
// Log + route onto the honest trt_ExitProcess / trt_TerminateProcess path.
// If the .211 silent death is one of these, the log line names it.
void TRT_GUEST_ABI nt_RtlExitUserProcess(u32 code) {
  char line[128];
  snprintf(line, sizeof(line), "ntdll: RtlExitUserProcess(0x%08x) called", code);
  trt_core_log(line);
  trt_ExitProcess(code);
}
u32 TRT_GUEST_ABI nt_NtTerminateProcess(void* proc, u32 code) {
  char line[128];
  snprintf(line, sizeof(line), "ntdll: NtTerminateProcess(proc=%p code=0x%08x) called", proc, code);
  trt_core_log(line);
  if (!proc || proc == (void*)-1) {
    trt_TerminateProcess(proc, code);
    return 0;  // unreachable when the terminate path is honest
  }
  trt_core_log("ntdll: NtTerminateProcess foreign handle -> honest fail (no bridge kill)");
  return 0xC0000008u;  // STATUS_INVALID_HANDLE
}

std::mutex g_nt_str_mu;
std::unordered_set<void*> g_nt_str_allocs;

void nt_str_track(void* p) {
  if (!p) return;
  std::lock_guard<std::mutex> lk(g_nt_str_mu);
  g_nt_str_allocs.insert(p);
}

void nt_str_free_tracked(void* p) {
  if (!p) return;
  bool owned = false;
  {
    std::lock_guard<std::mutex> lk(g_nt_str_mu);
    auto it = g_nt_str_allocs.find(p);
    if (it != g_nt_str_allocs.end()) {
      g_nt_str_allocs.erase(it);
      owned = true;
    }
  }
  if (owned) free(p);
}

void TRT_GUEST_ABI nt_RtlInitAnsiString(nt_ansi_str* dst, const char* src) {
  if (!dst) return;
  if (!src) {
    dst->length = 0;
    dst->max_length = 0;
    dst->buffer = nullptr;
    return;
  }
  size_t len = strlen(src);
  if (len > 0xFFFEu) len = 0xFFFEu;
  dst->length = (u16)len;
  dst->max_length = (u16)(len + 1u);
  dst->buffer = (char*)src;
}

void TRT_GUEST_ABI nt_RtlInitUnicodeString(nt_unicode_str* dst, const u16* src) {
  if (!dst) return;
  if (!src) {
    dst->length = 0;
    dst->max_length = 0;
    dst->buffer = nullptr;
    return;
  }
  size_t n = 0;
  while (src[n] && n < 0x7FFEu) ++n;
  dst->length = (u16)(n * 2u);
  dst->max_length = (u16)((n + 1u) * 2u);
  dst->buffer = (u16*)src;
}

u32 TRT_GUEST_ABI nt_RtlAnsiStringToUnicodeString(nt_unicode_str* dst,
                                                 const nt_ansi_str* src,
                                                 u8 allocate) {
  if (!dst || !src) return 0xC000000Du;
  if (src->length == 0 || !src->buffer) {
    if (allocate) {
      u16* buf = (u16*)calloc(1, sizeof(u16));
      if (!buf) return 0xC0000017u;
      nt_str_track(buf);
      dst->buffer = buf;
      dst->length = 0;
      dst->max_length = 2;
    } else {
      dst->length = 0;
      if (dst->buffer && dst->max_length >= 2) dst->buffer[0] = 0;
    }
    return 0;
  }
  int wlen = trt_MultiByteToWideChar(0, 0, src->buffer, (i32)src->length, nullptr, 0);
  if (wlen < 0) wlen = 0;
  const u32 cap_bytes = (u32)(wlen * 2);
  if (cap_bytes > 0xFFFEu) return 0x80000005u;
  if (allocate) {
    u16* buf = (u16*)malloc(cap_bytes + 2u);
    if (!buf) return 0xC0000017u;
    int actual = wlen > 0 ? trt_MultiByteToWideChar(0, 0, src->buffer, (i32)src->length, buf, wlen) : 0;
    if (actual < 0) actual = 0;
    buf[actual] = 0;
    nt_str_track(buf);
    dst->buffer = buf;
    dst->length = (u16)(actual * 2);
    dst->max_length = (u16)(cap_bytes + 2u);
    return 0;
  }
  if (!dst->buffer || dst->max_length < cap_bytes + 2u) return 0x80000005u;
  int actual = wlen > 0 ? trt_MultiByteToWideChar(0, 0, src->buffer, (i32)src->length, dst->buffer, wlen) : 0;
  if (actual < 0) actual = 0;
  dst->buffer[actual] = 0;
  dst->length = (u16)(actual * 2);
  return 0;
}

u32 TRT_GUEST_ABI nt_RtlUnicodeStringToAnsiString(nt_ansi_str* dst,
                                                 const nt_unicode_str* src,
                                                 u8 allocate) {
  if (!dst || !src) return 0xC000000Du;
  const i32 wchars = (i32)(src->length / 2u);
  if (wchars == 0 || !src->buffer) {
    if (allocate) {
      char* buf = (char*)calloc(1, 1);
      if (!buf) return 0xC0000017u;
      nt_str_track(buf);
      dst->buffer = buf;
      dst->length = 0;
      dst->max_length = 1;
    } else {
      dst->length = 0;
      if (dst->buffer && dst->max_length >= 1) dst->buffer[0] = 0;
    }
    return 0;
  }
  int alen = trt_WideCharToMultiByte(0, 0, src->buffer, wchars, nullptr, 0, nullptr, nullptr);
  if (alen < 0) alen = 0;
  const u32 bytes = (u32)alen;
  if (bytes > 0xFFFEu) return 0x80000005u;
  if (allocate) {
    char* buf = (char*)malloc(bytes + 1u);
    if (!buf) return 0xC0000017u;
    if (alen > 0) {
      trt_WideCharToMultiByte(0, 0, src->buffer, wchars, buf, alen, nullptr, nullptr);
    }
    buf[alen] = 0;
    nt_str_track(buf);
    dst->buffer = buf;
    dst->length = (u16)bytes;
    dst->max_length = (u16)(bytes + 1u);
    return 0;
  }
  if (!dst->buffer || dst->max_length < bytes + 1u) return 0x80000005u;
  if (alen > 0) {
    trt_WideCharToMultiByte(0, 0, src->buffer, wchars, dst->buffer, alen, nullptr, nullptr);
  }
  dst->buffer[alen] = 0;
  dst->length = (u16)bytes;
  return 0;
}

void TRT_GUEST_ABI nt_RtlFreeUnicodeString(nt_unicode_str* s) {
  if (!s) return;
  nt_str_free_tracked(s->buffer);
  s->buffer = nullptr;
  s->length = 0;
  s->max_length = 0;
}

void TRT_GUEST_ABI nt_RtlFreeAnsiString(nt_ansi_str* s) {
  if (!s) return;
  nt_str_free_tracked(s->buffer);
  s->buffer = nullptr;
  s->length = 0;
  s->max_length = 0;
}

i32 TRT_GUEST_ABI nt_RtlCompareUnicodeString(const nt_unicode_str* s1,
                                            const nt_unicode_str* s2,
                                            u8 case_insensitive) {
  if (!s1 && !s2) return 0;
  if (!s1) return -1;
  if (!s2) return 1;
  const u32 n1 = s1->length / 2u;
  const u32 n2 = s2->length / 2u;
  const u32 n = n1 < n2 ? n1 : n2;
  for (u32 i = 0; i < n; ++i) {
    u16 c1 = s1->buffer ? s1->buffer[i] : 0;
    u16 c2 = s2->buffer ? s2->buffer[i] : 0;
    if (case_insensitive) {
      if (c1 >= 'a' && c1 <= 'z') c1 -= 32;
      if (c2 >= 'a' && c2 <= 'z') c2 -= 32;
    }
    if (c1 != c2) return (c1 < c2) ? -1 : 1;
  }
  if (n1 == n2) return 0;
  return (n1 < n2) ? -1 : 1;
}

u8 TRT_GUEST_ABI nt_RtlEqualUnicodeString(const nt_unicode_str* s1,
                                         const nt_unicode_str* s2,
                                         u8 case_insensitive) {
  if (!s1 || !s2) return (s1 == s2) ? 1 : 0;
  if (s1->length != s2->length) return 0;
  return nt_RtlCompareUnicodeString(s1, s2, case_insensitive) == 0 ? 1 : 0;
}

u32 TRT_GUEST_ABI nt_RtlGetLastWin32Error(void) { return trt_GetLastError(); }
void TRT_GUEST_ABI nt_RtlSetLastWin32Error(u32 e) { trt_SetLastError(e); }

// ---- System / Process / VirtualMemory Query Surfaces -----------------------
static u32 nt_cpu_count() {
  u32 n = (u32)std::thread::hardware_concurrency();
  return n ? n : 8u;
}

#pragma pack(push, 1)
struct nt_system_basic_info {
  u32 reserved;
  u32 timer_resolution;
  u32 page_size;
  u32 number_of_physical_pages;
  u32 lowest_physical_page_number;
  u32 highest_physical_page_number;
  u32 allocation_granularity;
  u32 abi_pad; // real x64 ABI: 4 bytes pad before the ULONG_PTR fields (sizeof must be 64)
  u64 minimum_user_mode_address;
  u64 maximum_user_mode_address;
  u64 active_processors_affinity_mask;
  u8 number_of_processors;
  u8 pad[7];
};
struct nt_system_processor_info {
  u16 processor_architecture;
  u16 processor_level;
  u16 processor_revision;
  u16 maximum_processors;
  u32 processor_feature_bits;
};
struct nt_system_logical_proc_info {
  u64 processor_mask;
  u32 relationship;
  u32 pad;
  u64 flags[2];
};
struct nt_process_basic_info {
  u64 exit_status;
  u64 peb_base_address;
  u64 affinity_mask;
  u64 base_priority;
  u64 unique_process_id;
  u64 inherited_from_unique_process_id;
};
#pragma pack(pop)

u32 TRT_GUEST_ABI nt_NtQuerySystemInformation(u32 cls, void* buf, u32 len,
                                             u32* ret_len) {
  const u32 cpus = nt_cpu_count();
  if (cls == 0 /*SystemBasicInformation*/) {
    if (ret_len) *ret_len = (u32)sizeof(nt_system_basic_info);
    if (!buf || len < sizeof(nt_system_basic_info)) return 0xC0000004u; // STATUS_INFO_LENGTH_MISMATCH
    nt_system_basic_info sbi{};
    sbi.timer_resolution = 156250;
    sbi.page_size = 4096u;
    sbi.number_of_physical_pages = 5u * 1024u * 256u; // 5 GiB in 4K pages
    sbi.lowest_physical_page_number = 1u;
    sbi.highest_physical_page_number = sbi.number_of_physical_pages;
    sbi.allocation_granularity = 65536u;
    sbi.minimum_user_mode_address = 0x0000000000010000ULL;
    sbi.maximum_user_mode_address = 0x00007FFFFFFEFFFFULL;
    sbi.active_processors_affinity_mask = (cpus >= 64) ? ~0ULL : ((1ULL << cpus) - 1ULL);
    sbi.number_of_processors = (u8)(cpus > 255 ? 255 : cpus);
    memcpy(buf, &sbi, sizeof(sbi));
    return 0u;
  }
  if (cls == 1 /*SystemProcessorInformation*/) {
    if (ret_len) *ret_len = (u32)sizeof(nt_system_processor_info);
    if (!buf || len < sizeof(nt_system_processor_info)) return 0xC0000004u;
    nt_system_processor_info spi{};
    spi.processor_architecture = 9; // PROCESSOR_ARCHITECTURE_AMD64
    spi.processor_level = 23;       // AMD Zen 2 family
    spi.processor_revision = 0x4700;
    spi.maximum_processors = (u16)(cpus > 65535 ? 65535 : cpus);
    spi.processor_feature_bits = 0x3FFFFFFFu;
    memcpy(buf, &spi, sizeof(spi));
    return 0u;
  }
  if (cls == 2 /*SystemPerformanceInformation*/ || cls == 3 /*SystemTimeOfDayInformation*/) {
    const u32 min_sz = (cls == 2) ? 48u : 48u;
    if (ret_len) *ret_len = len ? len : min_sz;
    if (!buf || len < min_sz) return 0xC0000004u;
    memset(buf, 0, len);
    return 0u;
  }
  if (cls == 73 /*SystemLogicalProcessorInformation*/) {
    const u32 need = (u32)sizeof(nt_system_logical_proc_info);
    if (ret_len) *ret_len = need;
    if (!buf || len < need) return 0xC0000004u;
    nt_system_logical_proc_info lpi{};
    lpi.processor_mask = (cpus >= 64) ? ~0ULL : ((1ULL << cpus) - 1ULL);
    lpi.relationship = 0; // RelationProcessorCore
    memcpy(buf, &lpi, sizeof(lpi));
    return 0u;
  }
  if (cls == 5 /*SystemProcessInformation*/) {
    // Minimal single-process entry: NextEntryOffset=0, NumberOfThreads=1, ImageName empty.
    const u32 need = 256u;
    if (ret_len) *ret_len = need;
    if (!buf || len < need) return 0xC0000004u;
    memset(buf, 0, need);
    u32* u32s = (u32*)buf;
    u32s[1] = 1u; // NumberOfThreads
    return 0u;
  }
  char line[128];
  snprintf(line, sizeof(line),
           "ntdll: NtQuerySystemInformation(class=%u) -> STATUS_NOT_IMPLEMENTED", cls);
  trt_core_log(line);
  return 0xC0000002u; // STATUS_NOT_IMPLEMENTED
}

u32 TRT_GUEST_ABI nt_NtQueryInformationProcess(void* proc, u32 cls, void* buf,
                                                u32 len, u32* ret_len) {
  (void)proc;
  if (cls == 0 /*ProcessBasicInformation*/) {
    if (ret_len) *ret_len = (u32)sizeof(nt_process_basic_info);
    if (!buf || len < sizeof(nt_process_basic_info)) return 0xC0000004u;
    nt_process_basic_info pbi{};
    pbi.exit_status = 259u; // STILL_ACTIVE
    pbi.affinity_mask = (nt_cpu_count() >= 64) ? ~0ULL : ((1ULL << nt_cpu_count()) - 1ULL);
    pbi.base_priority = 8;
    pbi.unique_process_id = 1000u;
    pbi.inherited_from_unique_process_id = 100u;
    memcpy(buf, &pbi, sizeof(pbi));
    return 0u;
  }
  if (cls == 7 /*ProcessDebugPort*/ || cls == 26 /*ProcessWow64Information*/) {
    if (ret_len) *ret_len = 8u;
    if (!buf || len < 8u) return 0xC0000004u;
    *(u64*)buf = 0ULL;
    return 0u;
  }
  if (cls == 30 /*ProcessDebugObjectHandle*/) {
    if (ret_len) *ret_len = 8u;
    if (!buf || len < 8u) return 0xC0000004u;
    *(u64*)buf = 0ULL;
    return 0xC0000353u; // STATUS_PORT_NOT_SET
  }
  if (cls == 31 /*ProcessDebugFlags*/) {
    if (ret_len) *ret_len = 4u;
    if (!buf || len < 4u) return 0xC0000004u;
    *(u32*)buf = 1u; // PROCESS_DEBUG_INHERIT (not debugged)
    return 0u;
  }
  if (cls == 27 /*ProcessImageFileName*/ || cls == 43 /*ProcessImageFileNameWin32*/) {
    const char* path = (cls == 43) ? "Z:\\game.exe" : "\\Device\\HarddiskVolume1\\game.exe";
    const u32 chars = (u32)strlen(path);
    const u32 str_bytes = chars * 2u;
    const u32 need = (u32)sizeof(nt_unicode_str) + str_bytes + 2u;
    if (ret_len) *ret_len = need;
    if (!buf || len < need) return 0xC0000004u;
    auto* us = (nt_unicode_str*)buf;
    u16* wstr = (u16*)((u8*)buf + sizeof(nt_unicode_str));
    for (u32 i = 0; i < chars; ++i) wstr[i] = (u16)(u8)path[i];
    wstr[chars] = 0;
    us->length = (u16)str_bytes;
    us->max_length = (u16)(str_bytes + 2u);
    us->buffer = wstr;
    return 0u;
  }
  char line[128];
  snprintf(line, sizeof(line),
           "ntdll: NtQueryInformationProcess(class=%u) -> STATUS_NOT_IMPLEMENTED", cls);
  trt_core_log(line);
  return 0xC0000002u;
}

u32 TRT_GUEST_ABI nt_NtQueryVirtualMemory(void* proc, const void* addr, u32 cls,
                                          void* buf, u64 len, u64* ret_len) {
  (void)proc;
  if (cls == 0 /*MemoryBasicInformation*/) {
    const u64 wrote = trt_VirtualQuery(addr, buf, len);
    if (ret_len) *ret_len = wrote;
    return wrote ? 0u : 0xC000000Du;
  }
  char line[128];
  snprintf(line, sizeof(line),
           "ntdll: NtQueryVirtualMemory(class=%u) -> STATUS_NOT_IMPLEMENTED", cls);
  trt_core_log(line);
  return 0xC0000002u;
}

// --- Bun / WebKit startup primitives ---------------------------------------
// opcode-level runtimes (Bun, V8) park on futexes through the Rtl* names and
// seed their PRNG through ProcessPrng.  Without these the loader synthesises
// a return-0 thunk and the runtime corrupts itself during startup.
extern "C" int TRT_STDCALL trt_WaitOnAddress(volatile void*, void*, u32, u32);
extern "C" void TRT_STDCALL trt_WakeByAddressSingle(volatile void*);
extern "C" void TRT_STDCALL trt_WakeByAddressAll(volatile void*);

static int TRT_STDCALL nt_RtlWaitOnAddress(volatile void* addr, void* cmp,
                                           u64 size,
                                           const long long* timeout) {
  u32 ms = 0xFFFFFFFFu;
  if (timeout) {
    long long t = *timeout;  // relative, 100 ns units, negative
    if (t < 0) {
      t = -t;
      ms = (u32)(t / 10000);
      if (ms == 0 && t != 0) ms = 1;
    } else {
      ms = 0;
    }
  }
  return trt_WaitOnAddress(addr, cmp, (u32)size, ms);
}

static void TRT_STDCALL nt_RtlWakeAddressSingle(void* addr) {
  trt_WakeByAddressSingle((volatile void*)addr);
}

static void TRT_STDCALL nt_RtlWakeAddressAll(void* addr) {
  trt_WakeByAddressAll((volatile void*)addr);
}

// Growable function tables are a JIT unwinding detail; recording nothing is
// safe (unwinds fall back to the frame pointer), returning failure is not.
static u32 TRT_STDCALL nt_RtlAddGrowableFunctionTable(void** table,
                                                      void* fn_table,
                                                      u32 entry_count,
                                                      u32 max_count,
                                                      u64 range_base,
                                                      u64 range_end) {
  (void)fn_table; (void)entry_count; (void)max_count;
  (void)range_base; (void)range_end;
  if (table) *table = nullptr;
  return 0;
}

static void TRT_STDCALL nt_RtlDeleteGrowableFunctionTable(void* table) {
  (void)table;
}

static void TRT_STDCALL nt_RtlGrowFunctionTable(void* table, u32 new_count) {
  (void)table; (void)new_count;
}

static u16 TRT_STDCALL nt_RtlCaptureStackBackTrace(u32 frames, u32 skip,
                                                   void** out, u32* hash) {
#if defined(_WIN32)
  return (u16)::CaptureStackBackTrace(skip, frames, out, (DWORD*)hash);
#else
  (void)frames; (void)skip; (void)out;
  if (hash) *hash = 0;
  return 0;
#endif
}

} // namespace

// .242 DIAG: NTALLOC accessors for VIDEO-SAMPLE (vk_backend.hpp).
unsigned long long trt_ntalloc_calls(void) {
  return g_ntalloc242_calls.load(std::memory_order_relaxed);
}
unsigned long long trt_ntalloc_bytes(void) {
  return g_ntalloc242_bytes.load(std::memory_order_relaxed);
}

int trt_bridge_register_ntdll(void) {
  static const trt_named_fn k_fns[] = {
      {"RtlCaptureContext", (void*)&trt_RtlCaptureContext},
      {"RtlLookupFunctionEntry", (void*)&trt_RtlLookupFunctionEntry},
      {"RtlVirtualUnwind", (void*)&trt_RtlVirtualUnwind},
      {"RtlPcToFileHeader", (void*)&trt_RtlPcToFileHeader},
      {"RtlAddFunctionTable", (void*)&trt_RtlAddFunctionTable},
      {"RtlAddGrowableFunctionTable", (void*)&nt_RtlAddGrowableFunctionTable},
      {"RtlDeleteGrowableFunctionTable", (void*)&nt_RtlDeleteGrowableFunctionTable},
      {"RtlGrowFunctionTable", (void*)&nt_RtlGrowFunctionTable},
      {"RtlWaitOnAddress", (void*)&nt_RtlWaitOnAddress},
      {"RtlWakeAddressSingle", (void*)&nt_RtlWakeAddressSingle},
      {"RtlWakeAddressAll", (void*)&nt_RtlWakeAddressAll},
      {"RtlCaptureStackBackTrace", (void*)&nt_RtlCaptureStackBackTrace},
      {"RtlDeleteFunctionTable", (void*)&trt_RtlDeleteFunctionTable},
      {"RtlUnwind", (void*)&trt_RtlUnwind},
      {"RtlUnwindEx", (void*)&trt_RtlUnwindEx},
      {"RtlNtStatusToDosError", (void*)&nt_RtlNtStatusToDosError},
      {"RtlGetVersion", (void*)&nt_RtlGetVersion},
      {"RtlVerifyVersionInfo", (void*)&nt_RtlVerifyVersionInfo},
      {"RtlGetDeviceFamilyInfoEnum", (void*)&nt_RtlGetDeviceFamilyInfoEnum},
      {"RtlInitAnsiString", (void*)&nt_RtlInitAnsiString},
      {"RtlInitUnicodeString", (void*)&nt_RtlInitUnicodeString},
      {"RtlAnsiStringToUnicodeString", (void*)&nt_RtlAnsiStringToUnicodeString},
      {"RtlUnicodeStringToAnsiString", (void*)&nt_RtlUnicodeStringToAnsiString},
      {"RtlFreeUnicodeString", (void*)&nt_RtlFreeUnicodeString},
      {"RtlFreeAnsiString", (void*)&nt_RtlFreeAnsiString},
      {"RtlFreeOemString", (void*)&nt_RtlFreeAnsiString},
      {"RtlCompareUnicodeString", (void*)&nt_RtlCompareUnicodeString},
      {"RtlEqualUnicodeString", (void*)&nt_RtlEqualUnicodeString},
      {"RtlGetLastWin32Error", (void*)&nt_RtlGetLastWin32Error},
      {"RtlSetLastWin32Error", (void*)&nt_RtlSetLastWin32Error},
      {"NtAllocateVirtualMemory", (void*)&nt_NtAllocateVirtualMemory},
      {"NtAllocateVirtualMemoryEx", (void*)&nt_NtAllocateVirtualMemoryEx},
      {"NtFreeVirtualMemory", (void*)&nt_NtFreeVirtualMemory},
      {"NtProtectVirtualMemory", (void*)&nt_NtProtectVirtualMemory},
      {"NtClose", (void*)&nt_NtClose},
      {"NtQueryInformationFile", (void*)&nt_NtQueryInformationFile},
      {"NtWriteFile", (void*)&nt_NtWriteFile},
      {"NtCreateFile", (void*)&nt_NtCreateFile},
      {"NtReadFile", (void*)&nt_NtReadFile},
      {"NtCreateEvent", (void*)&nt_NtCreateEvent},
      {"NtSetEvent", (void*)&nt_NtSetEvent},
      {"NtWaitForSingleObject", (void*)&nt_NtWaitForSingleObject},
      {"NtSignalAndWaitForSingleObject",
       (void*)&nt_NtSignalAndWaitForSingleObject},
      {"NtQuerySystemInformation", (void*)&nt_NtQuerySystemInformation},
      {"NtQueryInformationProcess", (void*)&nt_NtQueryInformationProcess},
      {"NtQueryVirtualMemory", (void*)&nt_NtQueryVirtualMemory},
      {"RaiseFailFastException", (void*)&nt_RaiseFailFastException},
      {"RtlExitUserProcess", (void*)&nt_RtlExitUserProcess},
      {"NtTerminateProcess", (void*)&nt_NtTerminateProcess},
      {"ZwTerminateProcess", (void*)&nt_NtTerminateProcess},
      {"EtwEventRegister", (void*)&nt_EtwEventRegister},
      {"EtwEventUnregister", (void*)&nt_EtwEventUnregister},
      {"EtwEventWrite", (void*)&nt_EtwEventWrite},
      {"EtwEventWriteEx", (void*)&nt_EtwEventWriteEx},
      {"EtwEventWriteTransfer", (void*)&nt_EtwEventWriteTransfer},
      {"EtwEventWriteString", (void*)&nt_EtwEventWriteString},
      {"EtwEventSetInformation", (void*)&nt_EtwEventSetInformation},
      {"EtwEventEnabled", (void*)&nt_EtwEventEnabled},
      {"EtwEventActivityIdControl", (void*)&nt_EtwEventActivityIdControl},
      // Aliases for advapi32 Event* via api-ms-win-eventing-provider
      {"EventRegister", (void*)&nt_EtwEventRegister},
      {"EventUnregister", (void*)&nt_EtwEventUnregister},
      {"EventWrite", (void*)&nt_EtwEventWrite},
      {"EventWriteEx", (void*)&nt_EtwEventWriteEx},
      {"EventWriteTransfer", (void*)&nt_EtwEventWriteTransfer},
      {"EventWriteString", (void*)&nt_EtwEventWriteString},
      {"EventSetInformation", (void*)&nt_EtwEventSetInformation},
      {"EventEnabled", (void*)&nt_EtwEventEnabled},
      {"EventActivityIdControl", (void*)&nt_EtwEventActivityIdControl},
  };
  trt_module_register("ntdll.dll", TRT_ROUTE_BUILTIN, k_fns,
                      (int)(sizeof(k_fns) / sizeof(k_fns[0])));
  return 0;
}

} // extern "C"
