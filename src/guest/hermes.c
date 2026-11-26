// tiramisu/src/guest/hermes.c
// Hermes — a native Win64 tool-using agent guest for the Tiramisu runtime.
//
// Clean-room, freestanding C (no CRT, no libc): the Tiramisu PE64 loader maps
// this image straight into the UWP host and binds kernel32/ws2_32. The agent
// loop, JSON codec, HTTP client and file tools all run on the console; only
// the model inference crosses the LAN to an OpenAI-compatible relay.
//
// Config:  C:\hermes\config.ini      (key=value lines)
// Task:    C:\hermes\task.txt        (fallback if config has no task=)
// Output:  C:\hermes\out\answer.md   (final assistant message)
// Log:     C:\hermes\hermes.log      (append-only, survives a hot launch)
// Status:  C:\hermes\status.json     (rewritten every step)
//
//
// Tools: list_dir, read_file, write_file, append_file, make_dir, stat,
//        delete_file. Paths are confined to the workspace unless absolute.
//
// Attribution: the tool-calling protocol follows the public OpenAI Chat
// Completions schema (JSON only, no code copied). Built for Tiramisu (MIT).

typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef unsigned long long u64;
typedef long long          i64;
typedef void*              HANDLE;
typedef int                BOOL;

#define H_INVALID ((HANDLE)(i64)-1)

#ifndef HERMES_BRAND
#define HERMES_BRAND "Hermes"
#endif
#ifndef HERMES_ROOT
#define HERMES_ROOT "C:\\hermes"
#endif

// Terminal front end (hermes_ui.c). term_init is safe before any other call;
// every write is dropped until the device exists.
extern int  term_init(void);
extern int  term_ready(void);
extern void term_write(const char* s);
extern void term_write_c(const char* s, int color);
extern void term_status(const char* s);
extern void term_clear(void);
extern int  term_poll(void);
extern const char* term_line(void);
extern void term_push_history(const char* s);

// ---- kernel32 imports -------------------------------------------------------
__attribute__((dllimport)) HANDLE GetProcessHeap(void);
__attribute__((dllimport)) void*  HeapAlloc(HANDLE, u32, u64);
__attribute__((dllimport)) BOOL   HeapFree(HANDLE, u32, void*);
__attribute__((dllimport)) HANDLE CreateFileA(const char*, u32, u32, void*, u32, u32, HANDLE);
__attribute__((dllimport)) BOOL   ReadFile(HANDLE, void*, u32, u32*, void*);
__attribute__((dllimport)) BOOL   WriteFile(HANDLE, const void*, u32, u32*, void*);
__attribute__((dllimport)) u32    SetFilePointer(HANDLE, int, int*, u32);
__attribute__((dllimport)) BOOL   CloseHandle(HANDLE);
__attribute__((dllimport)) BOOL   CreateDirectoryA(const char*, void*);
__attribute__((dllimport)) HANDLE FindFirstFileW(const u16*, void*);
__attribute__((dllimport)) BOOL   FindNextFileW(HANDLE, void*);
__attribute__((dllimport)) BOOL   FindClose(HANDLE);
__attribute__((dllimport)) u32    GetFileAttributesA(const char*);
__attribute__((dllimport)) BOOL   DeleteFileA(const char*);
__attribute__((dllimport)) u64    GetTickCount64(void);
__attribute__((dllimport)) void   Sleep(u32);
__attribute__((dllimport)) u32    GetModuleFileNameA(void*, char*, u32);
__attribute__((dllimport)) u32    GetLastError(void);
__attribute__((dllimport)) u32    GetCurrentProcessId(void);
__attribute__((dllimport)) u32    RtlGetVersion(void*);

// ---- ws2_32 imports ---------------------------------------------------------
__attribute__((dllimport)) int  WSAStartup(u16, void*);
__attribute__((dllimport)) int  WSACleanup(void);
__attribute__((dllimport)) u64  socket(int, int, int);
__attribute__((dllimport)) int  connect(u64, const void*, int);
__attribute__((dllimport)) int  send(u64, const char*, int, int);
__attribute__((dllimport)) int  recv(u64, char*, int, int);
__attribute__((dllimport)) int  closesocket(u64);
__attribute__((dllimport)) int  getaddrinfo(const char*, const char*, const void*, void**);
__attribute__((dllimport)) void freeaddrinfo(void*);
__attribute__((dllimport)) int  WSAGetLastError(void);

#define AF_INET     2
#define SOCK_STREAM 1
#define IPPROTO_TCP 6
#define INVALID_SOCK ((u64)~(u64)0)

#define OPEN_EXISTING  3
#define OPEN_ALWAYS    4
#define CREATE_ALWAYS  2
#define FILE_APPEND_DATA 4
#define GENERIC_READ   0x80000000u
#define GENERIC_WRITE  0x40000000u
#define FILE_SHARE_READ 1
#define FILE_ATTRIBUTE_NORMAL 0x80u
#define FILE_ATTRIBUTE_DIRECTORY 0x10u

typedef struct { u32 lo, hi; } FILETIME;
typedef struct {
  u32 dwFileAttributes;
  FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime;
  u32 nFileSizeHigh, nFileSizeLow, dwReserved0, dwReserved1;
  char cFileName[260];
  char cAlternateFileName[14];
  char _pad[2];
} FIND;

typedef struct addrinfo_s {
  int ai_flags, ai_family, ai_socktype, ai_protocol;
  u64 ai_addrlen;
  char* ai_canonname;
  void* ai_addr;
  struct addrinfo_s* ai_next;
} ADDRINFO;

// ---- tiny runtime -----------------------------------------------------------
static HANDLE g_heap;
static void* xalloc(u64 n) { return HeapAlloc(g_heap, 0, n); }
static void  xfree(void* p) { if (p) HeapFree(g_heap, 0, p); }

static void* mc(void* d, const void* s, u64 n) {
  u8* a = (u8*)d; const u8* b = (const u8*)s;
  while (n--) *a++ = *b++;
  return d;
}
static void* mz(void* d, u64 n) {
  u8* a = (u8*)d; while (n--) *a++ = 0; return d;
}
static u64 slen(const char* s) { u64 n = 0; while (s && s[n]) ++n; return n; }
static int scmp(const char* a, const char* b) {
  while (*a && *a == *b) { ++a; ++b; }
  return (int)(u8)*a - (int)(u8)*b;
}
static int sncmp(const char* a, const char* b, u64 n) {
  while (n-- && *a && *a == *b) { ++a; ++b; }
  return n == (u64)~(u64)0 ? 0 : (int)(u8)*a - (int)(u8)*b;
}
static char* scpy(char* d, const char* s) { while ((*d++ = *s++)) {} return d - 1; }

// ---- string builder ---------------------------------------------------------
typedef struct { char* p; u32 len, cap; int ok; } SB;
static void sb_init(SB* b, u32 cap) {
  b->p = (char*)xalloc(cap); b->len = 0; b->cap = cap; b->ok = b->p != 0;
  if (b->ok) b->p[0] = 0;
}
static void sb_need(SB* b, u32 n) {
  if (!b->ok || b->len + n + 1 > b->cap) { b->ok = 0; return; }
}
static void sb_putc(SB* b, char c) { sb_need(b, 1); if (b->ok) { b->p[b->len++] = c; b->p[b->len] = 0; } }
static void sb_put(SB* b, const char* s) { u32 n = (u32)slen(s); sb_need(b, n); if (b->ok) { mc(b->p + b->len, s, n); b->len += n; b->p[b->len] = 0; } }
static void sb_putn(SB* b, const char* s, u32 n) { sb_need(b, n); if (b->ok) { mc(b->p + b->len, s, n); b->len += n; b->p[b->len] = 0; } }
static void sb_u64(SB* b, u64 v) {
  char t[24]; int i = 0;
  if (!v) { sb_putc(b, '0'); return; }
  while (v) { t[i++] = (char)('0' + (v % 10)); v /= 10; }
  while (i--) sb_putc(b, t[i]);
}
static void sb_i64(SB* b, i64 v) {
  if (v < 0) { sb_putc(b, '-'); sb_u64(b, (u64)(-v)); } else sb_u64(b, (u64)v);
}
static void sb_json(SB* b, const char* s) {
  sb_putc(b, '"');
  for (; s && *s; ++s) {
    unsigned char c = (unsigned char)*s;
    if (c == '"' || c == '\\') { sb_putc(b, '\\'); sb_putc(b, (char)c); }
    else if (c == '\n') sb_put(b, "\\n");
    else if (c == '\r') sb_put(b, "\\r");
    else if (c == '\t') sb_put(b, "\\t");
    else if (c < 0x20) {
      static const char hx[] = "0123456789abcdef";
      sb_put(b, "\\u00"); sb_putc(b, hx[c >> 4]); sb_putc(b, hx[c & 15]);
    } else sb_putc(b, (char)c);
  }
  sb_putc(b, '"');
}

// ---- file helpers -----------------------------------------------------------
static int read_all(const char* path, char** out, u32* outn) {
  HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
  if (h == H_INVALID) return 0;
  u32 cap = 65536, n = 0, got = 0;
  char* buf = (char*)xalloc(cap + 1);
  if (!buf) { CloseHandle(h); return 0; }
  for (;;) {
    if (n + 4096 > cap) {
      u32 nc = cap * 2;
      char* nb = (char*)xalloc(nc + 1);
      if (!nb) { xfree(buf); CloseHandle(h); return 0; }
      mc(nb, buf, n); xfree(buf); buf = nb; cap = nc;
    }
    if (!ReadFile(h, buf + n, cap - n, &got, 0) || got == 0) break;
    n += got;
    if (n > 4u * 1024 * 1024) break;  // hard cap: 4 MiB
  }
  CloseHandle(h);
  buf[n] = 0;
  *out = buf; *outn = n;
  return 1;
}

static int write_all(const char* path, const char* data, u32 n, u32 mode) {
  HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, 0, mode, FILE_ATTRIBUTE_NORMAL, 0);
  if (h == H_INVALID) return 0;
  u32 done = 0, w = 0;
  int ok = 1;
  while (done < n) {
    if (!WriteFile(h, data + done, n - done, &w, 0)) { ok = 0; break; }
    done += w;
  }
  CloseHandle(h);
  return ok;
}

static char g_token[256];
static HANDLE g_log = H_INVALID;
static u64 g_t0;
static void logf(const char* msg) {
  if (g_log == H_INVALID) return;
  SB b; sb_init(&b, 4096);
  if (!b.ok) return;
  sb_put(&b, "[+"); sb_u64(&b, GetTickCount64() - g_t0); sb_put(&b, "ms] "); sb_put(&b, msg); sb_put(&b, "\r\n");
  SetFilePointer(g_log, 0, 0, 2 /*FILE_END*/);
  u32 w = 0; WriteFile(g_log, b.p, b.len, &w, 0);
  xfree(b.p);
}
static void logkv(const char* k, const char* v) {
  SB b; sb_init(&b, 4096);
  if (!b.ok) return;
  sb_put(&b, "[+"); sb_u64(&b, GetTickCount64() - g_t0); sb_put(&b, "ms] "); sb_put(&b, k); sb_put(&b, "="); sb_put(&b, v ? v : ""); sb_put(&b, "\r\n");
  u32 w = 0; if (g_log != H_INVALID) { SetFilePointer(g_log, 0, 0, 2 /*FILE_END*/); WriteFile(g_log, b.p, b.len, &w, 0); }
  xfree(b.p);
}

static void mkdirs(const char* p) { CreateDirectoryA(p, 0); }

// ---- JSON parser (arena DOM) ------------------------------------------------
enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ };
typedef struct jn {
  int t;
  const char* s; int n;          // string / number slice (unescaped for J_STR)
  struct jn* kids; struct jn* next;
  const char* key; int keyn;
} JN;

typedef struct { const char* p; const char* end; char* arena; u32 used, cap; int depth; } JP;

static void* ja(JP* j, u32 n) {
  n = (n + 7u) & ~7u;
  if (j->used + n > j->cap) return 0;
  void* p = j->arena + j->used; j->used += n; mz(p, n); return p;
}
static JN* jnode(JP* j, int t) { JN* n = (JN*)ja(j, sizeof(JN)); if (n) n->t = t; return n; }

static void jws(JP* j) {
  while (j->p < j->end) {
    char c = *j->p;
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { ++j->p; continue; }
    break;
  }
}
static int jhex(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}
static int jstr(JP* j, const char** out, int* outn) {
  if (j->p >= j->end || *j->p != '"') return 0;
  ++j->p;
  char* w = (char*)j->p;         // unescape in place; result never grows
  const char* start = w;
  while (j->p < j->end) {
    unsigned char c = (unsigned char)*j->p++;
    if (c == '"') { *w = 0; *out = start; *outn = (int)(w - start); return 1; }
    if (c != '\\') { *w++ = (char)c; continue; }
    if (j->p >= j->end) return 0;
    char e = *j->p++;
    switch (e) {
      case '"': *w++ = '"'; break;
      case '\\': *w++ = '\\'; break;
      case '/': *w++ = '/'; break;
      case 'b': *w++ = '\b'; break;
      case 'f': *w++ = '\f'; break;
      case 'n': *w++ = '\n'; break;
      case 'r': *w++ = '\r'; break;
      case 't': *w++ = '\t'; break;
      case 'u': {
        if (j->end - j->p < 4) return 0;
        int h0 = jhex(j->p[0]), h1 = jhex(j->p[1]), h2 = jhex(j->p[2]), h3 = jhex(j->p[3]);
        if (h0 < 0 || h1 < 0 || h2 < 0 || h3 < 0) return 0;
        j->p += 4;
        u32 cp = (u32)((h0 << 12) | (h1 << 8) | (h2 << 4) | h3);
        if (cp < 0x80) *w++ = (char)cp;
        else if (cp < 0x800) { *w++ = (char)(0xC0 | (cp >> 6)); *w++ = (char)(0x80 | (cp & 63)); }
        else { *w++ = (char)(0xE0 | (cp >> 12)); *w++ = (char)(0x80 | ((cp >> 6) & 63)); *w++ = (char)(0x80 | (cp & 63)); }
        break;
      }
      default: return 0;
    }
  }
  return 0;
}
static JN* jval(JP* j);

static JN* jarr(JP* j) {
  JN* a = jnode(j, J_ARR); if (!a) return 0;
  ++j->p; jws(j);
  if (j->p < j->end && *j->p == ']') { ++j->p; return a; }
  JN* head = 0; JN* tail = 0;
  for (;;) {
    JN* v = jval(j); if (!v) return 0;
    if (!head) head = v; else tail->next = v;
    tail = v;
    jws(j);
    if (j->p < j->end && *j->p == ',') { ++j->p; continue; }
    if (j->p < j->end && *j->p == ']') { ++j->p; break; }
    return 0;
  }
  a->kids = head;
  return a;
}
static JN* jobj(JP* j) {
  JN* o = jnode(j, J_OBJ); if (!o) return 0;
  ++j->p; jws(j);
  if (j->p < j->end && *j->p == '}') { ++j->p; return o; }
  JN* head = 0; JN* tail = 0;
  for (;;) {
    jws(j);
    const char* k = 0; int kn = 0;
    if (!jstr(j, &k, &kn)) return 0;
    jws(j);
    if (j->p >= j->end || *j->p != ':') return 0;
    ++j->p;
    JN* v = jval(j); if (!v) return 0;
    v->key = k; v->keyn = kn;
    if (!head) head = v; else tail->next = v;
    tail = v;
    jws(j);
    if (j->p < j->end && *j->p == ',') { ++j->p; continue; }
    if (j->p < j->end && *j->p == '}') { ++j->p; break; }
    return 0;
  }
  o->kids = head;
  return o;
}
static JN* jval(JP* j) {
  if (j->depth++ > 48) return 0;
  jws(j);
  if (j->p >= j->end) return 0;
  char c = *j->p;
  JN* n = 0;
  if (c == '{') n = jobj(j);
  else if (c == '[') n = jarr(j);
  else if (c == '"') {
    const char* s = 0; int sn = 0;
    if (!jstr(j, &s, &sn)) return 0;
    n = jnode(j, J_STR); if (n) { n->s = s; n->n = sn; }
  } else if (c == 't' && j->end - j->p >= 4 && sncmp(j->p, "true", 4) == 0) {
    j->p += 4; n = jnode(j, J_BOOL); if (n) n->n = 1;
  } else if (c == 'f' && j->end - j->p >= 5 && sncmp(j->p, "false", 5) == 0) {
    j->p += 5; n = jnode(j, J_BOOL); if (n) n->n = 0;
  } else if (c == 'n' && j->end - j->p >= 4 && sncmp(j->p, "null", 4) == 0) {
    j->p += 4; n = jnode(j, J_NULL);
  } else {
    const char* st = j->p;
    if (*j->p == '-' || (*j->p >= '0' && *j->p <= '9')) {
      ++j->p;
      while (j->p < j->end && ((*j->p >= '0' && *j->p <= '9') || *j->p == '.' || *j->p == 'e' || *j->p == 'E' || *j->p == '+' || *j->p == '-')) ++j->p;
      n = jnode(j, J_NUM); if (n) { n->s = st; n->n = (int)(j->p - st); }
    }
  }
  --j->depth;
  return n;
}

static JN* jget(JN* o, const char* key) {
  if (!o || o->t != J_OBJ) return 0;
  u64 kn = slen(key);
  for (JN* k = o->kids; k; k = k->next)
    if (k->key && (u64)k->keyn == kn && sncmp(k->key, key, kn) == 0) return k;
  return 0;
}
static JN* jat(JN* a, int i) {
  if (!a || a->t != J_ARR) return 0;
  for (JN* k = a->kids; k; k = k->next) { if (i-- == 0) return k; }
  return 0;
}
static const char* jstrp(JN* n, int* len) {
  if (!n || n->t != J_STR) { if (len) *len = 0; return ""; }
  if (len) *len = n->n;
  return n->s;
}

// ---- HTTP client ------------------------------------------------------------
// Only plain HTTP: TLS termination is the relay's job on the PC. Returns the
// response body (heap) or 0; *status gets the numeric HTTP status.
static char* http_exchange(const char* url, const char* body, u32 bodylen, int* status, u32* outn) {
  *status = 0; *outn = 0;
  if (sncmp(url, "http://", 7) != 0) return 0;
  const char* p = url + 7;
  const char* host0 = p;
  while (*p && *p != ':' && *p != '/') ++p;
  char host[128]; u32 hn = (u32)(p - host0);
  if (hn == 0 || hn >= sizeof(host)) return 0;
  mc(host, host0, hn); host[hn] = 0;
  int port = 80;
  if (*p == ':') { ++p; port = 0; while (*p >= '0' && *p <= '9') port = port * 10 + (*p++ - '0'); }
  const char* path = (*p == '/') ? p : "/";

  ADDRINFO hints; mz(&hints, sizeof(hints));
  hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM; hints.ai_protocol = IPPROTO_TCP;
  char ps[8]; int pi = 0; int q = port;
  char t[8]; int ti = 0;
  if (q == 0) return 0;
  while (q) { t[ti++] = (char)('0' + q % 10); q /= 10; }
  while (ti) ps[pi++] = t[--ti];
  ps[pi] = 0;

  void* res = 0;
  int gr = getaddrinfo(host, ps, &hints, &res);
  if (gr != 0 || !res) { SB d; sb_init(&d, 128); if (d.ok) { sb_put(&d, "getaddrinfo failed: "); sb_i64(&d, (i64)gr); sb_put(&d, " wsa="); sb_i64(&d, (i64)WSAGetLastError()); logf(d.p); xfree(d.p); } return 0; }

  u64 s = INVALID_SOCK;
  int lasterr = 0;
  for (ADDRINFO* ai = (ADDRINFO*)res; ai; ai = ai->ai_next) {
    s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (s == INVALID_SOCK) { lasterr = WSAGetLastError(); continue; }
    {
      u8* b = (u8*)ai->ai_addr;
      SB d; sb_init(&d, 160);
      if (d.ok) {
        sb_put(&d, "addrlen="); sb_u64(&d, ai->ai_addrlen);
        sb_put(&d, " fam="); sb_u64(&d, (u64)(b ? (b[0] | (b[1] << 8)) : 0));
        sb_put(&d, " port="); sb_u64(&d, (u64)(b ? ((b[2] << 8) | b[3]) : 0));
        sb_put(&d, " ip=");
        if (b) { sb_u64(&d, b[4]); sb_put(&d, "."); sb_u64(&d, b[5]); sb_put(&d, "."); sb_u64(&d, b[6]); sb_put(&d, "."); sb_u64(&d, b[7]); }
        logf(d.p);
        xfree(d.p);
      }
    }
    if (connect(s, ai->ai_addr, (int)ai->ai_addrlen) == 0) break;
    lasterr = WSAGetLastError();
    closesocket(s); s = INVALID_SOCK;
  }
  freeaddrinfo(res);
  logf("http: connected");
  if (s == INVALID_SOCK) { SB d; sb_init(&d, 128); if (d.ok) { sb_put(&d, "connect failed wsa="); sb_i64(&d, (i64)lasterr); logf(d.p); xfree(d.p); } return 0; }

  SB head; sb_init(&head, 1024);
  if (!head.ok) { closesocket(s); return 0; }
  sb_put(&head, "POST "); sb_put(&head, path); sb_put(&head, " HTTP/1.1\r\nHost: ");
  sb_put(&head, host);
  if (port != 80) { sb_put(&head, ":"); sb_u64(&head, (u64)port); }
  sb_put(&head, "\r\nContent-Type: application/json\r\nAccept: application/json\r\n");
  if (g_token[0]) { sb_put(&head, "Authorization: Bearer "); sb_put(&head, g_token); sb_put(&head, "\r\n"); }
  sb_put(&head, "Content-Length: ");
  sb_u64(&head, bodylen);
  sb_put(&head, "\r\nConnection: close\r\n\r\n");
  if (!head.ok) { xfree(head.p); closesocket(s); return 0; }

  u32 sent = 0;
  while (sent < head.len) { int n = send(s, head.p + sent, (int)(head.len - sent), 0); if (n <= 0) break; sent += (u32)n; }
  xfree(head.p);
  logf("http: headers sent");
  if (sent < head.len) { closesocket(s); return 0; }
  sent = 0;
  while (sent < bodylen) { int n = send(s, body + sent, (int)(bodylen - sent), 0); if (n <= 0) break; sent += (u32)n; }
  logf("http: body sent");
  if (sent < bodylen) { closesocket(s); return 0; }
  { SB d; sb_init(&d, 128); if (d.ok) { sb_put(&d, "http: sent "); sb_u64(&d, bodylen); sb_put(&d, " bytes"); logf(d.p); xfree(d.p); } }

  u32 cap = 262144, n = 0;
  char* in = (char*)xalloc(cap + 1);
  if (!in) { closesocket(s); return 0; }
  for (;;) {
    if (n + 8192 > cap) {
      u32 nc = cap * 2;
      char* nb = (char*)xalloc(nc + 1);
      if (!nb) break;
      mc(nb, in, n); xfree(in); in = nb; cap = nc;
    }
    int r = recv(s, in + n, (int)(cap - n), 0);
    if (r <= 0) break;
    n += (u32)r;
    // Stop as soon as Content-Length is satisfied. aiohttp (Hermes api_server)
    // may keep the keep-alive socket open despite our Connection: close, which
    // otherwise leaves recv() blocked forever and the agent stuck at "request".
    char* hb2 = 0;
    for (u32 i = 0; i + 3 < n; ++i) if (in[i] == '\r' && in[i+1] == '\n' && in[i+2] == '\r' && in[i+3] == '\n') { hb2 = in + i + 4; break; }
    if (hb2) {
      u32 want = 0;
      for (u32 i = 0; i + 15 < (u32)(hb2 - in); ++i) {
        if (sncmp(in + i, "content-length:", 15) == 0) {
          char* q2 = in + i + 15;
          while (*q2 == ' ') ++q2;
          while (*q2 >= '0' && *q2 <= '9') want = want * 10 + (u32)(*q2++ - '0');
          break;
        }
      }
      if (want && n >= (u32)(hb2 - in) + want) break;
    }
    if (n > 8u * 1024 * 1024) break;
  }
  closesocket(s);
  { SB d; sb_init(&d, 128); if (d.ok) { sb_put(&d, "http: recv "); sb_u64(&d, n); sb_put(&d, " bytes"); logf(d.p); xfree(d.p); } }
  in[n] = 0;

  // status line
  char* sp = in;
  while (sp < in + n && *sp != ' ') ++sp;
  if (sp < in + n) *status = 0, ++sp;
  while (sp < in + n && *sp >= '0' && *sp <= '9') { *status = *status * 10 + (*sp - '0'); ++sp; }

  // find header/body split
  char* hb = 0;
  for (u32 i = 0; i + 3 < n; ++i) if (in[i] == '\r' && in[i + 1] == '\n' && in[i + 2] == '\r' && in[i + 3] == '\n') { hb = in + i + 4; break; }
  if (!hb) { xfree(in); return 0; }

  u32 blen = (u32)(in + n - hb);
  // Content-Length wins when the peer keeps the connection open
  for (u32 i = 0; i + 15 < (u32)(hb - in); ++i) {
    if (sncmp(in + i, "content-length:", 15) == 0) {
      u32 v = 0; char* q2 = in + i + 15;
      while (*q2 == ' ') ++q2;
      while (*q2 >= '0' && *q2 <= '9') v = v * 10 + (*q2++ - '0');
      if (v <= (u32)(in + n - hb)) blen = v;
      break;
    }
  }
  // chunked
  int chunked = 0;
  for (u32 i = 0; i + 18 < (u32)(hb - in); ++i)
    if (sncmp(in + i, "transfer-encoding:", 18) == 0) {
      for (u32 k = i; k + 7 < (u32)(hb - in); ++k)
        if (sncmp(in + k, "chunked", 7) == 0) { chunked = 1; break; }
      break;
    }

  char* out;
  if (chunked) {
    out = (char*)xalloc(blen + 1);
    if (!out) { xfree(in); return 0; }
    u32 o = 0; char* q2 = hb; char* end = hb + blen;
    while (q2 < end) {
      u32 sz = 0;
      while (q2 < end && *q2 && *q2 != '\r') {
        char c = *q2++;
        if (c >= '0' && c <= '9') sz = sz * 16 + (u32)(c - '0');
        else if (c >= 'a' && c <= 'f') sz = sz * 16 + (u32)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') sz = sz * 16 + (u32)(c - 'A' + 10);
        else break;
      }
      while (q2 < end && *q2 != '\n') ++q2;
      if (q2 < end) ++q2;
      if (sz == 0) break;
      u32 take = sz;
      if (q2 + take > end) take = (u32)(end - q2);
      mc(out + o, q2, take); o += take;
      q2 += take;
      while (q2 < end && *q2 != '\n') ++q2;
      if (q2 < end) ++q2;
    }
    out[o] = 0; *outn = o;
  } else {
    out = (char*)xalloc(blen + 1);
    if (!out) { xfree(in); return 0; }
    mc(out, hb, blen); out[blen] = 0; *outn = blen;
  }
  xfree(in);
  return out;
}

// ---- config -----------------------------------------------------------------
typedef struct {
  char endpoint[256];
  char model[96];
  char workspace[200];
  char task[4096];
  char token[256];
  char lang[32];
  int max_steps;
} CFG;

static void cfg_default(CFG* c) {
  mz(c, sizeof(*c));
  scpy(c->endpoint, "http://10.0.0.97:8790/v1/chat/completions");
  scpy(c->model, "hermes");
  scpy(c->workspace, HERMES_ROOT "\\workspace");
  scpy(c->lang, "en");
  c->max_steps = 12;
}
static void trim(char* s) {
  u32 n = (u32)slen(s);
  while (n && (s[n - 1] == '\r' || s[n - 1] == '\n' || s[n - 1] == ' ' || s[n - 1] == '\t')) s[--n] = 0;
  u32 i = 0; while (s[i] == ' ' || s[i] == '\t') ++i;
  if (i) { u32 k = 0; while (s[i]) s[k++] = s[i++]; s[k] = 0; }
}
// The package lives on Z: (the game folder); C: is the writable drive. Prefer
// the config shipped beside the exe so an uploaded zip is self-contained.
static void exe_side(const char* leaf, char* out, u32 cap) {
  char exe[300];
  u32 n = GetModuleFileNameA(0, exe, sizeof(exe));
  if (n == 0 || n >= sizeof(exe)) { out[0] = 0; return; }
  exe[n] = 0;
  int cut = -1;
  for (int i = 0; exe[i]; ++i) if (exe[i] == '\\' || exe[i] == '/') cut = i;
  u32 k = 0;
  for (int i = 0; i <= cut && k + 1 < cap; ++i) out[k++] = exe[i];
  for (int i = 0; leaf[i] && k + 1 < cap; ++i) out[k++] = leaf[i];
  out[k] = 0;
}

static int read_first(const char* a, const char* b, char** out, u32* outn) {
  if (a && a[0] && read_all(a, out, outn)) return 1;
  if (b && b[0] && read_all(b, out, outn)) return 1;
  return 0;
}

static void cfg_load(CFG* c) {
  cfg_default(c);
  char* txt = 0; u32 n = 0;
  char side[300]; exe_side("config.ini", side, sizeof(side));
  if (!read_first(side, HERMES_ROOT "\\config.ini", &txt, &n)) return;
  char* p = txt;
  while (p < txt + n) {
    char* e = p; while (e < txt + n && *e != '\n') ++e;
    char save = *e; *e = 0;
    trim(p);
    if (*p && *p != '#' && *p != '[') {
      char* eq = p; while (*eq && *eq != '=') ++eq;
      if (*eq == '=') {
        *eq = 0; char* k = p; char* v = eq + 1; trim(k); trim(v);
        if (scmp(k, "endpoint") == 0) { scpy(c->endpoint, v); }
        else if (scmp(k, "model") == 0) { scpy(c->model, v); }
        else if (scmp(k, "workspace") == 0) { scpy(c->workspace, v); }
        else if (scmp(k, "task") == 0) { scpy(c->task, v); }
        else if (scmp(k, "token") == 0) { scpy(c->token, v); scpy(g_token, v); }
        else if (scmp(k, "lang") == 0) { scpy(c->lang, v); }
        else if (scmp(k, "max_steps") == 0) { int m = 0; while (*v >= '0' && *v <= '9') m = m * 10 + (*v++ - '0'); if (m > 0 && m <= 64) c->max_steps = m; }
      }
    }
    *e = save;
    p = e + (e < txt + n ? 1 : 0);
  }
  xfree(txt);
  if (!c->task[0]) {
    char* tt = 0; u32 tn = 0;
    char tside[300]; exe_side("task.txt", tside, sizeof(tside));
    if (read_first(tside, HERMES_ROOT "\\task.txt", &tt, &tn)) {
      u32 k = 0; while (k < tn && k < sizeof(c->task) - 1) { c->task[k] = tt[k]; ++k; }
      c->task[k] = 0; trim(c->task); xfree(tt);
    }
  }
}

// ---- tools ------------------------------------------------------------------
static int is_abs(const char* p) {
  return (p[0] && p[1] == ':') || (p[0] == '\\' && p[1] == '\\');
}
// Join cfg.workspace with a relative path; reject traversal.
static int resolve_path(const CFG* c, const char* in, char* out, u32 cap) {
  if (!in || !*in) in = ".";
  if (is_abs(in)) {
    if (slen(in) + 1 > cap) return 0;
    scpy(out, in); return 1;
  }
  u64 wl = slen(c->workspace), il = slen(in);
  if (il > 2000 || wl + il + 2 > cap) return 0;
  // reject .. components
  for (const char* q = in; *q;) {
    const char* s = q; while (*q && *q != '\\' && *q != '/') ++q;
    u64 cn = (u64)(q - s);
    if (cn == 2 && s[0] == '.' && s[1] == '.') return 0;
    if (*q) ++q;
  }
  scpy(out, c->workspace);
  char* w = out + wl;
  if (wl && w[-1] != '\\') *w++ = '\\';
  scpy(w, in);
  return 1;
}

// The W family is the implementation Tiramisu bridges on every host build; the
// ANSI variant only gained a bridge later. Listing therefore goes through W and
// narrows the 592-byte WIN32_FIND_DATAW record itself (ASCII names).
static void to_w(const char* s, u16* w, int cap) {
  int i = 0;
  for (; s && s[i] && i + 1 < cap; ++i) w[i] = (u16)(u8)s[i];
  w[i] = 0;
}
static void w_to_a(const u16* w, char* a, int cap) {
  int i = 0;
  for (; w && w[i] && i + 1 < cap; ++i) a[i] = (char)w[i];
  a[i] = 0;
}
static void tool_list_dir(const CFG* c, const char* path, SB* out) {
  char full[512]; if (!resolve_path(c, path, full, sizeof(full))) { sb_put(out, "error: bad path"); return; }
  char pat[560]; scpy(pat, full); u32 l = (u32)slen(pat);
  if (l && pat[l - 1] != '\\') { pat[l++] = '\\'; pat[l] = 0; }
  scpy(pat + l, "*");
  u16 wpat[560]; to_w(pat, wpat, 560);
  u8 fd[592]; mz(fd, sizeof(fd));
  HANDLE h = FindFirstFileW(wpat, fd);
  if (h == H_INVALID) { sb_put(out, "error: cannot list "); sb_put(out, full); return; }
  int count = 0;
  do {
    char nm[260]; w_to_a((const u16*)(fd + 44), nm, sizeof(nm));
    if (nm[0] == '.' && (nm[1] == 0 || (nm[1] == '.' && nm[2] == 0))) continue;
    u32 attrs = *(u32*)(fd + 0);
    if (attrs & FILE_ATTRIBUTE_DIRECTORY) sb_put(out, "[dir]  ");
    else {
      u64 sz = ((u64)(*(u32*)(fd + 28)) << 32) | *(u32*)(fd + 32);
      sb_put(out, "[file] "); sb_u64(out, sz); sb_put(out, "\t");
    }
    sb_put(out, nm); sb_put(out, "\n");
    if (++count >= 400) break;
    mz(fd, sizeof(fd));
  } while (FindNextFileW(h, fd));
  FindClose(h);
  if (!count) sb_put(out, "(empty)\n");
}

static void tool_read_file(const CFG* c, const char* path, SB* out) {
  char full[512]; if (!resolve_path(c, path, full, sizeof(full))) { sb_put(out, "error: bad path"); return; }
  char* data = 0; u32 n = 0;
  if (!read_all(full, &data, &n)) { sb_put(out, "error: cannot read "); sb_put(out, full); return; }
  u32 take = n > 60000 ? 60000 : n;
  sb_putn(out, data, take);
  if (n > take) { sb_put(out, "\n...[truncated "); sb_u64(out, n - take); sb_put(out, " bytes]"); }
  xfree(data);
}

static void tool_write(const CFG* c, const char* path, const char* content, int append, SB* out) {
  char full[512]; if (!resolve_path(c, path, full, sizeof(full))) { sb_put(out, "error: bad path"); return; }
  // make parent dirs for C:\... (one level is enough for most cases)
  u32 l = (u32)slen(content);
  if (!write_all(full, content, l, append ? OPEN_ALWAYS : CREATE_ALWAYS)) {
    sb_put(out, "error: cannot write "); sb_put(out, full); return;
  }
  if (append) { /* FILE_APPEND_DATA handles the offset */ }
  sb_put(out, "ok: wrote "); sb_u64(out, l); sb_put(out, " bytes to "); sb_put(out, full);
}

static void tool_stat(const CFG* c, const char* path, SB* out) {
  char full[512]; if (!resolve_path(c, path, full, sizeof(full))) { sb_put(out, "error: bad path"); return; }
  u32 a = GetFileAttributesA(full);
  if (a == 0xFFFFFFFFu) { sb_put(out, "missing"); return; }
  sb_put(out, (a & FILE_ATTRIBUTE_DIRECTORY) ? "dir" : "file");
}

// ---- multi-language coding helpers -----------------------------------------
static void scat(char* d, const char* s) { u32 k = (u32)slen(d); for (u32 i = 0; s[i]; ++i) d[k++] = s[i]; d[k] = 0; }

static const char* lang_ext(const char* lang) {
  if (!lang || !*lang) return ".txt";
  if (scmp(lang, "python") == 0 || scmp(lang, "py") == 0) return ".py";
  if (scmp(lang, "javascript") == 0 || scmp(lang, "js") == 0) return ".js";
  if (scmp(lang, "typescript") == 0 || scmp(lang, "ts") == 0) return ".ts";
  if (scmp(lang, "c") == 0) return ".c";
  if (scmp(lang, "cpp") == 0 || scmp(lang, "c++") == 0 || scmp(lang, "cxx") == 0) return ".cpp";
  if (scmp(lang, "rust") == 0 || scmp(lang, "rs") == 0) return ".rs";
  if (scmp(lang, "go") == 0 || scmp(lang, "golang") == 0) return ".go";
  if (scmp(lang, "java") == 0) return ".java";
  if (scmp(lang, "csharp") == 0 || scmp(lang, "cs") == 0 || scmp(lang, "c#") == 0) return ".cs";
  if (scmp(lang, "ruby") == 0 || scmp(lang, "rb") == 0) return ".rb";
  if (scmp(lang, "php") == 0) return ".php";
  if (scmp(lang, "shell") == 0 || scmp(lang, "sh") == 0 || scmp(lang, "bash") == 0) return ".sh";
  if (scmp(lang, "html") == 0) return ".html";
  if (scmp(lang, "css") == 0) return ".css";
  if (scmp(lang, "sql") == 0) return ".sql";
  if (scmp(lang, "json") == 0) return ".json";
  if (scmp(lang, "markdown") == 0 || scmp(lang, "md") == 0) return ".md";
  if (scmp(lang, "kotlin") == 0 || scmp(lang, "kt") == 0) return ".kt";
  if (scmp(lang, "swift") == 0) return ".swift";
  if (scmp(lang, "lua") == 0) return ".lua";
  if (scmp(lang, "perl") == 0 || scmp(lang, "pl") == 0) return ".pl";
  if (scmp(lang, "r") == 0) return ".r";
  if (scmp(lang, "scala") == 0) return ".scala";
  if (scmp(lang, "zig") == 0) return ".zig";
  if (scmp(lang, "nim") == 0) return ".nim";
  if (scmp(lang, "dart") == 0) return ".dart";
  return ".txt";
}

static int has_ext(const char* p) {
  const char* b = p;
  for (const char* q = p; *q; ++q) if (*q == '\\' || *q == '/') b = q + 1;
  for (const char* q = b; *q; ++q) if (*q == '.') return 1;
  return 0;
}

static void tool_write_code(const CFG* c, const char* path, const char* language, const char* content, SB* out) {
  char full[512]; if (!resolve_path(c, path, full, sizeof(full))) { sb_put(out, "error: bad path"); return; }
  if (!has_ext(path)) {
    // append extension based on language
    u32 l = (u32)slen(full);
    const char* e = lang_ext(language);
    if (l + slen(e) + 1 < sizeof(full)) { scpy(full + l, e); }
  }
  u32 clen = (u32)slen(content);
  if (!write_all(full, content, clen, CREATE_ALWAYS)) { sb_put(out, "error: cannot write "); sb_put(out, full); return; }
  sb_put(out, "ok: wrote "); sb_u64(out, clen); sb_put(out, " bytes ("); sb_put(out, language && *language ? language : "text");
  sb_put(out, ") to "); sb_put(out, full);
}

static void tool_edit_file(const CFG* c, const char* path, const char* old, const char* newstr, int replace_all, SB* out) {
  char full[512]; if (!resolve_path(c, path, full, sizeof(full))) { sb_put(out, "error: bad path"); return; }
  char* data = 0; u32 n = 0;
  if (!read_all(full, &data, &n)) { sb_put(out, "error: cannot read "); sb_put(out, full); return; }
  u64 on = slen(old);
  SB res; sb_init(&res, n * 2 + 4096);
  if (!res.ok) { xfree(data); sb_put(out, "error: out of memory"); return; }
  int count = 0;
  const char* q = data;
  int stop = 0;
  if (on == 0) {
    sb_put(&res, data);
  } else {
    while (q < data + n && !stop) {
      if (sncmp(q, old, on) == 0) {
        sb_put(&res, newstr ? newstr : "");
        q += on; ++count;
        if (!replace_all) stop = 1;
      } else {
        sb_putc(&res, *q); ++q;
      }
    }
    if (stop) sb_put(&res, q);
  }
  if (count == 0 && on > 0) {
    xfree(data); xfree(res.p);
    sb_put(out, "error: pattern not found in "); sb_put(out, full); return;
  }
  if (!write_all(full, res.p, res.len, CREATE_ALWAYS)) {
    xfree(data); xfree(res.p); sb_put(out, "error: cannot write "); sb_put(out, full); return;
  }
  sb_put(out, "ok: replaced "); sb_i64(out, replace_all ? count : (count > 0 ? 1 : 0));
  sb_put(out, " occurrence(s) in "); sb_put(out, full);
  xfree(data); xfree(res.p);
}

static int contains_ci(const char* h, const char* n) {
  if (!n || !*n) return 1;
  u64 hn = slen(h), nn = slen(n);
  if (nn > hn) return 0;
  for (u64 i = 0; i + nn <= hn; ++i) {
    int ok = 1;
    for (u64 j = 0; j < nn; ++j) {
      char a = h[i+j], b = n[j];
      if (a >= 'A' && a <= 'Z') a += 32;
      if (b >= 'A' && b <= 'Z') b += 32;
      if (a != b) { ok = 0; break; }
    }
    if (ok) return 1;
  }
  return 0;
}

static void search_one_file(const CFG* c, const char* full, const char* query, SB* out, int* found) {
  char* data = 0; u32 n = 0;
  if (!read_all(full, &data, &n)) return;
  u32 line = 1, start = 0;
  for (u32 i = 0; i <= n; ++i) {
    if (i == n || data[i] == '\n') {
      u32 len = i - start;
      char tmp[4096];
      u32 t = len < sizeof(tmp)-1 ? len : sizeof(tmp)-1;
      mc(tmp, data + start, t); tmp[t] = 0;
      if (contains_ci(tmp, query)) {
        sb_put(out, full); sb_put(out, ":"); sb_u64(out, line); sb_put(out, ": ");
        // trim trailing cr
        if (t && tmp[t-1] == '\r') tmp[t-1] = 0;
        sb_put(out, tmp); sb_put(out, "\n");
        (*found)++;
        if (*found >= 200) { xfree(data); return; }
      }
      start = i + 1; ++line;
    }
  }
  xfree(data);
}

static void search_recurse(const CFG* c, const char* dir, const char* query, SB* out, int* found) {
  char pat[560]; scpy(pat, dir);
  u32 l = (u32)slen(pat);
  if (l && pat[l-1] != '\\') { pat[l++] = '\\'; pat[l] = 0; }
  scpy(pat + l, "*");
  u16 wpat[560]; to_w(pat, wpat, 560);
  u8 fd[592]; mz(fd, sizeof(fd));
  HANDLE h = FindFirstFileW(wpat, fd);
  if (h == H_INVALID) return;
  // collect entries first
  char names[400][260];
  u32 attrs[400];
  int nn = 0;
  do {
    char nm[260]; w_to_a((const u16*)(fd + 44), nm, sizeof(nm));
    if (nm[0] == '.' && (nm[1] == 0 || (nm[1] == '.' && nm[2] == 0))) { mz(fd, sizeof(fd)); continue; }
    if (nn < 400) { scpy(names[nn], nm); attrs[nn] = *(u32*)(fd + 0); ++nn; }
    mz(fd, sizeof(fd));
  } while (FindNextFileW(h, fd));
  FindClose(h);
  for (int i = 0; i < nn; ++i) {
    char child[560]; scpy(child, dir);
    u32 cl = (u32)slen(child);
    if (cl && child[cl-1] != '\\') { child[cl++] = '\\'; child[cl] = 0; }
    scpy(child + cl, names[i]);
    if (attrs[i] & FILE_ATTRIBUTE_DIRECTORY) {
      search_recurse(c, child, query, out, found);
      if (*found >= 200) return;
    } else {
      search_one_file(c, child, query, out, found);
      if (*found >= 200) return;
    }
  }
}

static void tool_search_files(const CFG* c, const char* path, const char* query, SB* out) {
  char full[512]; if (!resolve_path(c, path && *path ? path : ".", full, sizeof(full))) { sb_put(out, "error: bad path"); return; }
  if (!query || !*query) { sb_put(out, "error: query required"); return; }
  int found = 0;
  u32 a = GetFileAttributesA(full);
  if (a != 0xFFFFFFFFu && !(a & FILE_ATTRIBUTE_DIRECTORY)) {
    search_one_file(c, full, query, out, &found);
  } else {
    search_recurse(c, full, query, out, &found);
  }
  if (!found) sb_put(out, "(no matches)\n");
  else { sb_put(out, "--- "); sb_i64(out, found); sb_put(out, " match(es) ---\n"); }
}

static void tree_recurse(const CFG* c, const char* dir, int depth, int maxdepth, const char* prefix, SB* out) {
  char pat[560]; scpy(pat, dir);
  u32 l = (u32)slen(pat);
  if (l && pat[l-1] != '\\') { pat[l++] = '\\'; pat[l] = 0; }
  scpy(pat + l, "*");
  u16 wpat[560]; to_w(pat, wpat, 560);
  u8 fd[592]; mz(fd, sizeof(fd));
  HANDLE h = FindFirstFileW(wpat, fd);
  if (h == H_INVALID) return;
  char names[400][260];
  u32 attrs[400];
  int nn = 0;
  do {
    char nm[260]; w_to_a((const u16*)(fd + 44), nm, sizeof(nm));
    if (nm[0] == '.' && (nm[1] == 0 || (nm[1] == '.' && nm[2] == 0))) { mz(fd, sizeof(fd)); continue; }
    if (nn < 400) { scpy(names[nn], nm); attrs[nn] = *(u32*)(fd + 0); ++nn; }
    mz(fd, sizeof(fd));
  } while (FindNextFileW(h, fd));
  FindClose(h);
  for (int i = 0; i < nn; ++i) {
    sb_put(out, prefix);
    if (attrs[i] & FILE_ATTRIBUTE_DIRECTORY) sb_put(out, "[dir]  "); else sb_put(out, "[file] ");
    sb_put(out, names[i]); sb_put(out, "\n");
    if ((attrs[i] & FILE_ATTRIBUTE_DIRECTORY) && depth < maxdepth) {
      char child[560]; scpy(child, dir);
      u32 cl = (u32)slen(child);
      if (cl && child[cl-1] != '\\') { child[cl++] = '\\'; child[cl] = 0; }
      scpy(child + cl, names[i]);
      char nprefix[560]; scpy(nprefix, prefix); scat(nprefix, "  ");
      tree_recurse(c, child, depth + 1, maxdepth, nprefix, out);
    }
  }
}

static void tool_list_tree(const CFG* c, const char* path, int maxdepth, SB* out) {
  char full[512]; if (!resolve_path(c, path && *path ? path : ".", full, sizeof(full))) { sb_put(out, "error: bad path"); return; }
  if (maxdepth < 1) maxdepth = 1;
  if (maxdepth > 8) maxdepth = 8;
  tree_recurse(c, full, 1, maxdepth, "", out);
}

static void tool_scaffold(const CFG* c, const char* language, const char* name, SB* out) {
  if (!name || !*name) { sb_put(out, "error: name required"); return; }
  char dir[512]; if (!resolve_path(c, name, dir, sizeof(dir))) { sb_put(out, "error: bad name"); return; }
  mkdirs(dir);
  const char* ext = lang_ext(language);
  char mainpath[560]; scpy(mainpath, dir);
  scat(mainpath, "\\main"); scat(mainpath, ext + 1); // strip dot? we keep ext with dot via below
  // rebuild cleanly:
  mainpath[0]=0; scpy(mainpath, dir); scat(mainpath, "\\main"); scat(mainpath, ext);
  const char* tmpl = "";
  // minimal hello-world templates
  if (scmp(language, "python") == 0 || scmp(language, "py") == 0) tmpl = "print(\"Hello from Tiramisu Hermes (Python)\")\n";
  else if (scmp(language, "javascript") == 0 || scmp(language, "js") == 0) tmpl = "console.log(\"Hello from Tiramisu Hermes (JavaScript)\");\n";
  else if (scmp(language, "c") == 0) tmpl = "#include <stdio.h>\nint main(void){ printf(\"Hello from Tiramisu Hermes (C)\\n\"); return 0; }\n";
  else if (scmp(language, "cpp") == 0 || scmp(language, "c++") == 0) tmpl = "#include <iostream>\nint main(){ std::cout << \"Hello from Tiramisu Hermes (C++)\" << std::endl; return 0; }\n";
  else if (scmp(language, "rust") == 0 || scmp(language, "rs") == 0) tmpl = "fn main() { println!(\"Hello from Tiramisu Hermes (Rust)\"); }\n";
  else if (scmp(language, "go") == 0) tmpl = "package main\n\nimport \"fmt\"\n\nfunc main() { fmt.Println(\"Hello from Tiramisu Hermes (Go)\") }\n";
  else if (scmp(language, "java") == 0) tmpl = "public class Main { public static void main(String[] a){ System.out.println(\"Hello from Tiramisu Hermes (Java)\"); } }\n";
  else if (scmp(language, "typescript") == 0 || scmp(language, "ts") == 0) tmpl = "console.log(\"Hello from Tiramisu Hermes (TypeScript)\");\n";
  else if (scmp(language, "shell") == 0 || scmp(language, "sh") == 0 || scmp(language, "bash") == 0) tmpl = "#!/bin/sh\necho \"Hello from Tiramisu Hermes (Shell)\"\n";
  else tmpl = "Hello from Tiramisu Hermes\n";
  write_all(mainpath, tmpl, (u32)slen(tmpl), CREATE_ALWAYS);
  sb_put(out, "ok: scaffolded "); sb_put(out, language && *language ? language : "text");
  sb_put(out, " project at "); sb_put(out, dir);
  sb_put(out, " (main file: "); sb_put(out, mainpath); sb_put(out, ")");
}

// ---- os / kernel surface ----------------------------------------------------
typedef struct {
  u32 size, major, minor, build, platform;
  u16 csd[128];
} OSVI;

static void tool_sysinfo(const CFG* c, SB* out) {
  OSVI v; mz(&v, sizeof(v)); v.size = (u32)sizeof(v);
  RtlGetVersion(&v);
  sb_put(out, "brand=" HERMES_BRAND "\n");
  sb_put(out, "os="); sb_u64(out, v.major); sb_put(out, "."); sb_u64(out, v.minor);
  sb_put(out, "."); sb_u64(out, v.build); sb_put(out, "\n");
  sb_put(out, "pid="); sb_u64(out, GetCurrentProcessId()); sb_put(out, "\n");
  sb_put(out, "uptime_ms="); sb_u64(out, GetTickCount64() - g_t0); sb_put(out, "\n");
  sb_put(out, "workspace="); sb_put(out, c->workspace); sb_put(out, "\n");
  char mod[300]; u32 n = GetModuleFileNameA(0, mod, sizeof(mod));
  if (n && n < sizeof(mod)) { mod[n] = 0; sb_put(out, "module="); sb_put(out, mod); sb_put(out, "\n"); }
  sb_put(out, "tools=list_dir,read_file,write_file,append_file,make_dir,stat,delete_file,edit_file,search_files,list_tree,write_code,scaffold_project,shell,sysinfo\n");
}

static const char* skip_ws(const char* p) { while (p && (*p == ' ' || *p == '\t')) ++p; return p; }

static void tool_shell(const CFG* c, const char* cmd, SB* out) {
  const char* p = skip_ws(cmd);
  if (!p || !*p) { sb_put(out, "error: empty command"); return; }
  char verb[24]; int k = 0;
  while (*p && *p != ' ' && *p != '\t' && k < 23) verb[k++] = *p++;
  verb[k] = 0;
  p = skip_ws(p);
  if (scmp(verb, "help") == 0) {
    sb_put(out, "built-in shell (no external processes on the console)\n");
    sb_put(out, "  pwd | ls [path] | cat <file> | mkdir <path> | rm <file>\n");
    sb_put(out, "  echo <text> [> file] | sysinfo | help\n");
  } else if (scmp(verb, "pwd") == 0) {
    sb_put(out, c->workspace);
  } else if (scmp(verb, "ls") == 0 || scmp(verb, "dir") == 0) {
    tool_list_dir(c, (p && p[0]) ? p : ".", out);
  } else if (scmp(verb, "cat") == 0 || scmp(verb, "type") == 0) {
    if (!*p) sb_put(out, "error: cat needs a path");
    else tool_read_file(c, p, out);
  } else if (scmp(verb, "mkdir") == 0) {
    char full[512];
    if (!*p) sb_put(out, "error: mkdir needs a path");
    else if (resolve_path(c, p, full, sizeof(full))) { mkdirs(full); sb_put(out, "ok"); }
    else sb_put(out, "error: bad path");
  } else if (scmp(verb, "rm") == 0 || scmp(verb, "del") == 0) {
    char full[512];
    if (!*p) sb_put(out, "error: rm needs a path");
    else if (resolve_path(c, p, full, sizeof(full))) sb_put(out, DeleteFileA(full) ? "ok" : "error: delete failed");
    else sb_put(out, "error: bad path");
  } else if (scmp(verb, "echo") == 0) {
    const char* gt = p;
    while (*gt && *gt != '>') ++gt;
    if (*gt == '>') {
      char text[600]; u32 tn = (u32)(gt - p);
      if (tn >= sizeof(text)) tn = sizeof(text) - 1;
      for (u32 i = 0; i < tn; ++i) text[i] = p[i];
      text[tn] = 0;
      const char* file = skip_ws(gt + 1);
      if (!*file) { sb_put(out, "error: echo needs a file after >"); return; }
      tool_write(c, file, text, 0, out);
    } else {
      sb_put(out, p);
    }
  } else if (scmp(verb, "sysinfo") == 0) {
    tool_sysinfo(c, out);
  } else {
    sb_put(out, "error: unknown built-in '"); sb_put(out, verb); sb_put(out, "' (try help)");
  }
}

static void run_tool(const CFG* c, const char* name, JN* args, SB* out) {
  const char* path = jstrp(jget(args, "path"), 0);
  if (scmp(name, "list_dir") == 0) tool_list_dir(c, path, out);
  else if (scmp(name, "read_file") == 0) tool_read_file(c, path, out);
  else if (scmp(name, "write_file") == 0) tool_write(c, path, jstrp(jget(args, "content"), 0), 0, out);
  else if (scmp(name, "append_file") == 0) tool_write(c, path, jstrp(jget(args, "content"), 0), 1, out);
  else if (scmp(name, "stat") == 0) tool_stat(c, path, out);
  else if (scmp(name, "make_dir") == 0) { char full[512]; if (resolve_path(c, path, full, sizeof(full))) { mkdirs(full); sb_put(out, "ok"); } else sb_put(out, "error: bad path"); }
  else if (scmp(name, "delete_file") == 0) { char full[512]; if (resolve_path(c, path, full, sizeof(full))) sb_put(out, DeleteFileA(full) ? "ok" : "error: delete failed"); else sb_put(out, "error: bad path"); }
  else if (scmp(name, "edit_file") == 0) tool_edit_file(c, path, jstrp(jget(args, "old"), 0), jstrp(jget(args, "new"), 0), jget(args, "replace_all") && jget(args, "replace_all")->t == J_BOOL && jget(args, "replace_all")->n, out);
  else if (scmp(name, "search_files") == 0) tool_search_files(c, path, jstrp(jget(args, "query"), 0), out);
  else if (scmp(name, "list_tree") == 0) { int md = 3; JN* m = jget(args, "max_depth"); if (m && m->t == J_NUM) { md = 0; for (int i = 0; i < m->n && m->s[i] >= '0' && m->s[i] <= '9'; ++i) md = md * 10 + (m->s[i]-'0'); if (md < 1) md = 1; } tool_list_tree(c, path, md, out); }
  else if (scmp(name, "write_code") == 0) tool_write_code(c, path, jstrp(jget(args, "language"), 0), jstrp(jget(args, "content"), 0), out);
  else if (scmp(name, "scaffold_project") == 0) tool_scaffold(c, jstrp(jget(args, "language"), 0), jstrp(jget(args, "name"), 0), out);
  else if (scmp(name, "shell") == 0) tool_shell(c, jstrp(jget(args, "command"), 0), out);
  else if (scmp(name, "sysinfo") == 0) tool_sysinfo(c, out);
  else { sb_put(out, "error: unknown tool "); sb_put(out, name); }
}

// ---- prompt -----------------------------------------------------------------
static const char* TOOLS_JSON =
  "[{\"type\":\"function\",\"function\":{\"name\":\"list_dir\",\"description\":\"List a directory on the console.\",\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}}},"
  "{\"type\":\"function\",\"function\":{\"name\":\"read_file\",\"description\":\"Read a UTF-8 text file (max 60000 bytes).\",\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}}},"
  "{\"type\":\"function\",\"function\":{\"name\":\"write_file\",\"description\":\"Create or overwrite a text file.\",\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"content\":{\"type\":\"string\"}},\"required\":[\"path\",\"content\"]}}},"
  "{\"type\":\"function\",\"function\":{\"name\":\"append_file\",\"description\":\"Append text to a file.\",\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"content\":{\"type\":\"string\"}},\"required\":[\"path\",\"content\"]}}},"
  "{\"type\":\"function\",\"function\":{\"name\":\"make_dir\",\"description\":\"Create a directory.\",\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}}},"
  "{\"type\":\"function\",\"function\":{\"name\":\"stat\",\"description\":\"Report whether a path is a file, dir, or missing.\",\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}}},"
  "{\"type\":\"function\",\"function\":{\"name\":\"delete_file\",\"description\":\"Delete a file.\",\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}}},"
  "{\"type\":\"function\",\"function\":{\"name\":\"shell\",\"description\":\"Run a built-in console shell command (pwd, ls, cat, mkdir, rm, echo, sysinfo, help). No external processes.\",\"parameters\":{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\"}},\"required\":[\"command\"]}}},"
  "{\"type\":\"function\",\"function\":{\"name\":\"sysinfo\",\"description\":\"Report console OS build, pid, uptime, module path and workspace.\",\"parameters\":{\"type\":\"object\",\"properties\":{}}}},"
  "{\"type\":\"function\",\"function\":{\"name\":\"edit_file\",\"description\":\"Search and replace text in a file (multi-language code editing).\",\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"old\":{\"type\":\"string\"},\"new\":{\"type\":\"string\"},\"replace_all\":{\"type\":\"boolean\"}},\"required\":[\"path\",\"old\",\"new\"]}}},"
  "{\"type\":\"function\",\"function\":{\"name\":\"search_files\",\"description\":\"Search file contents for a string (case-insensitive) across a directory tree.\",\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"query\":{\"type\":\"string\"}},\"required\":[\"query\"]}}},"
  "{\"type\":\"function\",\"function\":{\"name\":\"list_tree\",\"description\":\"Recursively list a directory tree up to max_depth.\",\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"max_depth\":{\"type\":\"integer\"}}}}},"
  "{\"type\":\"function\",\"function\":{\"name\":\"write_code\",\"description\":\"Write source code to a file, auto-appending the correct extension for the language (python, javascript, typescript, c, cpp, rust, go, java, csharp, ruby, php, shell, html, css, sql, kotlin, swift, lua, and more).\",\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"language\":{\"type\":\"string\"},\"content\":{\"type\":\"string\"}},\"required\":[\"path\",\"language\",\"content\"]}}},"
  "{\"type\":\"function\",\"function\":{\"name\":\"scaffold_project\",\"description\":\"Create a new project directory with a starter main file for a given language.\",\"parameters\":{\"type\":\"object\",\"properties\":{\"language\":{\"type\":\"string\"},\"name\":{\"type\":\"string\"}},\"required\":[\"language\",\"name\"]}}}]";

static void status_write(const CFG* c, int step, const char* phase, const char* detail) {
  SB b; sb_init(&b, 8192);
  if (!b.ok) return;
  sb_put(&b, "{\"step\":"); sb_i64(&b, step);
  sb_put(&b, ",\"phase\":"); sb_json(&b, phase);
  sb_put(&b, ",\"detail\":"); sb_json(&b, detail ? detail : "");
  sb_put(&b, ",\"model\":"); sb_json(&b, c->model);
  sb_put(&b, ",\"up_ms\":"); sb_u64(&b, GetTickCount64() - g_t0);
  sb_put(&b, "}");
  write_all(HERMES_ROOT "\\status.json", b.p, b.len, CREATE_ALWAYS);
  xfree(b.p);
  term_status(phase);
  (void)step; (void)detail;
}

// ---- interactive session ----------------------------------------------------
static char* g_hist;   // JSON array of messages, kept open (no closing ']')
static u32 g_hlen;
static u32 g_hcap = 768 * 1024;

static void hs_append(const char* p, u32 n) {
  if (!g_hist || !p) return;
  if (g_hlen + n + 1 >= g_hcap) { logf("history full"); return; }
  mc(g_hist + g_hlen, p, n);
  g_hlen += n;
  g_hist[g_hlen] = 0;
}

static void hs_reset(void) {
  if (!g_hist) g_hist = (char*)xalloc(g_hcap);
  if (!g_hist) return;
  g_hlen = 0;
  const char* sys = "You are " HERMES_BRAND ", an autonomous multilingual coding agent running natively on an Xbox dev-mode console inside the Tiramisu Win32 runtime. You can read, write, edit, search and scaffold source code in any programming language (Python, JavaScript, TypeScript, C, C++, Rust, Go, Java, C#, Ruby, PHP, Shell, HTML, CSS, SQL, Kotlin, Swift, Lua, and more) using your file tools and built-in shell scoped to your workspace. The user is typing on a USB keyboard in a terminal. Work step by step, inspect before writing, verify after writing, and keep replies short. Reply in the user's language when asked.";
  SB b; sb_init(&b, 16384);
  if (!b.ok) return;
  sb_put(&b, "[{\"role\":\"system\",\"content\":");
  sb_json(&b, sys);
  sb_put(&b, "}");
  hs_append(b.p, b.len);
  xfree(b.p);
}

static void hs_user(const char* text) {
  SB b; sb_init(&b, 8192);
  if (!b.ok) return;
  sb_put(&b, ",{\"role\":\"user\",\"content\":");
  sb_json(&b, text);
  sb_put(&b, "}");
  hs_append(b.p, b.len);
  xfree(b.p);
}

static void hs_assistant(const char* text) {
  SB b; sb_init(&b, 8192);
  if (!b.ok) return;
  sb_put(&b, ",{\"role\":\"assistant\",\"content\":");
  sb_json(&b, text);
  sb_put(&b, "}");
  hs_append(b.p, b.len);
  xfree(b.p);
}

static void show_output(const char* s, u32 n) {
  if (!s) return;
  u32 take = n > 500 ? 500 : n;
  char buf[512];
  for (u32 i = 0; i < take; ++i) buf[i] = s[i];
  buf[take] = 0;
  term_write_c(buf, 2);
  term_write("\n");
  if (n > take) term_write_c("  ...[truncated]\n", 2);
}

static int run_session(const CFG* c) {
  hs_reset();
  for (;;) {
    term_status("READY");
    while (!term_poll()) Sleep(25);
    const char* in = term_line();
    char line[256];
    u32 ln = (u32)slen(in);
    if (ln > 240) ln = 240;
    for (u32 i = 0; i < ln; ++i) line[i] = in[i];
    line[ln] = 0;
    trim(line);
    if (!line[0]) continue;

    if (line[0] == '/') {
      if (scmp(line, "/quit") == 0 || scmp(line, "/exit") == 0) return 0;
      if (scmp(line, "/clear") == 0 || scmp(line, "/new") == 0) {
        term_clear();
        hs_reset();
        term_write_c("new session\n", 4);
        continue;
      }
      if (scmp(line, "/help") == 0) {
        term_write_c("commands: /help /clear /new /quit\n", 4);
        term_write_c("tools: list_dir read_file write_file append_file make_dir stat delete_file edit_file search_files list_tree write_code scaffold_project shell sysinfo\n", 4);
        term_write_c("keys: Enter send, Up/Down history, PgUp/PgDn scroll\n", 4);
        continue;
      }
      term_write_c("unknown command, try /help\n", 3);
      continue;
    }

    term_push_history(line);
    term_write_c(line, 1);
    term_write("\n");
    hs_user(line);
    term_status("THINKING");

    int final = 0;
    for (int step = 1; step <= c->max_steps && !final; ++step) {
      status_write(c, step, "request", "building");
      SB body; sb_init(&body, 256 * 1024);
      if (!body.ok) { term_write_c("out of memory\n", 3); break; }
      sb_put(&body, "{\"model\":"); sb_json(&body, c->model);
      sb_put(&body, ",\"messages\":"); sb_putn(&body, g_hist, g_hlen);
      sb_put(&body, "],\"tools\":"); sb_put(&body, TOOLS_JSON);
      sb_put(&body, ",\"tool_choice\":\"auto\",\"temperature\":0}");
      if (!body.ok) { xfree(body.p); term_write_c("request too large\n", 3); break; }

      int st = 0; u32 rn = 0;
      char* resp = http_exchange(c->endpoint, body.p, body.len, &st, &rn);
      xfree(body.p);
      if (!resp) {
        term_write_c("relay unreachable: start the relay on the PC\n", 3);
        break;
      }
      if (st < 200 || st >= 300) {
        char msg[96]; scpy(msg, "relay http ");
        char num[12]; int v = st, k = 0;
        if (v == 0) num[k++] = '0';
        while (v > 0 && k < 11) { num[k++] = (char)('0' + v % 10); v /= 10; }
        for (int a = 0, b = k - 1; a < b; ++a, --b) { char t = num[a]; num[a] = num[b]; num[b] = t; }
        num[k] = 0; scpy(msg + slen(msg), num);
        term_write_c(msg, 3); term_write("\n");
        xfree(resp);
        break;
      }

      JP jp; mz(&jp, sizeof(jp));
      jp.p = resp; jp.end = resp + rn;
      jp.cap = 4 * 1024 * 1024;
      jp.arena = (char*)xalloc(jp.cap);
      if (!jp.arena) { xfree(resp); term_write_c("out of memory\n", 3); break; }
      JN* root = jval(&jp);
      JN* choice = jat(jget(root, "choices"), 0);
      JN* msg = jget(choice, "message");
      if (!msg) {
        term_write_c("bad response from relay\n", 3);
        xfree(jp.arena); xfree(resp);
        break;
      }
      JN* content = jget(msg, "content");
      JN* tcs = jget(msg, "tool_calls");
      int clen = 0; const char* cstr = jstrp(content, &clen);

      if (tcs && tcs->kids) {
        SB asst; sb_init(&asst, 64 * 1024);
        if (!asst.ok) { xfree(jp.arena); xfree(resp); break; }
        sb_put(&asst, ",{\"role\":\"assistant\",\"content\":");
        if (content && content->t == J_STR) sb_json(&asst, cstr); else sb_put(&asst, "null");
        sb_put(&asst, ",\"tool_calls\":[");
        int first = 1;
        for (JN* tc = tcs->kids; tc; tc = tc->next) {
          JN* fn = jget(tc, "function");
          if (!first) sb_putc(&asst, ','); first = 0;
          sb_put(&asst, "{\"id\":"); sb_json(&asst, jstrp(jget(tc, "id"), 0));
          sb_put(&asst, ",\"type\":\"function\",\"function\":{\"name\":"); sb_json(&asst, jstrp(jget(fn, "name"), 0));
          sb_put(&asst, ",\"arguments\":"); sb_json(&asst, jstrp(jget(fn, "arguments"), 0)); sb_put(&asst, "}}");
        }
        sb_put(&asst, "]}");
        if (asst.ok) hs_append(asst.p, asst.len);
        xfree(asst.p);

        for (JN* tc = tcs->kids; tc; tc = tc->next) {
          JN* fn = jget(tc, "function");
          const char* id = jstrp(jget(tc, "id"), 0);
          const char* nm = jstrp(jget(fn, "name"), 0);
          const char* ar = jstrp(jget(fn, "arguments"), 0);
          term_write_c("> ", 2);
          term_write_c(nm ? nm : "?", 2);
          term_write("\n");
          JP aj; mz(&aj, sizeof(aj));
          aj.p = ar; aj.end = ar + slen(ar);
          aj.cap = 256 * 1024;
          aj.arena = (char*)xalloc(aj.cap);
          JN* args = aj.arena ? jval(&aj) : 0;
          SB out; sb_init(&out, 64 * 1024);
          if (!out.ok) { xfree(aj.arena); continue; }
          if (!args || args->t != J_OBJ) sb_put(&out, "error: arguments were not a JSON object");
          else run_tool(c, nm, args, &out);

          if (out.ok) {
            show_output(out.p, out.len);
            u32 keep = out.len > 60000 ? 60000 : out.len;
            SB tm; sb_init(&tm, keep + 512);
            if (tm.ok) {
              sb_put(&tm, ",{\"role\":\"tool\",\"tool_call_id\":"); sb_json(&tm, id);
              sb_put(&tm, ",\"content\":");
              SB cont; sb_init(&cont, keep + 16);
              if (cont.ok) { sb_putn(&cont, out.p, keep); sb_json(&tm, cont.p); xfree(cont.p); }
              else sb_put(&tm, "\"error: tool output too large\"");
              sb_put(&tm, "}");
              if (tm.ok) hs_append(tm.p, tm.len);
              xfree(tm.p);
            }
          }
          xfree(out.p);
          xfree(aj.arena);
        }
        status_write(c, step, "tools", "done");
        xfree(jp.arena); xfree(resp);
        continue;
      }

      if (content && content->t == J_STR && clen > 0) {
        term_write_c(cstr, 0);
        term_write("\n");
        write_all(HERMES_ROOT "\\out\\answer.md", cstr, (u32)clen, CREATE_ALWAYS);
        hs_assistant(cstr);
      } else {
        term_write_c("(empty reply)\n", 2);
      }
      status_write(c, step, "final", "done");
      final = 1;
      xfree(jp.arena); xfree(resp);
    }
    term_status(final ? "READY" : "ERROR");
    term_write("\n");
  }
}

// PE entry point.
void hermes_main(void) {
  g_heap = GetProcessHeap();
  g_t0 = GetTickCount64();
  mkdirs(HERMES_ROOT);
  mkdirs(HERMES_ROOT "\\workspace");
  mkdirs(HERMES_ROOT "\\out");
  g_log = CreateFileA(HERMES_ROOT "\\hermes.log", GENERIC_WRITE, FILE_SHARE_READ, 0, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
  logf(HERMES_BRAND " boot");

  int ui = term_init();
  { char ul[24]; scpy(ul, "ui ready="); scpy(ul + 9, ui ? "1" : "0"); logf(ul); }

  term_clear();
  term_write_c(HERMES_BRAND "  /  native agent on Xbox\n", 4);
  term_write_c("Tiramisu Win64 PE  /  real file tools + built-in shell + OS info\n\n", 2);

  void* wsa = xalloc(512);
  if (wsa) { mz(wsa, 512); int wr = WSAStartup(0x0202, wsa); logf(wr == 0 ? "winsock ready" : "winsock failed"); xfree(wsa); }

  CFG cfg; cfg_load(&cfg);
  logkv("endpoint", cfg.endpoint);
  logkv("model", cfg.model);
  logkv("workspace", cfg.workspace);

  term_write_c("model     ", 2); term_write_c(cfg.model, 4); term_write("\n");
  term_write_c("relay     ", 2); term_write_c(cfg.endpoint, 4); term_write("\n");
  term_write_c("workspace ", 2); term_write_c(cfg.workspace, 4); term_write("\n\n");
  term_write_c("type a request. /help for commands, /quit to exit.\n\n", 2);

  run_session(&cfg);
  WSACleanup();
}
