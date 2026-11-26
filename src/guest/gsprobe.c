// gsprobe.c — minimal freestanding PE used to characterise the guest's GS/TEB
// on the console. Writes what %gs:0x08/0x10/0x18 and the TEB self pointer say,
// plus a canary StackBase value, to C:\gsprobe.txt. No CRT, no UI.
//
// Built by tools/build-hermes.sh when GSPROBE=1.

typedef unsigned int u32;
typedef unsigned long long u64;
typedef long long i64;
typedef void* HANDLE;

__attribute__((dllimport)) HANDLE CreateFileA(const char*, u32, u32, void*, u32, u32, HANDLE);
__attribute__((dllimport)) int WriteFile(HANDLE, const void*, u32, u32*, void*);
__attribute__((dllimport)) int CloseHandle(HANDLE);
__attribute__((dllimport)) void ExitProcess(u32);
__attribute__((dllimport)) u32 GetLastError(void);

static u64 rdgs(u32 off) {
  u64 v;
  __asm__ volatile("movq %%gs:%c1, %0" : "=r"(v) : "i"(off));
  return v;
}

static u32 hex(char* out, u64 v) {
  static const char* d = "0123456789abcdef";
  u32 n = 0;
  for (int i = 15; i >= 0; --i) out[n++] = d[(v >> (i * 4)) & 15];
  return n;
}

static char buf[1024];
static u32 blen;

static void put(const char* s) { while (*s) buf[blen++] = *s++; }
static void kv(const char* k, u64 v) {
  while (*k) buf[blen++] = *k++;
  buf[blen++] = '=';
  blen += hex(buf + blen, v);
  buf[blen++] = '\n';
}

void gsprobe_main(void) {
  put("gsprobe\n");
  kv("gs_08_stackbase", rdgs(0x08));
  kv("gs_10_stacklimit", rdgs(0x10));
  kv("gs_18_self", rdgs(0x18));
  kv("gs_20_client", rdgs(0x20));
  kv("gs_00_nttib", rdgs(0x00));
  u64 sp;
  __asm__ volatile("movq %%rsp, %0" : "=r"(sp));
  kv("rsp", sp);

  HANDLE h = CreateFileA("C:\\gsprobe.txt", 0x40000000 /*GENERIC_WRITE*/,
                         1 /*FILE_SHARE_READ*/, 0, 2 /*CREATE_ALWAYS*/, 0x80, 0);
  if (h == (HANDLE)(i64)-1) {
    buf[blen++] = 'e'; buf[blen++] = 'r'; buf[blen++] = 'r'; buf[blen++] = '=';
    blen += hex(buf + blen, (u64)GetLastError());
    buf[blen++] = '\n';
    ExitProcess(2);
  }
  u32 wrote = 0;
  WriteFile(h, buf, blen, &wrote, 0);
  CloseHandle(h);
  ExitProcess(0);
}
