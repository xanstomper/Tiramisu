// tiramisu/tests/test_gdi.cpp
// The gdi32 bridge: a freestanding guest creates a font, selects it into
// a user32 DC, deletes it; the host verifies the record lifecycle through
// the shared log and the guest's return bits.
//
// Sprint 1A (GDI boot path): with no guest exe argument the test resolves
// the boot-path surface straight through the module table and drives it
// from the host via TRT_STDCALL (ms_abi) function pointers — the same
// handlers a game's gdi32! imports bind to.
#include "../src/trt/trt.h"
#include "../src/trt/gdi_font8x16.h"
#include "../src/trt/gdi_raster.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

extern "C" {
int trt_bridge_register_kernel32(void);
int trt_bridge_register_user32(void);
int trt_bridge_register_gdi(void);
}

namespace {

char g_log[256][256];
int g_log_count = 0;
bool log_contains(const char* needle) {
  for (int i = 0; i < g_log_count; ++i)
    if (strstr(g_log[i], needle)) return true;
  return false;
}
int log_count_of(const char* needle) {
  int n = 0;
  for (int i = 0; i < g_log_count; ++i)
    if (strstr(g_log[i], needle)) ++n;
  return n;
}

struct extent { void* base; size_t len; };
extent g_extents[16];
int g_extent_count = 0;

void* linux_alloc(size_t bytes, u64 preferred_base) {
  void* p = nullptr;
  if (preferred_base) {
    // ASan's shadow gap swallows the guest's usual preferred bases
    // (0x140000000 is INSIDE it — measured "unknown-crash" false positives):
    // under sanitizers, let the kernel pick and let the loader relocate.
#if defined(__SANITIZE_ADDRESS__) || (defined(__has_feature) && \
    (defined(__clang__) && __has_feature(address_sanitizer)))
    const uintptr_t gap_lo = 0x00008fff7000ull, gap_hi = 0x02008fff6fffull;
    if (preferred_base >= gap_lo && preferred_base <= gap_hi) preferred_base = 0;
#endif
    if (preferred_base)
      p = mmap((void*)(uintptr_t)preferred_base, bytes, PROT_READ | PROT_WRITE,
               MAP_FIXED_NOREPLACE | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  }
  if (!p || p == MAP_FAILED)
    p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) return nullptr;
  if (g_extent_count < 16) {
    g_extents[g_extent_count].base = p;
    g_extents[g_extent_count].len = bytes;
    ++g_extent_count;
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
void linux_release(void* base, size_t) {
  for (int i = 0; i < g_extent_count; ++i)
    if (g_extents[i].base == base) {
      munmap(base, g_extents[i].len);
      g_extents[i].base = nullptr;
      return;
    }
}
void linux_log(const char* line) {
  printf("  [trt] %s\n", line);
  if (g_log_count < 256)
    snprintf(g_log[g_log_count++], sizeof(g_log[0]), "%s", line);
}

u8* read_file(const char* path, size_t* size) {
  FILE* f = fopen(path, "rb");
  if (!f) return nullptr;
  fseek(f, 0, SEEK_END);
  const long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  u8* buf = (u8*)malloc((size_t)n);
  const size_t got = fread(buf, 1, (size_t)n, f);
  fclose(f);
  *size = got;
  return buf;
}

u64 win64_call(void* fn, u64 a, u64 b, u64 c) {
  u64 args[3] = {a, b, c};
  u64 ret;
  asm volatile(
    "movq %[a0], %%rcx\n"
    "movq %[a1], %%rdx\n"
    "movq %[a2], %%r8\n"
    "subq $48, %%rsp\n"
    "call *%[fn]\n"
    "addq $48, %%rsp\n"
    : "=a"(ret)
    : [a0] "m"(args[0]), [a1] "m"(args[1]), [a2] "m"(args[2]),
      [fn] "r"(fn)
    : "rcx", "rdx", "r8", "r9", "r10", "r11",
      "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "memory", "cc");
  return ret;
}

int failures = 0;
#define CHECK(cond, msg)                                             \
  do {                                                               \
    if (cond) printf("  PASS %s\n", msg);                            \
    else { printf("  FAIL %s\n", msg); ++failures; }                 \
  } while (0)

// Sprint 1A boot-path surface. Typedefs MUST carry TRT_STDCALL
// (ms_abi, src/trt/pe.h): a SysV call through one of these pointers
// enters the Win64 ABI exactly like the win64_call trampoline.
typedef void* (TRT_STDCALL* fn_getdc)(void*);
typedef int (TRT_STDCALL* fn_releasedc)(void*, void*);
typedef void* (TRT_STDCALL* fn_ccdc)(void*);
typedef void* (TRT_STDCALL* fn_ccbmp)(void*, int, int);
typedef void* (TRT_STDCALL* fn_stock)(int);
typedef void* (TRT_STDCALL* fn_select)(void*, void*);
typedef int (TRT_STDCALL* fn_delobj)(void*);
typedef int (TRT_STDCALL* fn_delete_dc)(void*);
typedef int (TRT_STDCALL* fn_blt9)(void*, int, int, int, int, void*, int, int,
                                   u32);
typedef int (TRT_STDCALL* fn_sblt)(void*, int, int, int, int, void*, int, int,
                                   int, int, u32);
typedef int (TRT_STDCALL* fn_caps)(void*, int);
typedef int (TRT_STDCALL* fn_setstretch)(void*, int);
typedef int (TRT_STDCALL* fn_getstretch)(void*);

void* resolve_gdi(const char* name) {
  return trt_resolve_module(nullptr, "gdi32.dll", name, 0);
}
void* resolve_user32(const char* name) {
  return trt_resolve_module(nullptr, "user32.dll", name, 0);
}
void* resolve_gdiplus(const char* name) {
  return trt_resolve_module(nullptr, "gdiplus.dll", name, 0);
}
void* resolve_msimg32(const char* name) {
  return trt_resolve_module(nullptr, "msimg32.dll", name, 0);
}

}  // namespace

int main(int argc, char** argv) {
  trt_backend be{&linux_alloc, &linux_protect, &linux_release, &linux_log,
                 nullptr};
  trt_bridge_boot(&be);
  trt_bridge_register_kernel32();
  trt_bridge_register_user32();
  trt_bridge_register_gdi();

  printf("tiramisu gdi32 bridge test\n");

  CHECK(trt_module_route("gdi32.dll") == TRT_ROUTE_BUILTIN,
        "gdi32.dll routes BUILTIN (no fake SYSTEM bind)");
  CHECK(trt_module_route("user32.dll") == TRT_ROUTE_BUILTIN,
        "user32.dll routes BUILTIN (shared DC space)");

  // ---- D3DKMT adapter queries (honest stubs, host-direct) ------------------
  {
    using Dkmt_t = i32(TRT_GUEST_ABI*)(void*);
    auto pOpen = (Dkmt_t)trt_resolve_module(nullptr, "gdi32.dll",
                                            "D3DKMTOpenAdapterFromLuid", 0);
    auto pClose = (Dkmt_t)trt_resolve_module(nullptr, "gdi32.dll",
                                             "D3DKMTCloseAdapter", 0);
    auto pQuery = (Dkmt_t)trt_resolve_module(nullptr, "gdi32.dll",
                                             "D3DKMTQueryAdapterInfo", 0);
    auto pEnum = (Dkmt_t)trt_resolve_module(nullptr, "gdi32.dll",
                                            "D3DKMTEnumAdapters", 0);
    CHECK(pOpen && pClose && pQuery && pEnum,
          "gdi32 exports the D3DKMT adapter-query surface");

    // D3DKMT_OPENADAPTERFROMLUID { LUID(8); ULONG hAdapter; }
    struct { u32 lo, hi, h; } oa{0x1337u, 0, 0u};
    CHECK(pOpen && pOpen(&oa) == 0 && oa.h == 0x44584B31u,
          "D3DKMTOpenAdapterFromLuid -> STATUS_SUCCESS + synthetic hAdapter");

    // D3DKMT_QUERYADAPTERINFO { ULONG hAdapter; ULONG Type; void* d; u32 n; }
    struct { u32 h, type; void* d; u32 n; } qa{0x44584B31u, 35u, nullptr, 0u};
    CHECK(pQuery && pQuery(&qa) == (i32)0xC0000002u,
          "D3DKMTQueryAdapterInfo -> STATUS_NOT_IMPLEMENTED (fail-visible)");

    // D3DKMT_ENUMADAPTERS { UINT NumAdapters; D3DKMT_ADAPTERINFO Adapters[16]; }
    u8 ea[8 + 16 * 24] = {};
    CHECK(pEnum && pEnum(ea) == 0 && *(u32*)ea == 1u &&
              *(u32*)(ea + 8 + 8) == 0x1337u,
          "D3DKMTEnumAdapters -> 1 Xbox APU adapter with the bridge LUID");

    struct { u32 h; } ca{0x44584B31u};
    CHECK(pClose && pClose(&ca) == 0, "D3DKMTCloseAdapter -> STATUS_SUCCESS");
  }

  // ---- guest-PE flow (existing coverage; needs a guest exe argument) ----
  if (argc >= 2) {
    size_t size = 0;
    u8* file = read_file(argv[1], &size);
    if (!file) { printf("cannot read %s\n", argv[1]); return 2; }

    trt_resolver rs;
    trt_resolver_default(&rs);
    trt_image* img = nullptr;
    const int rc = trt_map_pe(&be, &rs, file, size, &img);
    CHECK(rc == TRT_OK, "guest PE maps (gdi32 + user32 GetDC bound)");
    if (rc != TRT_OK || !img) {
      printf("  map failed: %s (%s)\n", trt_errstr(rc),
             img ? img->last_detail : "no image");
      free(file);
      printf("RESULT: %d FAIL\n", failures ? failures : 1);
      return 1;
    }
    CHECK(trt_seal_image(&be, img) == TRT_OK, "image sealed (W^X)");

    const u64 ret = win64_call(img->base + img->entry_rva, 0, 0, 0);
    printf("  guest returned 0x%llx (want 0xff)\n", (unsigned long long)ret);

    CHECK((ret & 0x01u) != 0, "GetDC handed out a real DC record");
    CHECK((ret & 0x02u) != 0, "CreateFontIndirectW returned a handle");
    CHECK((ret & 0x04u) != 0, "SelectObject recorded per-DC state");
    CHECK((ret & 0x08u) != 0, "DeleteObject freed the record (select-after fails)");
    CHECK((ret & 0x10u) != 0, "GetStockObject is stable");
    CHECK((ret & 0x20u) != 0, "stock object selected; ReleaseDC released");
    CHECK((ret & 0x40u) != 0, "garbage DC -> SelectObject NULL (fail-visible)");
    CHECK((ret & 0x80u) != 0, "SetBkMode/SetTextColor/SetBkColor recorded");

    // host-side lifecycle evidence in the log
    CHECK(log_contains("gdi32: CreateFontIndirectW h=") &&
              log_contains("lfHeight=-24"),
          "log names the created font with its lfHeight");
    CHECK(log_count_of("gdi32: DeleteObject") == 1,
          "exactly one DeleteObject (the freed font)");
    CHECK(log_contains("not a user32 DC -> NULL"),
          "the garbage-DC select was named");
    CHECK(log_contains("user32: GetDC") == false,  // GetDC itself is silent-ok
          "no misleading GetDC failure noise");

    trt_unmap(&be, img);
    free(file);
  } else {
    printf("  (no guest exe: module-table surface only)\n");
  }

  // ---- Sprint 1A: gdi32 boot path resolved through the module table ----
  CHECK(resolve_gdi("GetDC") != nullptr,
        "gdi32!GetDC resolves (shared user32 handler)");
  CHECK(resolve_gdi("ReleaseDC") != nullptr,
        "gdi32!ReleaseDC resolves (shared user32 handler)");
  CHECK(resolve_gdi("BitBlt") != nullptr, "gdi32!BitBlt resolves");
  CHECK(resolve_gdi("StretchBlt") != nullptr, "gdi32!StretchBlt resolves");
  CHECK(resolve_gdi("SetStretchBltMode") != nullptr,
        "gdi32!SetStretchBltMode resolves (binkw32 load path)");
  CHECK(resolve_gdi("GetStretchBltMode") != nullptr,
        "gdi32!GetStretchBltMode resolves");
  CHECK(resolve_gdi("CreateCompatibleDC") != nullptr,
        "gdi32!CreateCompatibleDC resolves");
  CHECK(resolve_gdi("CreateCompatibleBitmap") != nullptr,
        "gdi32!CreateCompatibleBitmap resolves");
  CHECK(resolve_gdi("GetStockObject") != nullptr,
        "gdi32!GetStockObject resolves");

  const fn_stock stock = (fn_stock)resolve_gdi("GetStockObject");
  if (stock) {
    void* white = stock(0);   // WHITE_BRUSH
    void* black = stock(4);   // BLACK_BRUSH
    void* nullb = stock(5);   // NULL_BRUSH
    void* oemf  = stock(10);  // OEM_FIXED_FONT
    void* pal   = stock(15);  // DEFAULT_PALETTE
    CHECK(white && black && nullb && oemf && pal,
          "GetStockObject WHITE/BLACK/NULL_BRUSH, OEM_FIXED_FONT, "
          "DEFAULT_PALETTE all non-NULL");
    CHECK(white == stock(0), "GetStockObject stable for the same id");
    const fn_delobj delobj = (fn_delobj)resolve_gdi("DeleteObject");
    CHECK(delobj && delobj(nullb) == 1,
          "DeleteObject on a stock handle is a no-op (TRUE)");
  } else {
    CHECK(false, "GetStockObject callable");
  }

  const fn_getdc getdc = (fn_getdc)resolve_gdi("GetDC");
  const fn_releasedc releasedc = (fn_releasedc)resolve_gdi("ReleaseDC");
  const fn_ccdc ccdc = (fn_ccdc)resolve_gdi("CreateCompatibleDC");
  const fn_ccbmp ccbmp = (fn_ccbmp)resolve_gdi("CreateCompatibleBitmap");
  const fn_blt9 bitblt = (fn_blt9)resolve_gdi("BitBlt");
  const fn_sblt sblt = (fn_sblt)resolve_gdi("StretchBlt");
  const fn_select selobj = (fn_select)resolve_gdi("SelectObject");
  const fn_delete_dc deldc = (fn_delete_dc)resolve_gdi("DeleteDC");
  if (getdc && releasedc && ccdc && ccbmp && bitblt && sblt && selobj && deldc) {
    void* hdc = getdc(nullptr);
    CHECK(hdc != nullptr, "gdi32!GetDC(NULL) handed out a DC record");
    const fn_setstretch setstretch =
        (fn_setstretch)resolve_gdi("SetStretchBltMode");
    const fn_getstretch getstretch =
        (fn_getstretch)resolve_gdi("GetStretchBltMode");
    if (setstretch && getstretch) {
      CHECK(getstretch(hdc) == 3,
            "stretch mode defaults to COLORONCOLOR (3)");
      CHECK(setstretch(hdc, 4) == 3,
            "SetStretchBltMode(HALFTONE) -> previous mode 3");
      CHECK(getstretch(hdc) == 4, "stretch mode round-trips (HALFTONE)");
      CHECK(setstretch(hdc, 9) == 0,
            "invalid stretch mode -> 0 (fail-visible)");
      CHECK(getstretch(hdc) == 4, "invalid set left the mode unchanged");
      CHECK(setstretch(nullptr, 3) == 0,
            "SetStretchBltMode on a garbage DC -> 0 (fail-visible)");
    } else {
      CHECK(false, "stretch-mode surface callable");
    }
    void* memdc = ccdc(hdc);
    CHECK(memdc != nullptr, "CreateCompatibleDC off the window DC");
    void* hbmp = ccbmp(memdc, 64, 64);
    CHECK(hbmp != nullptr, "CreateCompatibleBitmap 64x64 on the memory DC");
    CHECK(selobj(memdc, hbmp) != nullptr, "bitmap selected into the memory DC");
    CHECK(bitblt(memdc, 0, 0, 64, 64, hdc, 0, 0, 0x00CC0020u) == 1,
          "BitBlt SRCCOPY -> TRUE");
    CHECK(sblt(memdc, 0, 0, 32, 32, hdc, 0, 0, 64, 64, 0x00CC0020u) == 1,
          "StretchBlt SRCCOPY -> TRUE");
    CHECK(log_contains("gdi32: BitBlt 64x64") &&
              log_contains("gdi32: StretchBlt"),
          "blits name themselves in the log (fail-visible, never silent)");
    const fn_caps caps = (fn_caps)resolve_gdi("GetDeviceCaps");
    CHECK(caps && caps(hdc, 8) == 1920 && caps(hdc, 10) == 1080 &&
              caps(hdc, 12) == 32 && caps(hdc, 14) == 1,
          "GetDeviceCaps HORZRES/VERTRES/BITSPIXEL/PLANES plausible");

    using fn_otm = u32(TRT_GUEST_ABI*)(void*, u32, void*);
    const fn_otm otm_fn = (fn_otm)resolve_gdi("GetOutlineTextMetricsW");
    CHECK(otm_fn != nullptr, "gdi32!GetOutlineTextMetricsW resolves");
    if (otm_fn) {
      CHECK(otm_fn(hdc, 0, nullptr) == 216,
            "GetOutlineTextMetricsW(0, NULL) queries required size (216)");
      u8 otm_buf[256] = {};
      CHECK(otm_fn(hdc, 216, otm_buf) == 216,
            "GetOutlineTextMetricsW fills buffer");
      CHECK(*(u32*)otm_buf == 216,
            "GetOutlineTextMetricsW sets otmSize");
      CHECK(otm_fn(hdc, 2, otm_buf) == 0,
            "GetOutlineTextMetricsW refuses buffer smaller than 4 bytes");
    }

    CHECK(releasedc(nullptr, hdc) == 1, "gdi32!ReleaseDC released the window DC");
    CHECK(deldc(memdc) == 1, "DeleteDC released the memory DC");

    // ---- Palettes & DIB color tables (former gaps -> real state) ---------
    {
      using fn_crpal = void*(TRT_GUEST_ABI*)(const void*);
      using fn_selpal = void*(TRT_GUEST_ABI*)(void*, void*, int);
      using fn_realpal = u32(TRT_GUEST_ABI*)(void*);
      using fn_palent = u32(TRT_GUEST_ABI*)(void*, u32, u32, void*);
      using fn_dibct = u32(TRT_GUEST_ABI*)(void*, u32, u32, const void*);
      using fn_getdibct = u32(TRT_GUEST_ABI*)(void*, u32, u32, void*);
      using fn_crdib = void*(TRT_GUEST_ABI*)(void*, const void*, u32, void**, void*, u32);
      auto p_crpal = (fn_crpal)resolve_gdi("CreatePalette");
      auto p_selpal = (fn_selpal)resolve_gdi("SelectPalette");
      auto p_realpal = (fn_realpal)resolve_gdi("RealizePalette");
      auto p_getent = (fn_palent)resolve_gdi("GetPaletteEntries");
      auto p_setdibct = (fn_dibct)resolve_gdi("SetDIBColorTable");
      auto p_getdibct = (fn_getdibct)resolve_gdi("GetDIBColorTable");
      auto p_crdib = (fn_crdib)resolve_gdi("CreateDIBSection");
      CHECK(p_crpal && p_selpal && p_realpal && p_setdibct, "palette + DIB color table exports resolve");
      if (p_crpal && p_selpal && p_realpal && p_getent && p_setdibct && p_getdibct && p_crdib) {
        u8 logpal[4 + 2 * 4] = {};
        *(u16*)(logpal + 0) = 0x0300;
        *(u16*)(logpal + 2) = 2;
        *(u32*)(logpal + 4) = 0x00FF0000u;
        *(u32*)(logpal + 8) = 0x0000FF00u;
        void* hpal = p_crpal(logpal);
        CHECK(hpal != nullptr, "CreatePalette returns real HPALETTE handle");
        u32 entries[2] = {};
        CHECK(p_getent(hpal, 0, 2, entries) == 2 && entries[0] == 0x00FF0000u,
              "GetPaletteEntries reads stored entries");
        void* pdc = ccdc(nullptr);
        void* prev = p_selpal(pdc, hpal, 0);
        CHECK(prev != nullptr && p_realpal(pdc) == 2,
              "SelectPalette + RealizePalette reports 2 realized entries");
        u8 bmi[40] = {};
        *(u32*)(bmi + 0) = 40;
        *(i32*)(bmi + 4) = 8;
        *(i32*)(bmi + 8) = 8;
        *(u16*)(bmi + 12) = 1;
        *(u16*)(bmi + 14) = 8;
        void* dib = p_crdib(pdc, bmi, 0, nullptr, nullptr, 0);
        selobj(pdc, dib);
        u32 ct_in[2] = {0x11223344u, 0x55667788u};
        u32 ct_out[2] = {};
        CHECK(p_setdibct(pdc, 0, 2, ct_in) == 2 && p_getdibct(pdc, 0, 2, ct_out) == 2 &&
                  ct_out[1] == 0x55667788u,
              "SetDIBColorTable/GetDIBColorTable round-trips");
        deldc(pdc);
        const fn_delobj del = (fn_delobj)resolve_gdi("DeleteObject");
        if (del) { del(dib); del(hpal); }
      }
    }

    const fn_delobj delobj2 = (fn_delobj)resolve_gdi("DeleteObject");
    if (delobj2) delobj2(hbmp);  // free the compatible bitmap record
  } else {
    CHECK(false, "boot-path DC/blit surface callable");
  }

  // ---- Desktop text surface: state, metrics, ANSI variants, charset maps ----
  // Every ANSI desktop app (Putty included) drives these; before this suite
  // they were synthesized fail-visible answers or fixed 8x16 constants.
  {
    using fn_getdc = void*(TRT_GUEST_ABI*)(void*);
    using fn_releasedc = i32(TRT_GUEST_ABI*)(void*, void*);
    using fn_crfont = void*(TRT_GUEST_ABI*)(const void*);
    using fn_selobj = void*(TRT_GUEST_ABI*)(void*, void*);
    using fn_ii = i32(TRT_GUEST_ABI*)(void*, i32);
    using fn_i_i = i32(TRT_GUEST_ABI*)(void*);
    using fn_uu = u32(TRT_GUEST_ABI*)(void*, u32);
    using fn_u = u32(TRT_GUEST_ABI*)(void*);
    using fn_getcur = void*(TRT_GUEST_ABI*)(void*, u32);
    using fn_ext = i32(TRT_GUEST_ABI*)(void*, const u16*, i32, void*);
    using fn_exta = i32(TRT_GUEST_ABI*)(void*, const char*, i32, void*);
    using fn_exex = i32(TRT_GUEST_ABI*)(void*, const u16*, i32, i32, i32*, i32*, void*);
    using fn_cw = i32(TRT_GUEST_ABI*)(void*, u32, u32, i32*);
    using fn_tm = i32(TRT_GUEST_ABI*)(void*, void*);
    using fn_tci = i32(TRT_GUEST_ABI*)(void*, void*, u32);
    using fn_etoa = i32(TRT_GUEST_ABI*)(void*, i32, i32, u32, const void*,
                                        const char*, u32, const i32*);
    using fn_getobj = i32(TRT_GUEST_ABI*)(void*, i32, void*);

    auto p_getdc = (fn_getdc)resolve_gdi("GetDC");
    auto p_rel = (fn_releasedc)resolve_gdi("ReleaseDC");
    auto p_crfont = (fn_crfont)resolve_gdi("CreateFontIndirectW");
    auto p_sel = (fn_selobj)resolve_gdi("SelectObject");
    auto p_setbkmode = (fn_ii)resolve_gdi("SetBkMode");
    auto p_getbkmode = (fn_i_i)resolve_gdi("GetBkMode");
    auto p_settc = (fn_uu)resolve_gdi("SetTextColor");
    auto p_setbc = (fn_uu)resolve_gdi("SetBkColor");
    auto p_setta = (fn_uu)resolve_gdi("SetTextAlign");
    auto p_getta = (fn_u)resolve_gdi("GetTextAlign");
    auto p_getcur = (fn_getcur)resolve_gdi("GetCurrentObject");
    auto p_extentw = (fn_ext)resolve_gdi("GetTextExtentPoint32W");
    auto p_extenta = (fn_exta)resolve_gdi("GetTextExtentPoint32A");
    auto p_exexw = (fn_exex)resolve_gdi("GetTextExtentExPointW");
    auto p_charw = (fn_cw)resolve_gdi("GetCharWidth32W");
    auto p_tmw = (fn_tm)resolve_gdi("GetTextMetricsW");
    auto p_tci = (fn_tci)resolve_gdi("TranslateCharsetInfo");
    auto p_etoa = (fn_etoa)resolve_gdi("ExtTextOutA");
    auto p_getobj = (fn_getobj)resolve_gdi("GetObjectA");

    {
      // Name the missing export instead of a bare boolean: a typo'd or
      // unregistered symbol is the whole failure mode this suite guards.
      const struct { const char* n; const void* p; } ex[] = {
          {"GetDC", (const void*)p_getdc},
          {"ReleaseDC", (const void*)p_rel},
          {"CreateFontIndirectW", (const void*)p_crfont},
          {"SelectObject", (const void*)p_sel},
          {"SetBkMode", (const void*)p_setbkmode},
          {"GetBkMode", (const void*)p_getbkmode},
          {"SetTextColor", (const void*)p_settc},
          {"SetBkColor", (const void*)p_setbc},
          {"SetTextAlign", (const void*)p_setta},
          {"GetTextAlign", (const void*)p_getta},
          {"GetCurrentObject", (const void*)p_getcur},
          {"GetTextExtentPoint32W", (const void*)p_extentw},
          {"GetTextExtentPoint32A", (const void*)p_extenta},
          {"GetTextExtentExPointW", (const void*)p_exexw},
          {"GetCharWidth32W", (const void*)p_charw},
          {"GetTextMetricsW", (const void*)p_tmw},
          {"TranslateCharsetInfo", (const void*)p_tci},
          {"ExtTextOutA", (const void*)p_etoa},
          {"GetObjectA", (const void*)p_getobj},
      };
      bool all = true;
      for (const auto& e : ex)
        if (!e.p) { printf("  MISSING export: %s\n", e.n); all = false; }
      CHECK(all,
            "text/state exports resolve (GetBkMode, SetTextAlign, ExtTextOutA, "
            "GetCharWidth32W, GetTextExtentExPointW, TranslateCharsetInfo)");
    }

    if (p_getdc && p_setbkmode && p_getbkmode && p_setta && p_getta &&
        p_getcur && p_extentw && p_exexw && p_charw && p_tci && p_etoa &&
        p_getobj && p_crfont && p_sel && p_settc && p_setbc && p_tmw &&
        p_extenta) {
      void* dc = p_getdc(nullptr);
      CHECK(dc != nullptr, "fresh DC for the text suite");

      CHECK(p_getbkmode(dc) == 2, "fresh DC defaults to OPAQUE background");
      CHECK(p_setbkmode(dc, 1) == 2 && p_getbkmode(dc) == 1,
            "SetBkMode stores TRANSPARENT and returns the old OPAQUE");
      CHECK(p_setbkmode(dc, 7) == 0 && p_getbkmode(dc) == 1,
            "SetBkMode(7) is refused and does not change the stored mode");
      CHECK(p_settc(dc, 0x0000FFu) == 0x000000u,
            "SetTextColor returns the previous black");
      CHECK(p_setbc(dc, 0x123456u) == 0xFFFFFFu,
            "SetBkColor returns the previous white");
      CHECK(p_setta(dc, 6) == 0 && p_getta(dc) == 6,
            "SetTextAlign stores TA_CENTER|TA_TOP and returns the old TA_LEFT");
      CHECK(p_setta(dc, 0x8000u) == 0xFFFFFFFFu && p_getta(dc) == 6,
            "SetTextAlign with an unknown bit -> GDI_ERROR, state unchanged");

      u8 lf[92] = {};
      *(i32*)(lf + 0) = -32;  // lfHeight: 32 px cell
      *(i32*)(lf + 20) = 400; // lfWeight
      const char* face = "Arial";
      for (int i = 0; i < 5; ++i) ((u16*)(lf + 28))[i] = (u16)(u8)face[i];
      void* font = p_crfont(lf);
      CHECK(font != nullptr, "CreateFontIndirectW(-32) for the metrics suite");
      CHECK(p_sel(dc, font) != nullptr, "font selects into the DC");
      CHECK(p_getcur(dc, 6) == font, "GetCurrentObject(OBJ_FONT) -> the font");

      i32 sz[2] = {0, 0};
      const u16 abc[] = {'A', 'B', 'C'};
      CHECK(p_extentw(dc, abc, 3, sz) == 1 && sz[0] == 48 && sz[1] == 32,
            "GetTextExtentPoint32W(3 chars, 32px font) == 48x32 (not 8x16)");
      i32 sza[2] = {0, 0};
      CHECK(p_extenta(dc, "ABC", 3, sza) == 1 && sza[0] == 48 && sza[1] == 32,
            "GetTextExtentPoint32A measures the same cell");

      i32 fit = -1;
      i32 dx[10] = {};
      i32 full[2] = {};
      const u16 ten[] = {'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J'};
      CHECK(p_exexw(dc, ten, 10, 50, &fit, dx, full) == 1 && fit == 3 &&
                dx[9] == 160 && full[0] == 160,
            "GetTextExtentExPointW fits 3/10 chars in 50px, extent stays 160");

      i32 widths[3] = {};
      CHECK(p_charw(dc, 'A', 'C', widths) == 1 && widths[0] == 16 &&
                widths[2] == 16,
            "GetCharWidth32W returns one advance per character");

      u8 tm[60] = {};
      CHECK(p_tmw(dc, tm) == 1 && *(i32*)(tm + 0) == 32 &&
                *(i32*)(tm + 20) == 16,
            "GetTextMetricsW follows the selected font (height 32, ave 16)");

      u8 logfonta[60] = {};
      CHECK(p_getobj(font, 60, logfonta) == 60 &&
                logfonta[28] == 'A' && logfonta[31] == 'a',
            "GetObjectA(font) -> LOGFONTA with the face name");

      u8 cs[32] = {};
      CHECK(p_tci((void*)(uintptr_t)1252, cs, 2) == 1 &&
                *(u32*)(cs + 0) == 0 && *(u32*)(cs + 4) == 1252,
            "TranslateCharsetInfo(CP1252) -> ANSI charset / 1252");
      u8 cs2[32] = {};
      CHECK(p_tci((void*)(uintptr_t)0xDEAD, cs2, 2) == 0,
            "TranslateCharsetInfo(unknown codepage) -> FALSE, no guess");

      CHECK(p_etoa(dc, 0, 0, 0, nullptr, "Hello", 5, nullptr) == 1,
            "ExtTextOutA accepts an ANSI string (no synth path, no crash)");

      if (p_rel) p_rel(nullptr, dc);
    }
  }

  // ---- GDI raster surface: drawing must produce pixels --------------------
  // A desktop app paints through this surface (TextOut -> glyphs, PatBlt ->
  // fill, BitBlt -> copy). Before it existed every one of these answered
  // "not drawn", so "the app booted" proved nothing about what it showed.
  {
    using fn_getdc = void*(TRT_GUEST_ABI*)(void*);
    using fn_releasedc = i32(TRT_GUEST_ABI*)(void*, void*);
    using fn_getpix = u32(TRT_GUEST_ABI*)(void*, i32, i32);
    using fn_setpix = u32(TRT_GUEST_ABI*)(void*, i32, i32, u32);
    using fn_patblt = i32(TRT_GUEST_ABI*)(void*, i32, i32, i32, i32, u32);
    using fn_rect = i32(TRT_GUEST_ABI*)(void*, i32, i32, i32, i32);
    using fn_moveto = i32(TRT_GUEST_ABI*)(void*, i32, i32, void*);
    using fn_lineto = i32(TRT_GUEST_ABI*)(void*, i32, i32);
    using fn_brush = void*(TRT_GUEST_ABI*)(u32);
    using fn_pen = void*(TRT_GUEST_ABI*)(i32, i32, u32);
    using fn_textouta = i32(TRT_GUEST_ABI*)(void*, i32, i32, const char*, i32);
    using fn_setu = u32(TRT_GUEST_ABI*)(void*, u32);
    using fn_cliprect = i32(TRT_GUEST_ABI*)(void*, i32, i32, i32, i32);
    using fn_savedc = i32(TRT_GUEST_ABI*)(void*);
    using fn_restoredc = i32(TRT_GUEST_ABI*)(void*, i32);
    using fn_i2 = i32(TRT_GUEST_ABI*)(void*, i32);
    using fn_dib2d = i32(TRT_GUEST_ABI*)(void*, i32, i32, u32, u32, i32, i32,
                                         u32, u32, const void*, const void*, u32);
    using fn_sdib = i32(TRT_GUEST_ABI*)(void*, i32, i32, i32, i32, i32, i32,
                                        i32, i32, const void*, const void*,
                                        u32, u32);
    using fn_bitblt = i32(TRT_GUEST_ABI*)(void*, i32, i32, i32, i32, void*,
                                          i32, i32, u32);
    using fn_stblt = i32(TRT_GUEST_ABI*)(void*, i32, i32, i32, i32, void*,
                                         i32, i32, i32, i32, u32);
    using fn_ext2 = i32(TRT_GUEST_ABI*)(void*, const char*, i32, void*);

    auto g_getdc = (fn_getdc)resolve_gdi("GetDC");
    auto g_rel = (fn_releasedc)resolve_gdi("ReleaseDC");
    auto g_getpix = (fn_getpix)resolve_gdi("GetPixel");
    auto g_setpix = (fn_setpix)resolve_gdi("SetPixel");
    auto g_patblt = (fn_patblt)resolve_gdi("PatBlt");
    auto g_rect = (fn_rect)resolve_gdi("Rectangle");
    auto g_moveto = (fn_moveto)resolve_gdi("MoveToEx");
    auto g_lineto = (fn_lineto)resolve_gdi("LineTo");
    auto g_brush = (fn_brush)resolve_gdi("CreateSolidBrush");
    auto g_pen = (fn_pen)resolve_gdi("CreatePen");
    auto g_textouta = (fn_textouta)resolve_gdi("TextOutA");
    auto g_settc = (fn_setu)resolve_gdi("SetTextColor");
    auto g_setbc = (fn_setu)resolve_gdi("SetBkColor");
    auto g_setbkmode = (fn_i2)resolve_gdi("SetBkMode");
    auto g_clip = (fn_cliprect)resolve_gdi("IntersectClipRect");
    auto g_savedc = (fn_savedc)resolve_gdi("SaveDC");
    auto g_restoredc = (fn_restoredc)resolve_gdi("RestoreDC");
    auto g_ccdc = (void*(TRT_GUEST_ABI*)(void*))resolve_gdi("CreateCompatibleDC");
    auto g_bitblt = (fn_bitblt)resolve_gdi("BitBlt");
    auto g_sblt = (fn_stblt)resolve_gdi("StretchBlt");
    auto g_sel = (void*(TRT_GUEST_ABI*)(void*, void*))resolve_gdi("SelectObject");
    auto g_dib2d = (fn_dib2d)resolve_gdi("SetDIBitsToDevice");
    auto g_sdib = (fn_sdib)resolve_gdi("StretchDIBits");
    auto g_exta = (fn_ext2)resolve_gdi("GetTextExtentPoint32A");
    auto g_crfont = (void*(TRT_GUEST_ABI*)(const void*))resolve_gdi(
        "CreateFontIndirectW");

    const bool have = g_getdc && g_rel && g_getpix && g_setpix && g_patblt &&
                      g_rect && g_moveto && g_lineto && g_brush && g_pen &&
                      g_textouta && g_settc && g_setbc && g_setbkmode &&
                      g_clip && g_savedc && g_restoredc && g_ccdc &&
                      g_bitblt && g_sblt && g_sel && g_dib2d && g_sdib &&
                      g_exta && g_crfont;
    CHECK(have, "raster surface exports resolve (draw, blit, DIB, clip, "
                "SaveDC/RestoreDC)");
    if (have) {
      void* dc = g_getdc(nullptr);
      // Background: opaque white over the whole surface.
      CHECK(g_patblt(dc, 0, 0, 1920, 1080, 0x00F00021u) == 1,
            "PatBlt(PATCOPY) fills with the white stock brush");
      CHECK(g_getpix(dc, 0, 0) == 0x00FFFFFFu,
            "GetPixel reads back the filled white");

      CHECK(g_setpix(dc, 10, 10, 0x0000FFu) == 0x0000FFu &&
                g_getpix(dc, 10, 10) == 0x0000FFu,
            "SetPixel/GetPixel round-trip COLORREF 0x0000FF");
      CHECK(g_getpix(dc, 5000, 5000) == 0xFFFFFFFFu,
            "GetPixel outside the surface -> CLR_INVALID");

      // Text: opaque background + glyph coverage in the text colour.
      g_setbc(dc, 0x00FFFFFFu);
      g_settc(dc, 0x00FF0000u);  // blue in COLORREF (0x00BBGGRR)
      g_setbkmode(dc, 2);
      CHECK(g_textouta(dc, 40, 40, "Wg", 2) == 1,
            "TextOutA draws into the DC surface");
      i64 ink_blue = 0, ink_bg = 0;
      for (i32 y = 40; y < 56; ++y) {
        for (i32 x = 40; x < 56; ++x) {
          const u32 c = g_getpix(dc, x, y);
          if (c == 0x00FF0000u) ++ink_blue;
          if (c != 0x00FFFFFFu) ++ink_bg;
        }
      }
      CHECK(ink_blue > 0 && ink_bg > ink_blue,
            "glyph pixels use the DC text colour and blend coverage "
            "(not a solid block)");

      // A brush selection must not change text metrics (real per-type state).
      u8 lf[92] = {};
      *(i32*)(lf + 0) = -32;
      void* font32 = g_crfont(lf);
      g_sel(dc, font32);
      i32 szm[2] = {};
      g_exta(dc, "ABC", 3, szm);
      void* brush = g_brush(0x0000FF00u);
      g_sel(dc, brush);
      i32 szm2[2] = {};
      g_exta(dc, "ABC", 3, szm2);
      CHECK(szm[0] == 48 && szm2[0] == 48 && szm2[1] == 32,
            "selecting a brush leaves the font's metrics alone");

      // Clip rectangle really confines drawing.
      bool cl = false;
      cl = g_clip(dc, 600, 600, 620, 620) == 2;
      g_patblt(dc, 590, 590, 60, 60, 0x00000042u);  // BLACKNESS
      CHECK(cl && g_getpix(dc, 610, 610) == 0x00000000u &&
                g_getpix(dc, 590, 590) == 0x00FFFFFFu &&
                g_getpix(dc, 640, 640) == 0x00FFFFFFu,
            "IntersectClipRect confines PatBlt to the clip rectangle");
      // IntersectClipRect can only shrink the clip; a NULL region restores
      // the full surface (that is how a repaint after scroll starts over).
      auto g_selrgn = (i32(TRT_GUEST_ABI*)(void*, void*))resolve_gdi(
          "SelectClipRgn");
      CHECK(g_selrgn && g_selrgn(dc, nullptr) == 2,
            "SelectClipRgn(NULL) restores the full-surface clip");

      // Rectangle fills with the selected brush and frames with the pen.
      g_sel(dc, brush);  // 0x0000FF00 green
      CHECK(g_rect(dc, 700, 700, 740, 740) == 1,
            "Rectangle draws with the selected brush");
      CHECK(g_getpix(dc, 720, 720) == 0x0000FF00u,
            "Rectangle interior is the brush colour (COLORREF green)");

      // FillRect (user32, brush passed as an argument) must paint too: dialogs
      // and controls repaint themselves with it.
      auto g_fillrect = (i32(TRT_GUEST_ABI*)(void*, const void*, void*))
          resolve_user32("FillRect");
      i32 fr[4] = {760, 700, 820, 740};
      auto g_brush2 = (fn_brush)resolve_gdi("CreateSolidBrush");
      void* fbrush = g_brush2 ? g_brush2(0x00FF0000u) : nullptr;  // blue
      CHECK(g_fillrect && fbrush && g_fillrect(dc, fr, fbrush) == 1 &&
                g_getpix(dc, 800, 720) == 0x00FF0000u,
            "FillRect paints the rectangle in the given brush");

      // DrawText layout: single line, and DT_CALCRECT sizing.
      auto g_drawtext = (i32(TRT_GUEST_ABI*)(void*, const char*, i32, void*,
                                            u32))resolve_user32("DrawTextA");
      i32 dr[4] = {40, 120, 400, 180};
      const i32 th = g_drawtext ? g_drawtext(dc, "Desktop", 7, dr, 0x20u) : -1;
      i32 ink = 0;
      for (i32 y = 120; y < 180; ++y)
        for (i32 x = 40; x < 400; ++x)
          if (g_getpix(dc, x, y) != 0x00FFFFFFu) ++ink;
      CHECK(th == 32 && ink > 0,
            "DrawTextA draws a single line and reports the 32px cell height");
      i32 cr[4] = {0, 0, 0, 0};
      const i32 ch = g_drawtext ? g_drawtext(dc, "AB", 2, cr, 0x420u) : -1;
      CHECK(ch == 32 && cr[2] == 32 && cr[3] == 32,
            "DrawTextA(DT_CALCRECT) measures 2 chars of the selected 32px "
            "font");

      // LineTo/MoveToEx paints a one-pixel pen line.
      void* pen = g_pen(0, 1, 0x000000FFu);  // red
      g_sel(dc, pen);
      CHECK(g_moveto(dc, 800, 800, nullptr) == 1 &&
                g_lineto(dc, 830, 800) == 1,
            "MoveToEx/LineTo accept the line");
      CHECK(g_getpix(dc, 815, 800) == 0x000000FFu,
            "LineTo pixels carry the pen colour (COLORREF red)");

      // BitBlt copies real pixels between DCs.
      void* mem = g_ccdc(dc);
      g_patblt(mem, 0, 0, 16, 16, 0x00000042u);  // blacken the source
      CHECK(g_bitblt(dc, 900, 900, 16, 16, mem, 0, 0, 0x00CC0020u) == 1,
            "BitBlt SRCCOPY returns TRUE");
      CHECK(g_getpix(dc, 905, 905) == 0x00000000u &&
                g_getpix(dc, 930, 930) == 0x00FFFFFFu,
            "BitBlt copied the source block and nothing outside it");
      CHECK(g_sblt(dc, 940, 940, 32, 32, mem, 0, 0, 16, 16, 0x00CC0020u) == 1 &&
                g_getpix(dc, 955, 955) == 0x00000000u,
            "StretchBlt scales the source block into the destination");

      // StretchDIBits decodes and scales a 2x2 24bpp DIB to 16x16.
      u8 bmi[40] = {};
      *(u32*)(bmi + 0) = 40;
      *(i32*)(bmi + 4) = 2;
      *(i32*)(bmi + 8) = 2;
      *(u16*)(bmi + 12) = 1;
      *(u16*)(bmi + 14) = 24;
      // 24bpp rows are padded to 4 bytes: 2 px = 6 bytes + 2 pad = 8/row.
      u8 bits[16] = {
          0x00, 0x00, 0xFF, 0x00, 0x00, 0xFF, 0x00, 0x00,  // BGR row
          0x00, 0xFF, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00,  // BGR row
      };
      CHECK(g_dib2d(dc, 1000, 1000, 2, 2, 0, 0, 0, 2, bits, bmi, 0) == 2,
            "SetDIBitsToDevice decodes a 24bpp BI_RGB DIB");
      CHECK(g_sdib(dc, 1020, 1000, 16, 16, 0, 0, 2, 2, bits, bmi, 0,
                   0x00CC0020u) == 2,
            "StretchDIBits scales the DIB to 16x16");
      CHECK(g_getpix(dc, 1025, 1005) != 0x00FFFFFFu,
            "StretchDIBits wrote pixels into the DC surface");

      // SaveDC/RestoreDC restore text state for real.
      g_setbkmode(dc, 2);
      const i32 lvl = g_savedc(dc);
      g_setbkmode(dc, 1);
      CHECK(lvl > 0 && g_restoredc(dc, -1) == 1,
            "SaveDC returns a level and RestoreDC(-1) pops it");
      // and an unmatched restore is a named FALSE, never a silent success
      CHECK(g_restoredc(dc, -1) == 0,
            "RestoreDC with nothing saved -> FALSE");

      // ---- RoundRect / Arc / Pie / Chord are real geometry ----------
      g_setbkmode(dc, 1);
      g_sel(dc, nullptr);
      void* green_brush = g_brush ? g_brush(0x0000FF00u) : nullptr;
      void* red_pen = g_pen ? g_pen(0, 1, 0x000000FFu) : nullptr;
      if (green_brush) g_sel(dc, green_brush);
      if (red_pen) g_sel(dc, red_pen);
      auto g_roundrect = (i32(TRT_GUEST_ABI*)(void*, i32, i32, i32, i32, i32,
                                              i32))resolve_gdi("RoundRect");
      CHECK(g_roundrect && g_roundrect(dc, 1500, 100, 1700, 260, 40, 40) == 1,
            "RoundRect accepts the call");
      CHECK(g_getpix(dc, 1502, 102) == 0x00FFFFFFu &&
                g_getpix(dc, 1600, 180) == 0x0000FF00u,
            "RoundRect leaves the corner empty and fills the middle (rounded)");

      auto g_arc = (i32(TRT_GUEST_ABI*)(void*, i32, i32, i32, i32, i32, i32,
                                        i32, i32))resolve_gdi("Arc");
      CHECK(g_arc && g_arc(dc, 1500, 300, 1700, 500, 1500, 400, 1700, 400) == 1,
            "Arc accepts the radial points");
      // Half ellipse from left-middle to right-middle: y-down screen, so the
      // counter-clockwise half is the top one.
      bool arc_pixel = false;
      for (i32 y = 300; y <= 340 && !arc_pixel; ++y)
        for (i32 x = 1560; x <= 1640 && !arc_pixel; ++x)
          if (g_getpix(dc, x, y) == 0x000000FFu) arc_pixel = true;
      CHECK(arc_pixel, "Arc painted the top half of its bounding ellipse");

      auto g_pie = (i32(TRT_GUEST_ABI*)(void*, i32, i32, i32, i32, i32, i32,
                                        i32, i32))resolve_gdi("Pie");
      if (green_brush) g_sel(dc, green_brush);
      CHECK(g_pie && g_pie(dc, 1720, 300, 1860, 440, 1720, 370, 1860, 370) == 1,
            "Pie accepts the radial points");
      CHECK(g_getpix(dc, 1790, 340) == 0x0000FF00u,
            "Pie filled the wedge between its radii");
      auto g_chord = (i32(TRT_GUEST_ABI*)(void*, i32, i32, i32, i32, i32, i32,
                                          i32, i32))resolve_gdi("Chord");
      CHECK(g_chord && g_chord(dc, 1720, 500, 1860, 640, 1720, 570, 1860, 570) ==
                            1,
            "Chord accepts the radial points");

      // ---- GDI paths: record, fill, stroke, clip --------------------
      auto g_beginpath = (i32(TRT_GUEST_ABI*)(void*))resolve_gdi("BeginPath");
      auto g_endpath = (i32(TRT_GUEST_ABI*)(void*))resolve_gdi("EndPath");
      auto g_fillpath = (i32(TRT_GUEST_ABI*)(void*))resolve_gdi("FillPath");
      auto g_strokepath = (i32(TRT_GUEST_ABI*)(void*))resolve_gdi("StrokePath");
      auto g_selclippath = (i32(TRT_GUEST_ABI*)(void*, i32))
          resolve_gdi("SelectClipPath");
      auto g_getpath = (i32(TRT_GUEST_ABI*)(void*, void*, void*, i32))
          resolve_gdi("GetPath");
      auto g_setfillmode = (i32(TRT_GUEST_ABI*)(void*, i32))
          resolve_gdi("SetPolyFillMode");
      auto g_pathtorgn = (i32(TRT_GUEST_ABI*)(void*))resolve_gdi("PathToRegion");

      // Recording intercepts the paint calls: nothing lands until replay.
      CHECK(g_beginpath && g_beginpath(dc) == 1 &&
                g_rect(dc, 1100, 100, 1200, 200) == 1 && g_endpath &&
                g_endpath(dc) == 1,
            "BeginPath/Rectangle/EndPath record the path");
      CHECK(g_getpix(dc, 1150, 150) == 0x00FFFFFFu,
            "the recorded rectangle painted nothing before replay");
      CHECK(g_fillpath && g_fillpath(dc) == 1 &&
                g_getpix(dc, 1150, 150) == 0x0000FF00u,
            "FillPath filled the recorded rectangle with the brush");

      // StrokePath strokes an open polyline with the pen.
      g_beginpath(dc);
      g_moveto(dc, 1250, 100, nullptr);
      g_lineto(dc, 1250, 200);
      g_endpath(dc);
      CHECK(g_strokepath && g_strokepath(dc) == 1 &&
                g_getpix(dc, 1250, 150) == 0x000000FFu,
            "StrokePath stroked the recorded polyline with the pen");

      // GetPath reports the vertices and their PT_* types.
      i32 pts[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
      u8 types[8] = {0, 0, 0, 0, 0, 0, 0, 0};
      const i32 nv = g_getpath ? g_getpath(dc, pts, types, 8) : -1;
      CHECK(nv == 2 && pts[0] == 1250 && pts[1] == 100 && types[0] == 6 &&
                types[1] == 2,
            "GetPath returns the vertices with PT_MOVETO/PT_LINETO types");

      // Nested rectangles wound the same way: ALTERNATE leaves a hole,
      // WINDING fills straight over it.
      auto nested = [&]() {
        g_beginpath(dc);
        g_rect(dc, 1300, 100, 1400, 200);
        g_rect(dc, 1320, 120, 1380, 180);
        g_endpath(dc);
      };
      nested();
      g_setfillmode(dc, 1);
      g_fillpath(dc);
      const bool hole = g_getpix(dc, 1350, 150) == 0x00FFFFFFu;
      nested();
      g_setfillmode(dc, 2);
      g_fillpath(dc);
      const bool wound = g_getpix(dc, 1350, 150) == 0x0000FF00u;
      g_setfillmode(dc, 1);
      CHECK(hole && wound,
            "ALTERNATE leaves the nested hole, WINDING fills through it");

      // SelectClipPath(RGN_AND) clips later drawing to the path pixels.
      g_beginpath(dc);
      g_rect(dc, 1100, 300, 1200, 400);
      g_endpath(dc);
      CHECK(g_selclippath && g_selclippath(dc, 1) == 1,
            "SelectClipPath(RGN_AND) accepts the recorded rectangle");
      g_patblt(dc, 1000, 250, 300, 200, 0x00000042u);
      CHECK(g_getpix(dc, 1150, 350) == 0x00000000u &&
                g_getpix(dc, 1250, 350) == 0x00FFFFFFu,
            "the path clip kept the inside and rejected the outside");
      g_selrgn(dc, nullptr);  // SelectClipRgn restores the full clip again
      g_patblt(dc, 1000, 250, 300, 200, 0x00FF0062u);
      CHECK(g_getpix(dc, 1250, 350) == 0x00FFFFFFu,
            "SelectClipRgn(NULL) dropped the path clip mask");

      g_beginpath(dc);
      g_rect(dc, 1100, 500, 1200, 600);
      g_endpath(dc);
      CHECK(g_pathtorgn && g_pathtorgn(dc) != 0,
            "PathToRegion returns a region handle for the recorded path");

      // ---- GDI+ renders on the same DC surface ----------------------
      auto gp_start = (i32(TRT_GUEST_ABI*)(u64*, const void*, void*))
          resolve_gdiplus("GdiplusStartup");
      auto gp_fromhdc = (i32(TRT_GUEST_ABI*)(void*, void**))
          resolve_gdiplus("GdipCreateFromHDC");
      auto gp_fill = (i32(TRT_GUEST_ABI*)(u32, void**))resolve_gdiplus(
          "GdipCreateSolidFill");
      auto gp_fillrect = (i32(TRT_GUEST_ABI*)(void*, void*, i32, i32, i32, i32))
          resolve_gdiplus("GdipFillRectangleI");
      u64 gp_token = 0;
      void* gp = nullptr;
      void* gp_brush = nullptr;
      CHECK(gp_start && gp_start(&gp_token, nullptr, nullptr) == 0,
            "GdiplusStartup -> Ok");
      CHECK(gp_fromhdc && gp_fromhdc(dc, &gp) == 0 && gp,
            "GdipCreateFromHDC binds a Graphics to the DC");
      // 50% blue over white must blend, not overwrite: modern UI fades do this.
      CHECK(gp_fill && gp_fill(0x800000FFu, &gp_brush) == 0 && gp_brush,
            "GdipCreateSolidFill keeps an ARGB brush");
      CHECK(gp_fillrect && gp_fillrect(gp, gp_brush, 1500, 700, 100, 60) == 0,
            "GdipFillRectangleI accepts the rectangle");
      CHECK(g_getpix(dc, 1550, 730) == 0x00FF7F7Fu,
            "GDI+ ARGB fill blended 50% blue over white (0x7F7FFF)");

      auto gp_pen1 = (i32(TRT_GUEST_ABI*)(u32, float, i32, void**))
          resolve_gdiplus("GdipCreatePen1");
      auto gp_line = (i32(TRT_GUEST_ABI*)(void*, void*, i32, i32, i32, i32))
          resolve_gdiplus("GdipDrawLineI");
      void* gp_pen = nullptr;
      CHECK(gp_pen1 && gp_pen1(0xFFFF0000u, 3.0f, 2, &gp_pen) == 0 && gp_pen,
            "GdipCreatePen1 keeps an ARGB pen with a width");
      CHECK(gp_line && gp_line(gp, gp_pen, 1500, 800, 1600, 800) == 0,
            "GdipDrawLineI draws");
      CHECK(g_getpix(dc, 1550, 801) == 0x000000FFu,
            "GDI+ pen width 3 covers more than the centre row (COLORREF red)");

      auto gp_fontfam = (i32(TRT_GUEST_ABI*)(const u16*, void*, void**))
          resolve_gdiplus("GdipCreateFontFamilyFromName");
      auto gp_font = (i32(TRT_GUEST_ABI*)(void*, float, i32, i32, void**))
          resolve_gdiplus("GdipCreateFont");
      auto gp_string = (i32(TRT_GUEST_ABI*)(void*, const u16*, i32, void*,
                                            const void*, void*, void*))
          resolve_gdiplus("GdipDrawString");
      auto gp_measure = (i32(TRT_GUEST_ABI*)(void*, const u16*, i32, void*,
                                             const void*, void*, void*, void*,
                                             void*))resolve_gdiplus(
          "GdipMeasureString");
      static const u16 k_gp_text[] = {'G', 'd', 'i', '+', 0};
      void* gp_family = nullptr;
      void* gp_typeface = nullptr;
      CHECK(gp_fontfam && gp_font, "GDI+ font entry points resolve");
      if (gp_fontfam) gp_fontfam(k_gp_text, nullptr, &gp_family);
      if (gp_font) gp_font(gp_family, 24.0f, 0, 2, &gp_typeface);
      float layout[4] = {1500.0f, 900.0f, 300.0f, 40.0f};
      void* text_brush = nullptr;
      if (gp_fill) gp_fill(0xFF000000u, &text_brush);
      CHECK(gp_string && gp_string(gp, k_gp_text, 4, gp_typeface, layout,
                                   nullptr, text_brush) == 0,
            "GdipDrawString draws with the requested em size");
      i32 text_ink = 0;
      for (i32 y = 900; y < 940; ++y)
        for (i32 x = 1500; x < 1560; ++x)
          if (g_getpix(dc, x, y) == 0x00000000u) ++text_ink;
      CHECK(text_ink > 10, "GdipDrawString left glyph pixels on the surface");
      float bounds[4] = {0, 0, 0, 0};
      CHECK(gp_measure && gp_measure(gp, k_gp_text, 4, gp_typeface, layout,
                                     nullptr, bounds, nullptr, nullptr) == 0 &&
                bounds[2] > 0 && bounds[3] >= 24,
            "GdipMeasureString reports the laid-out extent");

      // Bitmap -> DrawImage: the pixels the caller owns must reach the DC.
      auto gp_bmp = (i32(TRT_GUEST_ABI*)(i32, i32, i32, i32, void*, void**))
          resolve_gdiplus("GdipCreateBitmapFromScan0");
      auto gp_drawimg = (i32(TRT_GUEST_ABI*)(void*, void*, i32, i32))
          resolve_gdiplus("GdipDrawImageI");
      u32 image_px[4] = {0xFFFF8000u, 0xFFFF8000u, 0xFFFF8000u, 0xFFFF8000u};
      void* gp_image = nullptr;
      CHECK(gp_bmp && gp_bmp(2, 2, 8, 0x26200Au, image_px, &gp_image) == 0 &&
                gp_image,
            "GdipCreateBitmapFromScan0 wraps caller pixels");
      CHECK(gp_drawimg && gp_drawimg(gp, gp_image, 1700, 900) == 0,
            "GdipDrawImageI draws the bitmap");
      CHECK(g_getpix(dc, 1700, 900) == 0x000080FFu,
            "GDI+ bitmap pixels carry their ARGB colours (orange)");

      // Path display list -> FillPath paints it.
      auto gp_path = (i32(TRT_GUEST_ABI*)(i32, void**))resolve_gdiplus(
          "GdipCreatePath");
      auto gp_pathrect = (i32(TRT_GUEST_ABI*)(void*, float, float, float, float))
          resolve_gdiplus("GdipAddPathRectangle");
      auto gp_fillpath = (i32(TRT_GUEST_ABI*)(void*, void*, void*))
          resolve_gdiplus("GdipFillPath");
      void* gp_shape = nullptr;
      void* path_brush = nullptr;
      if (gp_fill) gp_fill(0xFF00FF00u, &path_brush);
      CHECK(gp_path && gp_path(0, &gp_shape) == 0 && gp_shape &&
                gp_pathrect(gp_shape, 1500.0f, 960.0f, 40.0f, 20.0f) == 0 &&
                gp_fillpath(gp, path_brush, gp_shape) == 0,
            "GdipFillPath replays the recorded path");
      CHECK(g_getpix(dc, 1520, 970) == 0x0000FF00u,
            "GDI+ path fill landed on the DC surface");

      // ---- GDI+ affine transform ------------------------------------
      // Scale 2x about the origin: a 20x20 rect at (10,10) paints 40x40 at
      // (20,20). Before this, GdipScaleTransform returned NotImplemented.
      auto gp_scale = (i32(TRT_GUEST_ABI*)(void*, float, float, i32))
          resolve_gdiplus("GdipScaleTransform");
      auto gp_reset = (i32(TRT_GUEST_ABI*)(void*))resolve_gdiplus(
          "GdipResetTransform");
      auto gp_rotate = (i32(TRT_GUEST_ABI*)(void*, float, i32))
          resolve_gdiplus("GdipRotateTransform");
      auto gp_translate = (i32(TRT_GUEST_ABI*)(void*, float, float, i32))
          resolve_gdiplus("GdipTranslateTransform");
      CHECK(gp_scale && gp_scale(gp, 2.0f, 2.0f, 0) == 0,
            "GdipScaleTransform returns Ok");
      CHECK(gp_fill && gp_fill(0xFF0000FFu, &path_brush) == 0 &&
                gp_fillrect(gp, path_brush, 10, 10, 20, 20) == 0,
            "GdipFillRectangleI draws under the scaled transform");
      CHECK(g_getpix(dc, 24, 24) == 0x00FF0000u &&
                g_getpix(dc, 58, 58) == 0x00FF0000u &&
                g_getpix(dc, 62, 62) == 0x00FFFFFFu,
            "the scaled fill covered 40x40 device pixels, not 20x20");
      // Translate is applied after the scale (prepend order): +100,+0 in
      // device space moves the scaled rect right by exactly 100 px.
      CHECK(gp_translate && gp_translate(gp, 100.0f, 0.0f, 0) == 0 &&
                gp_fillrect(gp, path_brush, 10, 10, 20, 20) == 0 &&
                g_getpix(dc, 124, 24) == 0x00FF0000u,
            "GdipTranslateTransform composes with the scale transform");
      // Rotation about the rect's centre (420,410) by 90 degrees turns the
      // 40x20 rect into a 20x40 one spanning x 410..430, y 390..430.
      CHECK(gp_reset && gp_reset(gp) == 0 &&
                gp_translate(gp, -420.0f, -410.0f, 0) == 0 &&
                gp_rotate && gp_rotate(gp, 90.0f, 0) == 0 &&
                gp_translate(gp, 420.0f, 410.0f, 0) == 0 &&
                gp_fillrect(gp, path_brush, 400, 400, 40, 20) == 0,
            "GdipRotateTransform returns Ok and the rotated fill runs");
      CHECK(g_getpix(dc, 420, 400) == 0x00FF0000u &&
                g_getpix(dc, 405, 410) != 0x00FF0000u &&
                g_getpix(dc, 440, 400) != 0x00FF0000u,
            "the rotated rectangle covers its rotated footprint only");
      if (gp_reset) gp_reset(gp);

      // Matrix objects: create, invert, and transform a point round-trip.
      auto gp_mkmat = (i32(TRT_GUEST_ABI*)(float, float, float, float, float,
                                           float, void**))
          resolve_gdiplus("GdipCreateMatrix2");
      auto gp_invmat = (i32(TRT_GUEST_ABI*)(void*))resolve_gdiplus(
          "GdipInvertMatrix");
      auto gp_xfpts = (i32(TRT_GUEST_ABI*)(void*, void*, i32))resolve_gdiplus(
          "GdipTransformMatrixPoints");
      auto gp_delmat = (i32(TRT_GUEST_ABI*)(void*))resolve_gdiplus(
          "GdipDeleteMatrix");
      void* mat = nullptr;
      CHECK(gp_mkmat && gp_mkmat(2.0f, 0.0f, 0.0f, 2.0f, 10.0f, 20.0f, &mat) ==
                            0 && mat,
            "GdipCreateMatrix2 makes a matrix object");
      float pt[2] = {3.0f, 4.0f};
      CHECK(gp_xfpts && gp_xfpts(mat, pt, 1) == 0 && pt[0] == 16.0f &&
                pt[1] == 28.0f,
            "GdipTransformMatrixPoints applies scale then translation");
      CHECK(gp_invmat && gp_invmat(mat) == 0 && gp_xfpts(mat, pt, 1) == 0 &&
                pt[0] == 3.0f && pt[1] == 4.0f,
            "GdipInvertMatrix round-trips the transformed point");
      if (gp_delmat) gp_delmat(mat);

      // ---- msimg32 composites --------------------------------------
      auto mi_alpha = (i32(TRT_GUEST_ABI*)(void*, i32, i32, i32, i32, void*,
                                           i32, i32, i32, i32, u32))
          resolve_msimg32("AlphaBlend");
      auto mi_grad = (i32(TRT_GUEST_ABI*)(void*, void*, u32, void*, u32, u32))
          resolve_msimg32("GradientFill");
      auto mi_tblt = (i32(TRT_GUEST_ABI*)(void*, i32, i32, i32, i32, void*, i32,
                                          i32, i32, i32, u32))
          resolve_msimg32("TransparentBlt");
      g_patblt(dc, 1200, 990, 40, 40, 0x00000042u);   // source: black block
      g_patblt(dc, 1260, 990, 40, 40, 0x00FF0062u);   // destination: white
      CHECK(mi_alpha && mi_alpha(dc, 1260, 990, 40, 40, dc, 1200, 990, 40, 40,
                                 0x00800000u) == 1,
            "AlphaBlend AC_SRC_OVER accepts the blend");
      CHECK(g_getpix(dc, 1280, 1010) == 0x007F7F7Fu,
            "AlphaBlend 50% black over white is mid grey");
      CHECK(mi_alpha && mi_alpha(dc, 1260, 1040, 40, 40, dc, 1200, 990, 40, 40,
                                 0x00FF0000u) == 1 &&
                g_getpix(dc, 1280, 1060) == 0x00000000u,
            "AlphaBlend at SourceConstantAlpha 255 copies the source");
      g_patblt(dc, 1320, 990, 40, 40, 0x00FF0062u);   // white again
      CHECK(mi_tblt && mi_tblt(dc, 1320, 990, 40, 40, dc, 1200, 990, 40, 40,
                               0x00FFFFFFu) == 1 &&
                g_getpix(dc, 1340, 1010) == 0x00000000u,
            "TransparentBlt copies pixels that are not the colour key");
      g_patblt(dc, 1320, 1040, 40, 40, 0x00FF0062u);  // white again
      CHECK(mi_tblt && mi_tblt(dc, 1320, 1040, 40, 40, dc, 1200, 990, 40, 40,
                               0x00000000u) == 1 &&
                g_getpix(dc, 1340, 1060) == 0x00FFFFFFu,
            "TransparentBlt skips every pixel equal to the colour key");
      struct { u32 x, y; u16 r, g, b, a; } verts[2];
      verts[0].x = 1400; verts[0].y = 990;
      verts[0].r = 0xFF00; verts[0].g = 0; verts[0].b = 0; verts[0].a = 0;
      verts[1].x = 1900; verts[1].y = 1030;
      verts[1].r = 0; verts[1].g = 0; verts[1].b = 0xFFFF; verts[1].a = 0;
      u32 mesh[2] = {0, 1};
      CHECK(mi_grad && mi_grad(dc, verts, 2, mesh, 1, 0) == 1,
            "GradientFill GRADIENT_FILL_RECT_H accepts the mesh");
      const u32 left = g_getpix(dc, 1401, 1010);
      const u32 right = g_getpix(dc, 1899, 1010);
      char gradmsg[160];
      snprintf(gradmsg, sizeof(gradmsg),
               "GradientFill ramps red on the left (0x%06x) to blue on the "
               "right (0x%06x)",
               left, right);
      CHECK((left & 0xFF) > 150 && ((left >> 16) & 0xFF) < 100 &&
                ((right >> 16) & 0xFF) > 150 && (right & 0xFF) < 100,
            gradmsg);
      CHECK(mi_grad && mi_grad(dc, verts, 2, mesh, 1, 2) == 0,
            "GradientFill GRADIENT_FILL_TRIANGLE -> FALSE (named gap)");

      // ---- Latin-1 text coverage + real scrolling -------------------
      CHECK(trt_font8x16::kGlyphIndex[0xE9] != trt_font8x16::kGlyphMissing,
            "Latin-1 glyph coverage is compiled in (U+00E9 e-acute)");
      i32 acute_ink = 0;
      {
        const int slot = trt_font8x16::kGlyphIndex[0xE9];
        for (i32 y = 0; y < trt_font8x16::kGlyphH; ++y)
          for (i32 x = 0; x < trt_font8x16::kGlyphW; ++x)
            if (trt_font8x16::kGlyphCoverage[slot][y][x]) ++acute_ink;
      }
      CHECK(acute_ink > 20,
            "the e-acute glyph is a real raster, not a placeholder box");
      // ScrollWindowEx moves pixels: a 20px red band at y=700..719 must move
      // up to y=680..699 when the window scrolls by dy=+20 (GDI semantics).
      g_patblt(dc, 300, 660, 200, 100, 0x00FF0062u);  // white the area
      void* band_brush = g_brush ? g_brush(0x000000FFu) : nullptr;  // red
      if (band_brush) {
        i32 band[4] = {300, 700, 500, 720};
        auto g_fillrect2 = (i32(TRT_GUEST_ABI*)(void*, const void*, void*))
            resolve_user32("FillRect");
        if (g_fillrect2) g_fillrect2(dc, band, band_brush);
      }
      CHECK(g_getpix(dc, 400, 710) == 0x000000FFu,
            "the scroll test band is painted red");
      trt_raster::scroll(dc, 0, 20, 300, 660, 500, 740);
      char scrollmsg[200];
      snprintf(scrollmsg, sizeof(scrollmsg),
               "raster scroll moved the band up by the scrolled amount "
               "(680..699=0x%06x 700..719=0x%06x 720+=0x%06x)",
               g_getpix(dc, 400, 690), g_getpix(dc, 400, 710),
               g_getpix(dc, 400, 730));
      CHECK(g_getpix(dc, 400, 690) == 0x000000FFu &&
                g_getpix(dc, 400, 710) == 0x00FFFFFFu,
            scrollmsg);

      // BMP dump is the offline evidence path.
      CHECK(trt_raster::dump_bmp(dc, "/tmp/tir_gdi_raster_test.bmp"),
            "raster surface dumps a BMP");
      FILE* bf = fopen("/tmp/tir_gdi_raster_test.bmp", "rb");
      long bsz = 0;
      if (bf) { fseek(bf, 0, SEEK_END); bsz = ftell(bf); fclose(bf); }
      CHECK(bsz == 54 + (long)1920 * 1080 * 3,
            "BMP dump has the full 24bpp body");
      CHECK(trt_raster::non_background_pixels(dc, 0x00FFFFFFu) > 1000,
            "the surface holds real painted pixels");

      // Releasing the DC drops the surface: no stale pixels for a reused DC.
      g_rel(nullptr, dc);
      CHECK(g_getpix(dc, 10, 10) == 0xFFFFFFFFu,
            "ReleaseDC drops the surface (GetPixel -> CLR_INVALID)");
    }
  }

  if (failures) {
    printf("RESULT: %d FAIL\n", failures);
    return 1;
  }
  printf("RESULT: ALL PASS\n");
  return 0;
}
