// tiramisu/tests/test_desktop_paint.cpp
// End-to-end desktop-application proof: a real desktop-shaped PE registers a
// window class, creates a window, pumps messages and paints in WM_PAINT
// (PatBlt background, TextOutA text, Rectangle). The host then dumps the
// window's raster surface and checks the pixels are there.
//
// This is the gate for "desktop apps can draw". Before the raster surface and
// the paint cycle existed, every one of those calls named itself as drawing
// nothing, so a booting desktop app proved only that it did not crash.
#include "../src/trt/trt.h"
#include "../src/trt/gdi_raster.h"
#include "../src/trt/input.h"

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

struct extent { void* base; size_t len; };
extent g_ext[16];
int g_extn = 0;

void* l_alloc(size_t bytes, u64 pref) {
  void* p = mmap(pref ? (void*)(uintptr_t)pref : nullptr, bytes,
                 PROT_READ | PROT_WRITE,
                 pref ? (MAP_FIXED_NOREPLACE | MAP_PRIVATE | MAP_ANONYMOUS)
                      : (MAP_PRIVATE | MAP_ANONYMOUS),
                 -1, 0);
  if (p == MAP_FAILED) return nullptr;
  if (g_extn < 16) { g_ext[g_extn] = {p, bytes}; ++g_extn; }
  return p;
}
int l_protect(void* base, size_t bytes, unsigned prot) {
  int p = 0;
  if (prot & TRT_PROT_R) p |= PROT_READ;
  if (prot & TRT_PROT_W) p |= PROT_WRITE;
  if (prot & TRT_PROT_X) p |= PROT_EXEC;
  const size_t page = 4096;
  const uintptr_t b = (uintptr_t)base & ~(uintptr_t)(page - 1);
  const size_t len = ((uintptr_t)base + bytes - b + page - 1) & ~(page - 1);
  return mprotect((void*)b, len, p);
}
void l_release(void* base, size_t) {
  for (int i = 0; i < g_extn; ++i)
    if (g_ext[i].base == base) { munmap(base, g_ext[i].len); g_ext[i].base = nullptr; return; }
}

char g_log[512][256];
int g_log_count = 0;
bool log_contains(const char* needle) {
  for (int i = 0; i < g_log_count; ++i)
    if (strstr(g_log[i], needle)) return true;
  return false;
}
void l_log(const char* line) {
  if (g_log_count < 512) {
    snprintf(g_log[g_log_count], 256, "%s", line);
    ++g_log_count;
  }
  printf("  [trt] %s\n", line);
}

u8* read_file(const char* path, size_t* size) {
  FILE* f = fopen(path, "rb");
  if (!f) return nullptr;
  fseek(f, 0, SEEK_END);
  const long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  u8* buf = (u8*)malloc((size_t)n);
  if (fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); fclose(f); return nullptr; }
  fclose(f);
  *size = (size_t)n;
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
    : [a0] "m"(args[0]), [a1] "m"(args[1]), [a2] "m"(args[2]), [fn] "r"(fn)
    : "rcx", "rdx", "r8", "r9", "r10", "r11",
      "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "memory", "cc");
  return ret;
}

int failures = 0;
#define CHECK(cond, msg)                                             \
  do {                                                               \
    if (cond) printf("  PASS %s\n", msg);                            \
    else { printf("  FAIL %s\n", msg); ++failures; }                 \
  } while (0)

// Count pixels in a dumped 24bpp BMP that are not the given grey level.
long count_nonwhite(const char* path, u32 rgb) {
  size_t size = 0;
  u8* f = read_file(path, &size);
  if (!f || size < 54) { free(f); return -1; }
  const i32 w = *(i32*)(f + 18);
  const i32 h = *(i32*)(f + 22);
  const u32 off = *(u32*)(f + 10);
  const u32 row = ((u32)w * 3u + 3u) & ~3u;
  if (off + row * (u32)h > size) { free(f); return -1; }
  const u8 want_b = (u8)(rgb & 0xFF), want_g = (u8)((rgb >> 8) & 0xFF),
           want_r = (u8)((rgb >> 16) & 0xFF);
  long n = 0;
  for (i32 y = 0; y < h; ++y) {
    const u8* p = f + off + (size_t)y * row;
    for (i32 x = 0; x < w; ++x, p += 3)
      if (p[0] != want_b || p[1] != want_g || p[2] != want_r) ++n;
  }
  free(f);
  return n;
}

}  // namespace

int main(int argc, char** argv) {
  setvbuf(stdout, NULL, _IONBF, 0);
  if (argc < 2) {
    fprintf(stderr, "usage: %s <guest_desktop.exe>\n", argv[0]);
    return 2;
  }
  printf("tiramisu DESKTOP-APP paint test — %s\n", argv[1]);

  trt_backend be{&l_alloc, &l_protect, &l_release, &l_log, nullptr};
  trt_bridge_boot(&be);
  trt_bridge_register_kernel32();
  trt_bridge_register_user32();
  trt_bridge_register_gdi();

  size_t size = 0;
  u8* file = read_file(argv[1], &size);
  if (!file) { fprintf(stderr, "cannot read %s\n", argv[1]); return 2; }

  trt_resolver rs;
  trt_resolver_default(&rs);
  trt_image* img = nullptr;
  const int rc = trt_map_pe(&be, &rs, file, size, &img);
  CHECK(rc == TRT_OK, "map the desktop-shaped guest PE");
  if (rc != TRT_OK) { free(file); return 1; }
  CHECK(trt_seal_image(&be, img) == TRT_OK, "seal (W^X)");

  const u64 ret = win64_call(img->base + img->entry_rva, 0, 0, 0);
  printf("  guest returned 0x%08llx (want 0x1ffff)\n", (unsigned long long)ret);
  CHECK(ret & 0x01u, "RegisterClassExW -> atom");
  CHECK(ret & 0x02u, "CreateWindowExW -> hwnd");
  CHECK(ret & 0x04u, "WM_PAINT delivered with a paint DC");
  CHECK(ret & 0x08u, "PatBlt erased the background");
  CHECK(ret & 0x10u, "TextOutA drew the title text");
  CHECK(ret & 0x20u, "Rectangle drew the framed box");
  CHECK(ret & 0x40u, "message pump dispatched the paint cycle");
  CHECK(ret & 0x80u, "GetTextExtentPoint32A measured the selected font");
  CHECK(ret & 0x100u, "GDI+ filled a rectangle through the DC surface");
  CHECK(ret & 0x200u, "GDI+ blitted a caller-owned bitmap");
  CHECK(ret & 0x400u, "msimg32 AlphaBlend composited at 50%");
  CHECK(ret & 0x800u, "msimg32 GradientFill painted a red->blue ramp");
  CHECK(ret & 0x1000u, "RoundRect drew with rounded corners");
  CHECK(ret & 0x2000u, "ScrollWindowEx accepted the scroll");
  CHECK(ret & 0x4000u,
        "InvalidateRect + GetUpdateRect report the same sub-rectangle");
  CHECK(ret & 0x8000u, "BeginPath/Rectangle/EndPath + FillPath painted");
  CHECK(ret & 0x10000u, "GdipScaleTransform scaled the following fill");

  CHECK(log_contains("TextOutW c=16"), "log names the drawn string");
  CHECK(log_contains("TextOutW c=16") && log_contains("cell=12x24"),
        "text used the guest's -24 font (advance 12, cell 24)");

  char dir[] = "/tmp/tiramisu-desktop.XXXXXX";
  if (!mkdtemp(dir)) { printf("  FAIL temp dir\n"); ++failures; }
  const int dumped = trt_raster::dump_all(dir);
  char dmsg[128];
  snprintf(dmsg, sizeof(dmsg), "%d window surface(s) dumped for inspection",
           dumped);
  CHECK(dumped >= 1, dmsg);

  // The guest painted a 640x400 white client area with black text and a green
  // box. Count inside that area (the BMP is bottom-up, so the client area's
  // top row is the file's last row).
  char bmp[512];
  snprintf(bmp, sizeof(bmp), "%s/gdi-dc1.bmp", dir);
  const long ink = count_nonwhite(bmp, 0x00FFFFFFu);
  char imsg[160];
  snprintf(imsg, sizeof(imsg),
           "dumped surface holds the painted client area (%ld non-white px "
           "outside it)", ink);
  CHECK(ink > 1000, imsg);

  size_t bsize = 0;
  u8* bf = read_file(bmp, &bsize);
  long white = 0, black_in_client = 0, green = 0;
  long magenta = 0, orange = 0, grey = 0, red_left = 0, blue_right = 0;
  long teal = 0, yellow = 0, violet = 0;
  if (bf && bsize > 54) {
    const i32 w = *(i32*)(bf + 18);
    const i32 h = *(i32*)(bf + 22);
    const u32 off = *(u32*)(bf + 10);
    const u32 row = ((u32)w * 3u + 3u) & ~3u;
    for (i32 y = 0; y < h && off + row * (u32)(y + 1) <= bsize; ++y) {
      const u8* p = bf + off + (size_t)y * row;
      const i32 client_y = h - 1 - y;  // BMP rows are bottom-up
      for (i32 x = 0; x < w; ++x, p += 3) {
        const bool in_client = x < 640 && client_y < 400;
        if (p[0] == 255 && p[1] == 255 && p[2] == 255) ++white;
        if (in_client && p[0] == 0 && p[1] == 0 && p[2] == 0)
          ++black_in_client;
        if (p[0] == 0 && p[1] == 255 && p[2] == 0) ++green;
        if (p[0] == 255 && p[1] == 0 && p[2] == 255) ++magenta;
        if (p[0] == 0 && p[1] == 128 && p[2] == 255) ++orange;
        if (p[0] == 0x80 && p[1] == 0x80 && p[2] == 0x00) ++teal;    // path fill
        if (p[0] == 0x00 && p[1] == 0xFF && p[2] == 0xFF) ++yellow;  // clipped
        // GDI+ ARGB 0xFF8000FF under a 2x scale: violet, 40x40 = 1600 px.
        if (p[0] == 0xFF && p[1] == 0x00 && p[2] == 0x80) ++violet;
        // 50% white over black: every channel lands at 127..128.
        if (p[0] >= 125 && p[0] <= 129 && p[1] >= 125 && p[1] <= 129 &&
            p[2] >= 125 && p[2] <= 129)
          ++grey;
        // GradientFill ramp: red on the left edge, blue on the right.
        if (x >= 401 && x <= 405 && client_y >= 305 && client_y <= 335 &&
            p[2] > 150 && p[0] < 100)
          ++red_left;
        if (x >= 633 && x <= 637 && client_y >= 305 && client_y <= 335 &&
            p[0] > 150 && p[2] < 100)
          ++blue_right;
      }
    }
  }
  free(bf);
  char gmsg[200];
  snprintf(gmsg, sizeof(gmsg),
           "client area is white (%ld px), text is black (%ld px) and the box "
           "is green (%ld px)",
           white, black_in_client, green);
  CHECK(white > 150000 && black_in_client > 200 && green > 1000, gmsg);
  char pmsg[240];
  snprintf(pmsg, sizeof(pmsg),
           "GDI+ magenta (%ld px) and bitmap orange (%ld px) rendered",
           magenta, orange);
  CHECK(magenta > 1000 && orange > 200, pmsg);

  // GDI path: FillPath painted the recorded 100x80 rectangle, and the path
  // clip let PATCOPY paint only the 80x80 recorded region.
  char pathmsg[192];
  snprintf(pathmsg, sizeof(pathmsg),
           "path fill teal (%ld px, want ~8000) and clipped PATCOPY yellow "
           "(%ld px, want ~6400)", teal, yellow);
  CHECK(teal > 6000 && teal < 9500 && yellow > 5000 && yellow < 7500,
        pathmsg);

  char xfmsg[160];
  snprintf(xfmsg, sizeof(xfmsg),
           "GDI+ scaled fill covered %ld device px (want 1600 = 40x40)", violet);
  CHECK(violet > 1400 && violet < 1800, xfmsg);
  snprintf(pmsg, sizeof(pmsg),
           "AlphaBlend produced %ld mid-grey px (50%% over black)", grey);
  CHECK(grey > 4000, pmsg);
  snprintf(pmsg, sizeof(pmsg),
           "GradientFill ramp: %ld red px on the left, %ld blue px on the right",
           red_left, blue_right);
  CHECK(red_left > 100 && blue_right > 100, pmsg);

  // Rounded corners: the corner of the RoundRect is untouched background while
  // its middle is the green brush.
  long corner_green = 0, centre_green = 0;
  if (bf) {
    const i32 w = *(i32*)(bf + 18);
    const i32 h = *(i32*)(bf + 22);
    const u32 off = *(u32*)(bf + 10);
    const u32 row = ((u32)w * 3u + 3u) & ~3u;
    auto px_at = [&](i32 x, i32 y, u8* out) {
      const i32 row_y = h - 1 - y;
      if (row_y < 0 || x < 0 || x >= w) return false;
      const u8* q = bf + off + (size_t)row_y * row + (size_t)x * 3;
      out[0] = q[0];
      out[1] = q[1];
      out[2] = q[2];
      return true;
    };
    u8 c[3] = {0, 0, 0};
    if (px_at(401, 73, c) && !(c[0] == 0 && c[1] == 255 && c[2] == 0))
      ++corner_green;
    if (px_at(480, 120, c) && c[0] == 0 && c[1] == 255 && c[2] == 0)
      ++centre_green;
  }
  CHECK(corner_green && centre_green,
        "RoundRect corner is empty while its middle is filled (rounded)");

  // ScrollWindowEx(hwnd, 0, +20, {24,72,360,168}) must move the green box up
  // by 20 px: y=60 is now green and y=155 (the old bottom) is background.
  long box_moved = 0, box_gone = 0;
  if (bf) {
    const i32 w = *(i32*)(bf + 18);
    const i32 h = *(i32*)(bf + 22);
    const u32 off = *(u32*)(bf + 10);
    const u32 row = ((u32)w * 3u + 3u) & ~3u;
    auto px_at = [&](i32 x, i32 y, u8* out) {
      const i32 row_y = h - 1 - y;
      if (row_y < 0 || x < 0 || x >= w) return false;
      const u8* q = bf + off + (size_t)row_y * row + (size_t)x * 3;
      out[0] = q[0];
      out[1] = q[1];
      out[2] = q[2];
      return true;
    };
    u8 c[3] = {0, 0, 0};
    // The black bar was at y=140; scrolling by dy=+20 moves it to y=120 and
    // the row it vacated shows the green that used to be at y=160.
    const bool bar = px_at(200, 120, c) && c[0] == 0 && c[1] == 0 && c[2] == 0;
    const bool vacated =
        px_at(200, 140, c) && c[0] == 0 && c[1] == 255 && c[2] == 0;
    if (bar) ++box_moved;
    if (vacated) ++box_gone;
  }
  CHECK(box_moved && box_gone,
        "ScrollWindowEx moved painted pixels (black bar y=140 -> y=120)");
  printf("  surface dump: %s\n", bmp);

  trt_unmap(&be, img);
  free(file);

  if (failures) {
    printf("RESULT: %d FAIL\n", failures);
    return 1;
  }
  printf("RESULT: ALL PASS\n");
  return 0;
}
