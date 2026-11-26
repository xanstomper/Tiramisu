#include "../src/trt/trt.h"
#include "../src/trt/pe.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
extern "C" u32 TRT_STDCALL trt_FormatMessageW(u32,const void*,u32,u32,u16*,u32,void*);
extern "C" void* TRT_STDCALL trt_LocalFree(void*);
extern "C" u32 TRT_STDCALL trt_GetLastError(void);
static void* alloc(size_t n,u64) { return calloc(1,n); }
static int protect(void*,size_t,unsigned) { return 0; }
static void release(void* p,size_t) { free(p); }
static void logline(const char*) {}
int main() {
  trt_backend be{alloc,protect,release,logline,nullptr}; trt_bridge_boot(&be);
  int fails=0;
  struct Output { u16* msg; unsigned long long guard; } out{nullptr,0xa1b2c3d4e5f60718ULL};
  u32 n=trt_FormatMessageW(0x1100,nullptr,2,0,reinterpret_cast<u16*>(&out.msg),0,nullptr);
  const u16 expected[]={'e','r','r','o','r',' ','2',0};
  if (n!=7 || !out.msg || memcmp(out.msg,expected,sizeof(expected)) || out.guard!=0xa1b2c3d4e5f60718ULL) { puts("FAIL allocated message"); ++fails; }
  if (out.msg && trt_LocalFree(out.msg)!=nullptr) { puts("FAIL LocalFree"); ++fails; }
  u16 buf[16]={}; n=trt_FormatMessageW(0x1000,nullptr,2,0,buf,16,nullptr);
  if (n!=7 || memcmp(buf,expected,sizeof(expected))) { puts("FAIL fixed buffer"); ++fails; }
  u16 small[3]={9,9,9}; n=trt_FormatMessageW(0x1000,nullptr,2,0,small,3,nullptr);
  if (n || trt_GetLastError()!=122 || small[0]!=9) { puts("FAIL short buffer"); ++fails; }
  n=trt_FormatMessageW(0x1100,nullptr,2,0,nullptr,0,nullptr);
  if (n || trt_GetLastError()!=87) { puts("FAIL null buffer"); ++fails; }
  printf("FormatMessageW checks: %s\n",fails?"FAIL":"PASS"); return fails?1:0;
}
