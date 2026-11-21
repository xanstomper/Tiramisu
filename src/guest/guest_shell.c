// tiramisu/src/guest/guest_shell.c
// Proves the shell32 bridge: the boot-path folder lookups every engine
// runs to find save/settings directories.
// Returns a bitmask:
//   0x001 SHGetFolderPathW(CSIDL_MYDOCUMENTS) -> S_OK, "C:\users\..."
//   0x004 SHGetFolderPathW(CSIDL_LOCAL_APPDATA) -> S_OK
//   0x008 SHGetFolderPathA matches the W result (ANSI form)
//   0x010 SHGetKnownFolderPath(FOLDERID_SavedGames) -> S_OK
//   0x020 SHGetFolderPathW(unknown csidl) -> E_FAIL (fail-visible)
//   0x040 SHGetMalloc -> non-null IMalloc, Alloc/Free round-trip
//   0x080 ShellExecuteExW -> FALSE (named no-shell)
//   0x100 SHF_FLAG_CREATE: dir actually created (CreateFileW write works)
typedef unsigned short u16;
typedef unsigned char u8;
typedef unsigned int u32;
typedef int i32;
typedef unsigned long long u64;
typedef long long i64;

#ifdef _WIN32
#define GSTD __stdcall
#else
#define GSTD
#endif

int _fltused = 1;

__attribute__((dllimport)) i32 GSTD SHGetFolderPathW(void* hwnd, u32 csidl,
                                                     void* token, u32 flags,
                                                     u16* path);
__attribute__((dllimport)) i32 GSTD SHGetFolderPathA(void* hwnd, u32 csidl,
                                                     void* token, u32 flags,
                                                     char* path);
__attribute__((dllimport)) i32 GSTD SHGetKnownFolderPath(const u8* rfid,
                                                         u32 flags,
                                                         void* token,
                                                         u16** path);
__attribute__((dllimport)) i32 GSTD SHGetMalloc(void** out);
__attribute__((dllimport)) i32 GSTD ShellExecuteExW(void* info);

// kernel32 pieces the create-write check needs
__attribute__((dllimport)) void* GSTD CreateFileW(const u16* name,
                                                  u32 access, u32 share,
                                                  void* sa, u32 disp,
                                                  u32 flags, void* tmpl);
__attribute__((dllimport)) i32 GSTD CloseHandle(void* h);
__attribute__((dllimport)) i32 GSTD WriteFile(void* h, const void* buf,
                                              u32 bytes, u32* written,
                                              void* overlapped);

static void utf16_cmp(const u16* a, const char* b, u32* ok) {
  u32 i = 0;
  while (b[i] && a[i] == (u16)(u8)b[i]) ++i;
  *ok = (b[i] == 0 && a[i] == 0) ? 1 : 0;
}

u32 GSTD guest_shell_main(u64 a, u64 b, u64 c) {
  (void)a; (void)b; (void)c;
  u32 bits = 0;

  // ---- 1. wide CSIDL lookups ----
  u16 path[260];
  {
    const i32 hr = SHGetFolderPathW(0, 0x0005 /*MYDOCUMENTS*/, 0, 0, path);
    if (hr == 0) {
      u32 ok = 0;
      utf16_cmp(path, "C:\\users\\player\\Documents", &ok);
      if (ok) bits |= 0x01u;
    }
  }
  {
    u16 appdata[260];
    if (SHGetFolderPathW(0, 0x001C /*LOCAL_APPDATA*/, 0, 0, appdata) == 0) {
      u32 ok = 0;
      utf16_cmp(appdata, "C:\\users\\player\\AppData\\Local", &ok);
      if (ok) bits |= 0x04u;
    }
  }


  // ---- 2. ANSI form agrees ----
  {
    char patha[260];
    if (SHGetFolderPathA(0, 0x0005, 0, 0, patha) == 0) {
      u32 ok = 1;
      const char* want = "C:\\users\\player\\Documents";
      for (u32 i = 0; want[i]; ++i)
        if (patha[i] != want[i]) ok = 0;
      if (ok) bits |= 0x08u;
    }
  }


  // ---- 3. FOLDERID GUID form ----
  {
    // FOLDERID_SavedGames {4C5C32FF-BB9D-43B0-B5B4-2D72E54EAAA4}
    u8 rfid[16];
    for (u32 i = 0; i < 16; ++i) rfid[i] = 0;
    rfid[3] = 0x4C; rfid[2] = 0x5C; rfid[1] = 0x32; rfid[0] = 0xFF;
    rfid[5] = 0xBB; rfid[4] = 0x9D;
    rfid[7] = 0x43; rfid[6] = 0xB0;
    rfid[8] = 0xB5; rfid[9] = 0xB4; rfid[10] = 0x2D; rfid[11] = 0x72;
    rfid[12] = 0xE5; rfid[13] = 0x4E; rfid[14] = 0xAA; rfid[15] = 0xA4;
    u16* sp = 0;
    if (SHGetKnownFolderPath(rfid, 0, 0, &sp) == 0 && sp) {
      u32 ok = 0;
      utf16_cmp(sp, "C:\\users\\player\\Saved Games", &ok);
      if (ok) bits |= 0x10u;
    }
  }


  // ---- 4. unknown csidl fails VISIBLY ----
  {
    const i32 hr = SHGetFolderPathW(0, 0x0039 /*PROGRAM_FILES: no mapping*/,
                                    0, 0, path);
    if (hr == (i32)0x80004005) bits |= 0x20u;
  }


  // ---- 5. IMalloc round-trip ----
  {
    void* malloc_ = 0;
    if (SHGetMalloc(&malloc_) == 0 && malloc_) {
      // vtable: QueryInterface=0, AddRef=1, Release=2, Alloc=3, Realloc=4,
      // Free=5
      void** vt = *(void***)malloc_;
      typedef void* (*alloc_fn)(void*, u32);
      typedef void (*free_fn)(void*, void*);
      const alloc_fn alloc = (alloc_fn)vt[3];
      const free_fn freeme = (free_fn)vt[5];
      u32* p = (u32*)alloc(malloc_, 32);
      if (p) {
        p[0] = 0xCAFE;
        if (p[0] == 0xCAFE) bits |= 0x40u;
        freeme(malloc_, p);
      }
    }
  }


  // ---- 6. ShellExecuteExW fails named ----
  if (ShellExecuteExW(0) == 0) bits |= 0x80u;


  // ---- 7. the created dir actually accepts a save file ----
  {
    u16 dir[260];
    if (SHGetFolderPathW(0, 0x0005, 0, 0x8000 /*FLAG_CREATE*/, dir) == 0) {
      // append \save.cfg
      u32 n = 0;
      while (dir[n]) ++n;
      static const u16 tail[9] = {'\\', 's', 'a', 'v', 'e', '.', 'c', 'f', 'g'};
      for (u32 i = 0; i < 9; ++i) dir[n + i] = tail[i];
      dir[n + 9] = 0;
      void* f = CreateFileW(dir, 0x40000000 /*GENERIC_WRITE*/, 0, 0,
                            2 /*CREATE_ALWAYS*/, 0, 0);
      if (f && f != (void*)-1) {
        u32 written = 0;
        static const u8 data[4] = {'t', 'e', 's', 't'};
        if (WriteFile(f, data, 4, &written, 0) && written == 4) bits |= 0x100u;
        CloseHandle(f);
      }
    }
  }
  // echo mark: proves the debug array wasn't clobbered
  return bits;
}
