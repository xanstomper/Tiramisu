// tiramisu/tests/test_dinput.cpp
// DirectInput 8: the input path most PC games actually use. The guest
// creates the factory, the mouse and keyboard devices, acquires them, and
// polls states fed by the harness through the real input ring.
#include "../src/trt/trt.h"
#include "../src/trt/input.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

extern "C" {
int trt_bridge_register_kernel32(void);
int trt_bridge_register_user32(void);
int trt_bridge_register_dinput(void);
}

namespace {

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

} // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    printf("usage: test_dinput <guest.exe>\n");
    return 2;
  }
  size_t size = 0;
  u8* file = read_file(argv[1], &size);
  if (!file) { printf("cannot read %s\n", argv[1]); return 2; }

  trt_backend be{&linux_alloc, &linux_protect, &linux_release, &linux_log,
                 nullptr};
  trt_bridge_boot(&be);
  trt_bridge_register_kernel32();
  trt_bridge_register_user32();
  trt_bridge_register_dinput();

  printf("tiramisu DirectInput 8 test\n");

  trt_resolver rs;
  trt_resolver_default(&rs);
  trt_image* img = nullptr;
  const int rc = trt_map_pe(&be, &rs, file, size, &img);
  CHECK(rc == TRT_OK, "guest PE maps (dinput8 bound)");
  if (rc != TRT_OK || !img) {
    printf("  map failed: %s (%s)\n", trt_errstr(rc),
           img ? img->last_detail : "no image");
    free(file);
    return 1;
  }
  CHECK(trt_seal_image(&be, img) == TRT_OK, "image sealed (W^X)");

  const u64 ret = win64_call(img->base + img->entry_rva, 0, 0, 0);
  printf("  guest returned 0x%llx (want 0x1ff)\n", (unsigned long long)ret);

  CHECK(ret & 0x1u, "DirectInput8Create returned the factory");
  CHECK(ret & 0x2u, "CreateDevice (GUID_SysMouse) returned the device");
  CHECK(ret & 0x4u, "SetDataFormat accepted");
  CHECK(ret & 0x8u, "SetCooperativeLevel accepted");
  CHECK(ret & 0x10u, "Acquire accepted");
  CHECK(ret & 0x20u, "GetDeviceState carried fed deltas + button");
  CHECK(ret & 0x40u, "keyboard state carried the fed key");
  CHECK(ret & 0x80u, "keyboard released after key-up");
  CHECK(ret & 0x100u, "Release counted down");

  // DirectInput 8 Gamepad enumeration & live DIJOYSTATE
  {
    using DI8Create_t = int(TRT_GUEST_ABI*)(void*, u32, const u8*, void**, void*);
    auto pDI8C = (DI8Create_t)trt_resolve_module(nullptr, "dinput8.dll", "DirectInput8Create", 0);
    void* di8 = nullptr;
    const int chr = pDI8C ? pDI8C(nullptr, 0x0800u, nullptr, &di8, nullptr) : -1;
    struct enum_ctx {
      int count;
      bool has_pad;
      u8 pad_guid[16];
    } ectx{0, false, {}};
    auto enum_cb = +[](const void* inst, void* user) TRT_GUEST_ABI -> int {
      auto* ctx = (enum_ctx*)user;
      ++ctx->count;
      u32 dev_type = 0;
      memcpy(&dev_type, (const u8*)inst + 36, 4);
      if ((dev_type & 0xFFu) == 0x15u) {
        ctx->has_pad = true;
        memcpy(ctx->pad_guid, (const u8*)inst + 4, 16);
      }
      return 1;  // DIENUM_CONTINUE
    };
    void* pad_dev = nullptr;
    struct joy80 {
      i32 lX, lY, lZ, lRx, lRy, lRz;
      i32 rglSlider[2];
      u32 rgdwPOV[4];
      u8 rgbButtons[32];
    } js{};
    u8 dev_info[1080] = {};
    *(u32*)dev_info = sizeof(dev_info);
    u8 caps[44] = {};
    *(u32*)caps = sizeof(caps);

    trt_input_pad_state_n(0, true, 32767, 0, -32768, 0,
                          0x1001u /*A + DPadUp*/, 200u, 0u);
    trt_pad_set_vibration(0, 12345u, 23456u);

    int gds_hr = -1, gdi_hr = -1, gcap_hr = -1;
    if (chr == 0 && di8) {
      void** ivt = *(void***)di8;
      ((int(TRT_GUEST_ABI*)(void*, u32, void*, void*, u32))ivt[4])(
          di8, 0 /*DI8DEVCLASS_ALL*/, (void*)enum_cb, &ectx, 0);
      if (ectx.has_pad) {
        ((int(TRT_GUEST_ABI*)(void*, const u8*, void**, void*))ivt[3])(
            di8, ectx.pad_guid, &pad_dev, nullptr);
      }
      if (pad_dev) {
        void** dvt = *(void***)pad_dev;
        gcap_hr = ((int(TRT_GUEST_ABI*)(void*, void*))dvt[3])(pad_dev, caps);
        gdi_hr = ((int(TRT_GUEST_ABI*)(void*, void*))dvt[15])(pad_dev, dev_info);
        ((int(TRT_GUEST_ABI*)(void*, void*, u32))dvt[13])(pad_dev, nullptr, 0);
        ((int(TRT_GUEST_ABI*)(void*))dvt[7])(pad_dev);
        gds_hr = ((int(TRT_GUEST_ABI*)(void*, u32, void*))dvt[9])(pad_dev, sizeof(js), &js);
        // DISFFC_STOPALL (2) clears controller vibration
        ((int(TRT_GUEST_ABI*)(void*, u32))dvt[22])(pad_dev, 2u);
        ((u32(TRT_GUEST_ABI*)(void*))dvt[2])(pad_dev);
      }
      ((u32(TRT_GUEST_ABI*)(void*))ivt[2])(di8);
    }
    u16 vl = 99, vr = 99;
    trt_pad_get_vibration(0, &vl, &vr);
    CHECK(ectx.count >= 3 && ectx.has_pad && pad_dev != nullptr &&
              gcap_hr == 0 && gdi_hr == 0 && gds_hr == 0 &&
              js.lX > 60000 && js.rgdwPOV[0] == 0u &&
              (js.rgbButtons[0] & 0x80u) != 0 && vl == 0 && vr == 0,
          "DirectInput8 EnumDevices/CreateDevice/GetDeviceInfo/DIJOYSTATE/STOPALL gamepad flow");
  }

  printf("\nRESULT: %s\n", failures ? "FAILURES" : "ALL PASS");
  return failures ? 1 : 0;
}
