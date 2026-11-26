// tiramisu/tests/test_pe32.cpp
// Offline proof for the M7 Phase 1 PE32 loader on a Linux host (LOAD-ONLY:
// no 32-bit code executes — there is no x86 engine until Phase 2):
//   1. map a real PE32 guest DLL (guest_lib32.dll) at its preferred base
//   2. map a real PE32 guest EXE (guest_hello32.exe) whose preferred base
//      is taken -> guest-VA relocation, HIGHLOW fixups verified byte-wise
//      against the file's own .reloc directory
//   3. bind its imports: kernel32/delay symbols -> trap addresses from the
//      reserved range (deduped across images), the sibling DLL export ->
//      the export's GUEST VA (not a trap)
//   4. seal (W^X) and audit (trt_audit_imports, 4-byte variant)
//   5. negative: bad magic, machine/magic mismatch, truncation, insane
//      import/reloc/TLS RVAs, DIR64 in a PE32 image -> NAMED errors, no crash
// Console runs the identical loader through the UWP backend.
#include "../src/trt/trt.h"
#include "../src/trt/pe_exports.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>
#include <string>

// ---------------- mmap backend (overlap-checked: a fixed preferred-base
// allocation FAILS when the range is taken — the console contract) --------
namespace {

struct extent { void* base; size_t len; };
extent g_extents[64];
int g_extent_count = 0;

void* linux_alloc(size_t bytes, u64 preferred_base) {
  if (preferred_base) {
    const uintptr_t want = (uintptr_t)preferred_base;
    const uintptr_t want_end = want + bytes;
    for (int i = 0; i < g_extent_count; ++i) {
      if (!g_extents[i].base) continue;
      const uintptr_t b = (uintptr_t)g_extents[i].base;
      const uintptr_t e = b + g_extents[i].len;
      if (want < e && b < want_end) return nullptr;
    }
  }
  void* p = mmap(preferred_base ? (void*)(uintptr_t)preferred_base : nullptr,
                 bytes, PROT_READ | PROT_WRITE,
                 preferred_base ? (MAP_FIXED_NOREPLACE | MAP_PRIVATE | MAP_ANONYMOUS)
                                : (MAP_PRIVATE | MAP_ANONYMOUS),
                 -1, 0);
  if (p == MAP_FAILED) return nullptr;
  int slot = -1;
  for (int i = 0; i < g_extent_count; ++i) {
    if (!g_extents[i].base) { slot = i; break; }
  }
  if (slot < 0 && g_extent_count < 64) slot = g_extent_count++;
  if (slot >= 0) {
    g_extents[slot].base = p;
    g_extents[slot].len = bytes;
  }
  return p;
}

int linux_protect(void* base, size_t bytes, unsigned prot) {
  int p = 0;
  if (prot & TRT_PROT_R) p |= PROT_READ;
  if (prot & TRT_PROT_W) p |= PROT_WRITE;
  if (prot & TRT_PROT_X) p |= PROT_EXEC;
  const size_t page = (size_t)sysconf(_SC_PAGESIZE);
  const uintptr_t b = (uintptr_t)base & ~(uintptr_t)(page - 1);
  const size_t len = ((uintptr_t)base + bytes - b + page - 1) & ~(uintptr_t)(page - 1);
  return mprotect((void*)b, len, p);
}

void linux_release(void* base, size_t bytes) {
  for (int i = 0; i < g_extent_count; ++i)
    if (g_extents[i].base == base) {
      munmap(base, g_extents[i].len);
      g_extents[i].base = nullptr;
      return;
    }
  (void)bytes;
}

void linux_log(const char* line) { printf("  [trt] %s\n", line); }

// ---------------- import plumbing ----------------
// The mapped lib32 image: its exports resolve to host pointers INTO the
// mapped image, and the PE32 loader must translate them to guest VAs.
trt_image* g_lib32_img = nullptr;

bool ieq(const char* a, const char* b) {
  while (*a && *b) {
    char ca = *a, cb = *b;
    if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
    if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
    if (ca != cb) return false;
    ++a; ++b;
  }
  return *a == *b;
}

void* pe32_test_resolve(void* user, const char* module, const char* symbol,
                        u16 ordinal) {
  (void)user;
  if (ieq(module, "guest_lib32.dll") && g_lib32_img) {
    void* fn = trt_pe_find_export(nullptr, nullptr, g_lib32_img, symbol, ordinal);
    if (fn) return fn;
  }
  // kernel32 + delay-loaded guest_delay32.dll: bindable bridge symbols
  // (a dummy non-null address — the loader turns it into a trap; the
  // address itself is never called in this load-only test).
  if (ieq(module, "kernel32.dll") || ieq(module, "guest_delay32.dll"))
    return (void*)(uintptr_t)0x1000;
  return nullptr;
}

int failures = 0;
#define CHECK(cond, msg)                                              \
  do {                                                                \
    if (cond) { printf("  PASS %s\n", msg); }                          \
    else { printf("  FAIL %s\n", msg); ++failures; }                   \
  } while (0)

u8* read_file(const char* path, size_t* size) {
  FILE* f = fopen(path, "rb");
  if (!f) return nullptr;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  u8* buf = (u8*)malloc((size_t)n ? (size_t)n : 1);
  if (n > 0 && fread(buf, 1, (size_t)n, f) != (size_t)n) {
    free(buf); fclose(f); return nullptr;
  }
  fclose(f);
  *size = (size_t)n;
  return buf;
}

u16 rd16b(const u8* p) { u16 v; memcpy(&v, p, 2); return v; }
u32 rd32b(const u8* p) { u32 v; memcpy(&v, p, 4); return v; }

// ---- PE32 file parsing helpers (operate on the FILE bytes) ----
// Returns the file offset of an RVA via the section table, or 0.
u64 file_rva2off(const u8* file, size_t size, u64 rva) {
  const u32 nt_off = rd32b(file + 0x3C);
  const u16 nsec = rd16b(file + nt_off + 6);
  const u16 opt_sz = rd16b(file + nt_off + 20);
  const u8* secs = file + nt_off + 24 + opt_sz;
  for (u32 i = 0; i < nsec; ++i) {
    const u8* sh = secs + (size_t)i * 40;
    if (sh + 40 > file + size) break;
    const u32 va = rd32b(sh + 12);
    const u32 vsz = rd32b(sh + 8);
    const u32 rsz = rd32b(sh + 16);
    const u32 roff = rd32b(sh + 20);
    const u32 span = vsz ? vsz : rsz;
    if (rva >= va && rva < (u64)va + span) {
      const u64 delta = rva - va;
      if (delta < rsz && roff + delta < size) return roff + delta;
      return 0;
    }
  }
  if (rva < size) return rva;  // headers region
  return 0;
}

// Verify every HIGHLOW fixup the loader applied: mapped_value must equal
// file_value + delta32. Entries whose target is an IAT slot (static or
// delay) are SKIPPED, not failed: the loader relocates first and then
// binds the import, legitimately overwriting the slot with a trap address
// or a sibling guest VA (the import-walk checks verify those slots
// separately). Returns the number of HIGHLOW entries verified, or -1 on a
// structural problem; *skipped_out counts the skipped IAT-slot entries.
int verify_highlow(const u8* file, size_t size, const trt_image* img,
                   u32 delta32, int* skipped_out) {
  *skipped_out = 0;
  const u32 nt_off = rd32b(file + 0x3C);
  const u8* opt = file + nt_off + 24;
  const trt_data_directory* dirs = trt_pe_directories(opt, TRT_PE_OPT32_MAGIC);

  // collect the IAT slot ranges (static + delay) from the FILE's import
  // descriptors: any HIGHLOW target inside one is a bound slot, not a
  // plain relocated pointer.
  struct range { u64 lo, hi; };
  range iat_ranges[16];
  int n_ranges = 0;
  auto add_iat_range = [&](u32 iat_rva, u32 int_rva) {
    if (!iat_rva || !int_rva || n_ranges >= 16) return;
    int count = 0;
    for (u64 t = 0; t < 1024; t += 4) {
      const u64 off = file_rva2off(file, size, int_rva + t);
      if (!off) break;
      if (rd32b(file + off) == 0) break;
      ++count;
    }
    if (count > 0) {
      iat_ranges[n_ranges].lo = iat_rva;
      iat_ranges[n_ranges].hi = iat_rva + (u64)count * 4;
      ++n_ranges;
    }
  };
  const u32 imp_va = dirs[TRT_PE_DIR_IMPORT].virtual_address;
  const u32 imp_sz = dirs[TRT_PE_DIR_IMPORT].size;
  if (imp_va && imp_sz) {
    const u64 imp_off = file_rva2off(file, size, imp_va);
    if (imp_off) {
      for (u64 d = 0; d + 20 <= imp_sz; d += 20) {
        const u8* desc = file + imp_off + d;
        const u32 oft = rd32b(desc);
        const u32 name_rva = rd32b(desc + 12);
        const u32 iat_rva = rd32b(desc + 16);
        if (!iat_rva && !name_rva) break;
        add_iat_range(iat_rva, oft ? oft : iat_rva);
      }
    }
  }
  const u32 dd_va = dirs[TRT_PE_DIR_DELAY_IMPORT].virtual_address;
  const u32 dd_sz = dirs[TRT_PE_DIR_DELAY_IMPORT].size;
  if (dd_va && dd_sz) {
    const u64 dd_off = file_rva2off(file, size, dd_va);
    if (dd_off) {
      for (u64 d = 0; d + 32 <= dd_sz; d += 32) {
        const u8* dd = file + dd_off + d;
        const u32 name_rva = rd32b(dd + 4);
        const u32 iat_rva = rd32b(dd + 12);
        const u32 int_rva = rd32b(dd + 16);
        if (!name_rva && !iat_rva) break;
        add_iat_range(iat_rva, int_rva);
      }
    }
  }

  const u32 reloc_va = dirs[TRT_PE_DIR_RELOC].virtual_address;
  const u32 reloc_sz = dirs[TRT_PE_DIR_RELOC].size;
  if (!reloc_va || !reloc_sz) return -1;
  const u64 reloc_off = file_rva2off(file, size, reloc_va);
  if (!reloc_off) return -1;
  int checked = 0;
  u64 off = 0;
  while (off + 8 <= reloc_sz) {
    const u8* blk = file + reloc_off + off;
    const u32 page_rva = rd32b(blk);
    const u32 block_size = rd32b(blk + 4);
    if (block_size < 8 || off + block_size > reloc_sz) return -1;
    const u32 n_entries = (block_size - 8) / 2;
    for (u32 e = 0; e < n_entries; ++e) {
      const u16 entry = rd16b(blk + 8 + (size_t)e * 2);
      const u32 type = entry >> 12;
      const u32 fo = entry & 0x0FFF;
      if (type == TRT_RELOC_ABSOLUTE) continue;
      if (type != TRT_RELOC_HIGHLOW) return -1;  // guest must be HIGHLOW-only
      const u64 tgt_rva = page_rva + fo;
      if (tgt_rva + 4 > img->size_of_image) return -1;
      // a HIGHLOW target inside a bound IAT slot: the binder overwrote it
      // after the reloc pass — verified by the import walk, not here
      bool in_iat = false;
      for (int i = 0; i < n_ranges; ++i) {
        if (tgt_rva >= iat_ranges[i].lo && tgt_rva < iat_ranges[i].hi) {
          in_iat = true;
          break;
        }
      }
      if (in_iat) {
        ++(*skipped_out);
        continue;
      }
      const u64 f_off = file_rva2off(file, size, tgt_rva);
      if (!f_off) return -1;
      const u32 file_v = rd32b(file + f_off);
      const u32 mapped_v = rd32b(img->base + tgt_rva);
      if (mapped_v != file_v + delta32) {
        printf("  info HIGHLOW mismatch at rva=%llx file=%08x mapped=%08x delta=%08x\n",
               (unsigned long long)tgt_rva, file_v, mapped_v, delta32);
        return -1;
      }
      ++checked;
    }
    off += block_size;
  }
  return checked;
}

// Walk the MAPPED image's import table (4-byte thunks). For each import,
// calls fn(mod, sym, ordinal, slot_value). Returns the import count.
template <typename F>
int walk_mapped_imports(const trt_image* img, F&& fn) {
  const u8* base = img->base;
  const u32 lfanew = rd32b(base + 0x3C);
  const u8* opt = base + lfanew + 24;
  const u16 magic = rd16b(opt);
  const trt_data_directory* dirs = trt_pe_directories(opt, magic);
  const u32 imp_va = dirs[TRT_PE_DIR_IMPORT].virtual_address;
  const u32 imp_sz = dirs[TRT_PE_DIR_IMPORT].size;
  if (!imp_va || !imp_sz) return 0;
  int count = 0;
  for (u64 d = 0; d + 20 <= imp_sz; d += 20) {
    const u8* desc = base + imp_va + d;
    const u32 oft = rd32b(desc);
    const u32 name_rva = rd32b(desc + 12);
    const u32 iat_rva = rd32b(desc + 16);
    if (!iat_rva && !name_rva) break;
    const char* mod = (const char*)(base + name_rva);
    const u64 int_rva = oft ? oft : iat_rva;
    for (u64 t = 0;; t += 4) {
      if (int_rva + t + 4 > img->size_of_image ||
          iat_rva + t + 4 > img->size_of_image)
        break;
      const u32 thunk = rd32b(base + int_rva + t);
      if (thunk == 0) break;
      const u32 slot = rd32b(base + iat_rva + t);
      if (thunk & 0x80000000u) {
        fn(mod, nullptr, (u16)(thunk & 0xFFFF), slot);
      } else {
        const char* sym = (const char*)(base + thunk + 2);
        fn(mod, sym, 0, slot);
      }
      ++count;
    }
  }
  return count;
}

// Walk the MAPPED image's delay-import table. Returns the delay count.
template <typename F>
int walk_mapped_delay_imports(const trt_image* img, F&& fn) {
  const u8* base = img->base;
  const u32 lfanew = rd32b(base + 0x3C);
  const u8* opt = base + lfanew + 24;
  const u16 magic = rd16b(opt);
  const trt_data_directory* dirs = trt_pe_directories(opt, magic);
  const u32 dd_va = dirs[TRT_PE_DIR_DELAY_IMPORT].virtual_address;
  const u32 dd_sz = dirs[TRT_PE_DIR_DELAY_IMPORT].size;
  if (!dd_va || !dd_sz) return 0;
  int count = 0;
  for (u64 d = 0; d + 32 <= dd_sz; d += 32) {
    const u8* dd = base + dd_va + d;
    const u32 attrs = rd32b(dd);
    const u32 name_rva = rd32b(dd + 4);
    const u32 iat_rva = rd32b(dd + 12);
    const u32 int_rva = rd32b(dd + 16);
    if (!name_rva && !iat_rva) break;
    (void)attrs;  // lld-link emits RVA-based descriptors
    const char* mod = (const char*)(base + name_rva);
    for (u64 t = 0;; t += 4) {
      if (int_rva + t + 4 > img->size_of_image ||
          iat_rva + t + 4 > img->size_of_image)
        break;
      const u32 thunk = rd32b(base + int_rva + t);
      if (thunk == 0) break;
      const u32 slot = rd32b(base + iat_rva + t);
      if (thunk & 0x80000000u) {
        fn(mod, nullptr, (u16)(thunk & 0xFFFF), slot);
      } else {
        const char* sym = (const char*)(base + thunk + 2);
        fn(mod, sym, 0, slot);
      }
      ++count;
    }
  }
  return count;
}

// Find a trap binding by module!symbol in the registry. Returns 1 + the
// trap VA, or 0 when absent.
int find_trap(const char* mod, const char* sym, u32* out_va) {
  const int n = trt_pe32_trap_count();
  for (int i = 0; i < n; ++i) {
    char m[64] = "", s[128] = "";
    u16 ord = 0;
    u32 va = 0;
    if (trt_pe32_trap_info(i, m, sizeof(m), s, sizeof(s), &ord, &va) != 0)
      continue;
    if (ieq(m, mod) && sym && strcmp(s, sym) == 0) {
      *out_va = va;
      return 1;
    }
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  setvbuf(stdout, NULL, _IONBF, 0);
  if (argc < 3) {
    fprintf(stderr, "usage: %s <guest_hello32.exe> <guest_lib32.dll>\n", argv[0]);
    return 2;
  }
  size_t exe_size = 0, lib_size = 0;
  u8* exe_file = read_file(argv[1], &exe_size);
  u8* lib_file = read_file(argv[2], &lib_size);
  if (!exe_file || !lib_file) {
    fprintf(stderr, "cannot read guest files\n");
    return 2;
  }
  printf("tiramisu offline PE32 loader test — guest %s (%zu bytes)\n",
         argv[1], exe_size);

  trt_backend be = {&linux_alloc, &linux_protect, &linux_release, &linux_log,
                    nullptr};
  trt_resolver rs = {nullptr, &pe32_test_resolve};

  // Bridge boot so the audit resolves kernel32 through the real module
  // table; the synthetic guest DLLs register STUB (they exist as files the
  // audit cannot see — the live loader resolves them through the resolver).
  trt_bridge_boot(&be);
  trt_bridge_register_kernel32();
  trt_bridge_register_user32();
  trt_module_register("guest_lib32.dll", TRT_ROUTE_STUB, nullptr, 0);
  trt_module_register("guest_delay32.dll", TRT_ROUTE_STUB, nullptr, 0);
  trt_pe32_guest_reset();

  // ---- 1. map the DLL at its preferred guest base ----
  printf("case: lib32 at preferred guest base\n");
  trt_image* lib_img = nullptr;
  int rc = trt_map_pe(&be, &rs, lib_file, lib_size, &lib_img);
  CHECK(rc == TRT_OK, "lib32 map returns OK (routed to PE32 path)");
  if (rc != TRT_OK || !lib_img) { printf("RESULT: FAILURES\n"); return 1; }
  CHECK(lib_img->is_pe32 == 1, "lib32 is_pe32 == 1");
  CHECK(lib_img->guest_base == 0x10000000u, "lib32 guest_base == preferred 0x10000000");
  CHECK(lib_img->reloc_fixups == 0, "lib32 reloc_fixups == 0 at preferred base");
  CHECK(lib_img->entry_rva == 0, "lib32 entry_rva == 0 (/NOENTRY)");
  g_lib32_img = lib_img;

  // the export the exe will bind: host pointer + RVA
  void* lib_add_fn = trt_pe_find_export(nullptr, nullptr, lib_img, "Lib32Add", 0);
  CHECK(lib_add_fn != nullptr, "Lib32Add export found in mapped lib32");
  const u32 lib_add_rva =
      lib_add_fn ? (u32)((u8*)lib_add_fn - lib_img->base) : 0;
  printf("  info Lib32Add rva=%x guest_va=%08x\n", lib_add_rva,
         lib_img->guest_base + lib_add_rva);

  // ---- 2. map the EXE: preferred guest base is taken -> relocate ----
  printf("case: exe relocated in guest space (HIGHLOW pass)\n");
  trt_image* exe_img = nullptr;
  rc = trt_map_pe(&be, &rs, exe_file, exe_size, &exe_img);
  CHECK(rc == TRT_OK, "exe map returns OK");
  if (rc != TRT_OK || !exe_img) { printf("RESULT: FAILURES\n"); return 1; }
  CHECK(exe_img->is_pe32 == 1, "exe is_pe32 == 1");
  CHECK(exe_img->guest_base != 0x10000000u, "exe guest_base != preferred (relocated)");
  CHECK(exe_img->guest_base >= 0x10000000u && exe_img->guest_base < 0x70000000u,
        "exe guest_base inside the guest image range");
  CHECK(exe_img->reloc_fixups > 0, "exe reloc_fixups > 0");
  CHECK(exe_img->entry_rva != 0, "exe entry_rva non-zero");
  const u32 delta32 = exe_img->guest_base - 0x10000000u;
  int n_hl_skipped = 0;
  const int n_hl = verify_highlow(exe_file, exe_size, exe_img, delta32,
                                  &n_hl_skipped);
  CHECK(n_hl > 0, "HIGHLOW entries verified against the file");
  printf("  info HIGHLOW verified=%d skipped(IAT slots)=%d loader fixups=%u\n",
         n_hl, n_hl_skipped, exe_img->reloc_fixups);
  CHECK(n_hl + n_hl_skipped == (int)exe_img->reloc_fixups,
        "verified + skipped HIGHLOW == loader reloc_fixups");

  // ---- 3. import binds: traps for bridges, guest VA for the sibling ----
  printf("case: import binds (traps + sibling guest VA)\n");
  u32 tick_trap = 0, err_trap = 0, lib_slot = 0;
  int n_imp = walk_mapped_imports(
      exe_img, [&](const char* mod, const char* sym, u16 ord, u32 slot) {
        (void)ord;
        if (sym && strcmp(sym, "GetTickCount64") == 0) tick_trap = slot;
        if (sym && strcmp(sym, "GetLastError") == 0) err_trap = slot;
        if (sym && strcmp(sym, "Lib32Add") == 0) lib_slot = slot;
        printf("  info import %s!%s -> %08x%s\n", mod, sym ? sym : "#ord",
               slot, trt_pe32_is_trap(slot) ? " (trap)" : " (guest VA)");
      });
  CHECK(n_imp == 3, "exe has 3 static imports");
  CHECK(trt_pe32_is_trap(tick_trap), "GetTickCount64 bound to a trap address");
  CHECK(trt_pe32_is_trap(err_trap), "GetLastError bound to a trap address");
  CHECK(tick_trap != err_trap, "distinct traps per symbol");
  CHECK(lib_slot == lib_img->guest_base + lib_add_rva,
        "Lib32Add bound to the sibling export's GUEST VA (not a trap)");
  CHECK(!trt_pe32_is_trap(lib_slot), "Lib32Add slot is not a trap address");
  CHECK(exe_img->pe32_trap_count == 3,
        "exe pe32_trap_count == 3 (2 static + 1 delay)");
  CHECK(exe_img->pe32_guestva_count == 1, "exe pe32_guestva_count == 1");

  // trap registry names the bindings
  u32 found_va = 0;
  CHECK(find_trap("kernel32.dll", "GetTickCount64", &found_va) &&
            found_va == tick_trap,
        "trap registry names kernel32!GetTickCount64");
  CHECK(find_trap("kernel32.dll", "GetLastError", &found_va) &&
            found_va == err_trap,
        "trap registry names kernel32!GetLastError");

  // delay imports: Delay32Fn -> trap
  u32 delay_slot = 0;
  int n_delay = walk_mapped_delay_imports(
      exe_img, [&](const char* mod, const char* sym, u16 ord, u32 slot) {
        (void)mod; (void)ord;
        if (sym && strcmp(sym, "Delay32Fn") == 0) delay_slot = slot;
      });
  CHECK(n_delay == 1, "exe has 1 delay import");
  CHECK(trt_pe32_is_trap(delay_slot), "Delay32Fn bound to a trap address");
  CHECK(find_trap("guest_delay32.dll", "Delay32Fn", &found_va) &&
            found_va == delay_slot,
        "trap registry names guest_delay32.dll!Delay32Fn");

  // ---- 4. seal + audit ----
  printf("case: seal + audit\n");
  CHECK(trt_seal_image(&be, exe_img) == TRT_OK, "seal OK (W^X flip, no .pdata)");
  {
    std::vector<std::string> missing;
    rc = trt_audit_imports(exe_file, exe_size, &missing);
    CHECK(rc == TRT_OK, "audit runs (TRT_OK)");
    if (!missing.empty()) {
      for (const auto& m : missing) printf("  info audit missing: %s\n", m.c_str());
    }
    CHECK(missing.empty(), "audit: no missing imports");
  }
  {
    std::vector<std::string> missing;
    rc = trt_audit_imports(lib_file, lib_size, &missing);
    CHECK(rc == TRT_OK, "audit runs on the DLL");
    CHECK(missing.empty(), "audit: DLL has no missing imports");
  }

  // ---- 5. dedup: a second map reuses the same trap addresses ----
  printf("case: trap dedup across images\n");
  trt_image* exe_img2 = nullptr;
  rc = trt_map_pe(&be, &rs, exe_file, exe_size, &exe_img2);
  CHECK(rc == TRT_OK, "second exe map returns OK");
  if (rc == TRT_OK && exe_img2) {
    CHECK(exe_img2->guest_base != exe_img->guest_base,
          "second exe gets a distinct guest base");
    u32 tick_trap2 = 0;
    walk_mapped_imports(
        exe_img2, [&](const char* mod, const char* sym, u16 ord, u32 slot) {
          (void)mod; (void)ord;
          if (sym && strcmp(sym, "GetTickCount64") == 0) tick_trap2 = slot;
        });
    CHECK(tick_trap2 == tick_trap, "GetTickCount64 trap is deduped (same VA)");
    trt_unmap(&be, exe_img2);
  }

  // ---- 6. negative cases: malformed PE32s fail NAMED, never crash ----
  printf("case: malformed PE32 inputs fail named\n");
  {
    // bad optional magic (0x10b -> 0x9999)
    std::vector<u8> m(exe_file, exe_file + exe_size);
    const u32 nt_off = rd32b(m.data() + 0x3C);
    m[nt_off + 24] = 0x99; m[nt_off + 25] = 0x99;
    trt_image* img = nullptr;
    rc = trt_map_pe(&be, &rs, m.data(), m.size(), &img);
    CHECK(rc == TRT_ERR_NOT_PE64, "bad magic -> TRT_ERR_NOT_PE64");
    CHECK(img == nullptr, "no image leaked on bad magic");
  }
  {
    // machine 0x14c + PE32+ magic 0x20b: neither path accepts it
    std::vector<u8> m(exe_file, exe_file + exe_size);
    const u32 nt_off = rd32b(m.data() + 0x3C);
    m[nt_off + 24] = 0x0b; m[nt_off + 25] = 0x02;
    trt_image* img = nullptr;
    rc = trt_map_pe(&be, &rs, m.data(), m.size(), &img);
    CHECK(rc == TRT_ERR_NOT_PE64, "machine/magic mismatch -> TRT_ERR_NOT_PE64");
    CHECK(img == nullptr, "no image leaked on mismatch");
  }
  {
    // truncated mid-optional-header
    std::vector<u8> m(exe_file, exe_file + exe_size);
    const u32 nt_off = rd32b(m.data() + 0x3C);
    m.resize(nt_off + 100);
    trt_image* img = nullptr;
    rc = trt_map_pe(&be, &rs, m.data(), m.size(), &img);
    CHECK(rc == TRT_ERR_BAD_DOS, "truncated headers -> TRT_ERR_BAD_DOS");
    CHECK(img == nullptr, "no image leaked on truncation");
  }
  {
    // insane import directory RVA (PE32 dirs at opt + 96, import = dir 1)
    std::vector<u8> m(exe_file, exe_file + exe_size);
    const u32 nt_off = rd32b(m.data() + 0x3C);
    const u32 dir_off = nt_off + 24 + 96 + 1 * 8;
    const u32 big = 0x7FFFF000u;
    memcpy(m.data() + dir_off, &big, 4);
    trt_image* img = nullptr;
    rc = trt_map_pe(&be, &rs, m.data(), m.size(), &img);
    CHECK(rc == TRT_ERR_IMPORT, "insane import RVA -> TRT_ERR_IMPORT");
    CHECK(img == nullptr, "no image leaked on insane import RVA");
  }
  {
    // insane reloc directory RVA (dir 5); needs delta != 0, so map while
    // the preferred guest base is taken (the live exe holds a bumped base,
    // and this copy's preferred base is taken by lib32)
    std::vector<u8> m(exe_file, exe_file + exe_size);
    const u32 nt_off = rd32b(m.data() + 0x3C);
    const u32 dir_off = nt_off + 24 + 96 + 5 * 8;
    const u32 big = 0x7FFFF000u;
    memcpy(m.data() + dir_off, &big, 4);
    trt_image* img = nullptr;
    rc = trt_map_pe(&be, &rs, m.data(), m.size(), &img);
    CHECK(rc == TRT_ERR_RELOC_BOUNDS, "insane reloc RVA -> TRT_ERR_RELOC_BOUNDS");
    CHECK(img == nullptr, "no image leaked on insane reloc RVA");
  }
  {
    // DIR64 reloc type in a PE32 image: flip the first HIGHLOW entry
    std::vector<u8> m(exe_file, exe_file + exe_size);
    const u32 nt_off = rd32b(m.data() + 0x3C);
    const u8* opt = m.data() + nt_off + 24;
    const trt_data_directory* dirs =
        trt_pe_directories(opt, TRT_PE_OPT32_MAGIC);
    const u64 reloc_off =
        file_rva2off(m.data(), m.size(), dirs[TRT_PE_DIR_RELOC].virtual_address);
    bool patched = false;
    if (reloc_off && reloc_off + 10 <= m.size()) {
      const u16 e = rd16b(m.data() + reloc_off + 8);
      if ((e >> 12) == TRT_RELOC_HIGHLOW) {
        const u16 bad = (u16)((TRT_RELOC_DIR64 << 12) | (e & 0x0FFF));
        memcpy(m.data() + reloc_off + 8, &bad, 2);
        patched = true;
      }
    }
    CHECK(patched, "DIR64 test: reloc entry patched");
    trt_image* img = nullptr;
    rc = trt_map_pe(&be, &rs, m.data(), m.size(), &img);
    CHECK(rc == TRT_ERR_RELOC_BADTYPE, "DIR64 in PE32 -> TRT_ERR_RELOC_BADTYPE");
    CHECK(img == nullptr, "no image leaked on DIR64 reloc");
  }
  {
    // insane TLS directory RVA (dir 9): the guest has no TLS dir, so
    // setting one creates a TLS_BOUNDS failure
    std::vector<u8> m(exe_file, exe_file + exe_size);
    const u32 nt_off = rd32b(m.data() + 0x3C);
    const u32 dir_off = nt_off + 24 + 96 + 9 * 8;
    const u32 big = 0x7FFFF000u;
    const u32 sz = 24;
    memcpy(m.data() + dir_off, &big, 4);
    memcpy(m.data() + dir_off + 4, &sz, 4);
    trt_image* img = nullptr;
    rc = trt_map_pe(&be, &rs, m.data(), m.size(), &img);
    CHECK(rc == TRT_ERR_TLS_BOUNDS, "insane TLS RVA -> TRT_ERR_TLS_BOUNDS");
    CHECK(img == nullptr, "no image leaked on insane TLS RVA");
  }

  trt_unmap(&be, exe_img);
  trt_unmap(&be, lib_img);
  g_lib32_img = nullptr;
  free(exe_file);
  free(lib_file);
  printf(failures ? "\nRESULT: %d FAILURES\n" : "\nRESULT: ALL PASS\n",
         failures);
  return failures ? 1 : 0;
}
