// tiramisu/src/guest/guest_gdi.c
// Proves the gdi32 bridge: the font/object lifecycle a dialog-based game
// runs (OrTMM's exact import set plus the state setters SDL2-class engines
// use). Returns a bitmask:
//   0x01 GetDC -> a real DC record
//   0x02 CreateFontIndirectW -> non-NULL handle
//   0x04 SelectObject(font) -> non-NULL (previous default), second select
//        returns the font (per-DC state really recorded)
//   0x08 DeleteObject(font) -> TRUE; select after delete fails NULL
//   0x10 GetStockObject(BLACK_BRUSH) -> stable stock handle
//   0x20 SelectObject(stock) works; ReleaseDC -> TRUE
//   0x40 SelectObject with a garbage DC -> NULL (fail-visible)
//   0x80 SetBkMode/SetTextColor/SetBkColor on the DC -> nonzero
// Host-side checks (tests/test_gdi.cpp) additionally observe the log: the
// CreateFontIndirectW lfHeight line, and the object count lifecycle.
typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned char u8;
typedef unsigned long long u64;
typedef int i32;

#ifdef _WIN32
#define GSTD __stdcall
#else
#define GSTD
#endif

int _fltused = 1;

// LOGFONTW (x64, 96 bytes): we set only the fields games set
struct logfontw {
  i32 height; i32 width; i32 escapement; i32 orientation; i32 weight;
  u8 italic; u8 underline; u8 strikeout; u8 charset; u8 out_prec;
  u8 clip_prec; u8 quality; u8 pitch_family; u16 face[32];
};

__attribute__((dllimport)) void* GSTD GetDC(void* hwnd);
__attribute__((dllimport)) i32 GSTD ReleaseDC(void* hwnd, void* hdc);
__attribute__((dllimport)) void* GSTD CreateFontIndirectW(const struct logfontw* lf);
__attribute__((dllimport)) void* GSTD SelectObject(void* hdc, void* obj);
__attribute__((dllimport)) i32 GSTD DeleteObject(void* obj);
__attribute__((dllimport)) void* GSTD GetStockObject(i32 obj);
__attribute__((dllimport)) i32 GSTD SetBkMode(void* hdc, i32 mode);
__attribute__((dllimport)) u32 GSTD SetTextColor(void* hdc, u32 color);
__attribute__((dllimport)) u32 GSTD SetBkColor(void* hdc, u32 color);

static void make_font(struct logfontw* lf, i32 height) {
  for (u32 i = 0; i < sizeof(struct logfontw) / 4; ++i)
    ((u32*)lf)[i] = 0;
  lf->height = height;
  lf->weight = 700 /*FW_BOLD*/;
  lf->charset = 1 /*DEFAULT_CHARSET*/;
}

u32 GSTD guest_gdi_main(u64 a, u64 b, u64 c) {
  (void)a; (void)b; (void)c;
  u32 bits = 0;

  // 1. a DC from user32 (the shared record space)
  void* hdc = GetDC(0);
  if (hdc) bits |= 0x01u;

  // 2. create a font (OrTMM: CreateFontIndirectW)
  struct logfontw lf;
  make_font(&lf, -24 /*24px tall, negative = char height*/);
  void* font = CreateFontIndirectW(&lf);
  if (font) bits |= 0x02u;

  // 3. select it into the DC (OrTMM: SelectObject)
  if (hdc && font) {
    void* prev = SelectObject(hdc, font);
    if (prev) bits |= 0x04u;
    // second select: the previous object must be OUR font (state recorded)
    void* prev2 = SelectObject(hdc, GetStockObject(14 /*SYSTEM_FONT*/));
    if (prev2 == font) bits |= 0x04u;
  }

  // 4. delete the font (OrTMM: DeleteObject); select after delete must fail
  if (font) {
    if (DeleteObject(font) == 1) bits |= 0x08u;
    if (hdc && SelectObject(hdc, font) == 0) bits |= 0x08u;
  }

  // 5. stock objects are stable handles
  void* black = GetStockObject(4 /*BLACK_BRUSH*/);
  if (black && GetStockObject(4) == black) bits |= 0x10u;

  // 6. stock objects select fine (ReleaseDC is deferred: the state setters
  //    below still use this DC, as a game would before releasing)
  if (hdc && black && SelectObject(hdc, black)) bits |= 0x20u;

  // 7. garbage DC fails visibly
  if (SelectObject((void*)0xDEAD0000u, (void*)0x80000004u) == 0)
    bits |= 0x40u;

  // 8. state setters accept the real DC
  // SetBkMode returns the PREVIOUS mode (real GDI), not a boolean: a fresh DC
  // is OPAQUE(2), so a non-zero return is the contract here.
  if (SetBkMode(hdc, 2) != 0 && SetTextColor(hdc, 0x00FF0000u) != 0xFFFFFFFFu &&
      SetBkColor(hdc, 0x00000000u) != 0xFFFFFFFFu)
    bits |= 0x80u;

  // 9. release the DC only after the last use (the GDI discipline)
  if (hdc && ReleaseDC(0, hdc) == 1) bits |= 0x20u;

  return bits;
}
