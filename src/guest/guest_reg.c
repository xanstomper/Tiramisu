// tiramisu/src/guest/guest_reg.c
// A guest that uses the registry the way a PC game does at startup:
//   - read a setting from HKCU\Software\<game>
//   - write one back (a "recent level" or a first-run flag)
//   - enumerate the subkeys of a key
//   - ask for a value that does not exist (must fail VISIBLY, not silently
//     return a zero that the game then trusts)
// Returns a bitmask.
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

__attribute__((dllimport)) i32 GSTD RegOpenKeyExW(void* key, const u16* sub,
                                                   u32 options, u32 sam,
                                                   void** out);
__attribute__((dllimport)) i32 GSTD RegCreateKeyExW(void* key, const u16* sub,
                                                    u32 reserved, void* cls,
                                                    u32 options, u32 sam,
                                                    void** out, u32* disp);
__attribute__((dllimport)) i32 GSTD RegQueryValueExW(void* key, const u16* name,
                                                      u32* reserved, u32* type,
                                                      u8* data, u32* size);
__attribute__((dllimport)) i32 GSTD RegSetValueExW(void* key, const u16* name,
                                                   u32 reserved, u32 type,
                                                   const u8* data, u32 cbData);
__attribute__((dllimport)) i32 GSTD RegCloseKey(void* key);
__attribute__((dllimport)) i32 GSTD RegEnumKeyExW(void* key, u32 index,
                                                  u16* name, u32 cchMax,
                                                  u32* reserved,
                                                  u32* lpcchName,
                                                  u32* lastWrite);

#define HKEY_CURRENT_USER ((void*)0x80000001u)
#define HKEY_LOCAL_MACHINE ((void*)0x80000002u)
#define REG_SZ 1u
#define REG_DWORD 4u


u32 GSTD reg_boot(u64 a, u64 b, u64 c) {
  (void)a; (void)b; (void)c;
  u32 bits = 0;

  // 1. open a per-user key (a game asking for its own settings)
  const u16 sub[] = {'S', 'o', 'f', 't', 'w', 'a', 'r', 'e', '\\',
                     'T', 'i', 'r', 'a', 'm', 'i', 's', 'u', 0};
  void* key = 0;
  i32 hr = RegOpenKeyExW(HKEY_CURRENT_USER, sub, 0, 0x20019, &key);
  // the key does not exist yet on a fresh hive: that is a NORMAL outcome
  bits |= (hr == 2 /*FILE_NOT_FOUND*/) ? 0x1u : 0u;

  // 2. create it (as a game does on first run)
  u32 disposition = 0;
  hr = RegCreateKeyExW(HKEY_CURRENT_USER, sub, 0, 0, 0, 0x20019, &key,
                       &disposition);
  bits |= (hr == 0 && key != 0) ? 0x2u : 0u;

  // 3. write a string value
  const u16 value[] = {'L', 'a', 's', 't', 'L', 'e', 'v', 'e', 'l', 0};
  const u16 payload[] = {'s', 'h', 'a', 'd', 'o', 'w', 0};
  hr = RegSetValueExW(key, value, 0, REG_SZ, (const u8*)payload,
                      (u32)(sizeof(payload)));
  bits |= (hr == 0) ? 0x4u : 0u;

  // 4. read it back
  u8 buf[64];
  u32 type = 0, size = sizeof(buf);
  u32 diag_type = 0, diag_size = 0;
  hr = RegQueryValueExW(key, value, 0, &type, buf, &size);
  const int read_ok = (hr == 0 && type == REG_SZ && size == sizeof(payload) &&
                       buf[0] == 's' && buf[2] == 'h');  // UTF-16
  // Windows reports the required capacity through the same in/out pointer.
  u32 short_size = 1;
  const i32 short_hr = RegQueryValueExW(key, value, 0, 0, buf, &short_size);
  bits |= read_ok && short_hr == 234 && short_size == sizeof(payload) ? 0x8u : 0u;
  // diagnostics: the type and size we got back ride in the high bytes
  diag_type = type;
  diag_size = size;

  // 5. a value that is not there must FAIL, not come back as a zero
  const u16 missing[] = {'N', 'o', 'p', 'e', 0};
  u32 msize = sizeof(buf);
  hr = RegQueryValueExW(key, missing, 0, &type, buf, &msize);
  bits |= (hr == 2 /*FILE_NOT_FOUND*/) ? 0x10u : 0u;

  // 6. a machine-wide key too (games read HKLM for GPU/driver settings)
  const u16 lmsub[] = {'S', 'o', 'f', 't', 'w', 'a', 'r', 'e',
                        '\\', 'T', 'i', 'r', 'a', 'm', 'i', 's', 'u',
                        '\\', 'H', 'K', 'L', 'M', 0};
  void* lmkey = 0;
  hr = RegCreateKeyExW(HKEY_LOCAL_MACHINE, lmsub, 0, 0, 0, 0x20019, &lmkey,
                       &disposition);
  bits |= (hr == 0 && lmkey != 0) ? 0x20u : 0u;

  // 7. enumerate the subkeys of HKCU\Software (should see our key)
  const u16 soft[] = {'S', 'o', 'f', 't', 'w', 'a', 'r', 'e', 0};
  void* softkey = 0;
  hr = RegOpenKeyExW(HKEY_CURRENT_USER, soft, 0, 0x20019, &softkey);
  u32 name_len = 0;
  u16 namebuf[64];
  i32 enum_hr = 2;
  if (hr == 0) {
    for (u32 i = 0; i < 8; ++i) {
      enum_hr = RegEnumKeyExW(softkey, i, namebuf, (u32)(sizeof(namebuf) / 2),
                              0, &name_len, 0);
      if (enum_hr != 0) break;
      if (name_len == 0) break;
    }
  }
  bits |= (enum_hr == 259 /*NO_MORE_ITEMS*/) ? 0x40u : 0u;

  RegCloseKey(key);
  if (lmkey) RegCloseKey(lmkey);
  if (softkey) RegCloseKey(softkey);

  // ============ 8. the token/security half (OrTMM's imports) ============
  // 8a. OpenProcessToken: a REAL pseudo token handle
  __attribute__((dllimport)) int GSTD OpenProcessToken(void* proc, u32 access,
                                                        void** tok);
  __attribute__((dllimport)) int GSTD GetTokenInformation(void* tok, u32 cls,
                                                           void* info, u32 len,
                                                           u32* len_out);
  __attribute__((dllimport)) int GSTD
      ConvertStringSecurityDescriptorToSecurityDescriptorW(const u16* sddl,
                                                            u32 req,
                                                            void** sd,
                                                            u32* len);
  __attribute__((dllimport)) int GSTD ConvertSidToStringSidW(const void* sid,
                                                              u16** str);
  void* tok = 0;
  if (OpenProcessToken((void*)0xFFFFFFFFFFFFFFFFull /*pseudo current proc*/,
                       0x0008 /*TOKEN_QUERY*/, &tok) == 1 && tok)
    bits |= 0x80u;

  // 8b. GetTokenInformation(TokenUser) fills a SID_AND_ATTRIBUTES record
  if (tok) {
    u8 tbuf[64];
    u32 tlen = 0;
    if (GetTokenInformation(tok, 1 /*TokenUser*/, tbuf, (u32)sizeof(tbuf),
                            &tlen) == 1 && tlen >= 8 + 24) {
      // the record points at a SID with revision 1 and NT authority (5)
      const u8* sid = *(const u8**)tbuf;
      const int user_ok = sid && sid[0] == 1 && sid[2 + 5] == 5;
      bits |= user_ok ? 0x100u : 0u;
      // 8c. ConvertSidToStringSidW on THAT sid gives "S-1-5-88-1-1001"
      if (user_ok) {
        u16* str = 0;
        if (ConvertSidToStringSidW(sid, &str) == 1 && str) {
          const char* want = "S-1-5-88-1-1001";
          int ok = 1;
          for (int i = 0; want[i]; ++i)
            if (str[i] != (u16)(u8)want[i]) ok = 0;
          bits |= (ok && str[15] == 0) ? 0x200u : 0u;
        }
      }
    }

    // 8d. TokenIntegrityLevel also answers (medium integrity S-1-16-8192)
    u8 ibuf[48];
    u32 ilen = 0;
    if (GetTokenInformation(tok, 25 /*TokenIntegrityLevel*/, ibuf,
                            (u32)sizeof(ibuf), &ilen) == 1) {
      const u8* sid = *(const u8* const*)ibuf;
      bits |= (sid && sid[0] == 1 && sid[7] == 16) ? 0x400u : 0u;
    }

    // 8e. an unsupported class fails NAMED (ERROR_INVALID_PARAMETER = 87)
    u32 xl = 0;
    if (GetTokenInformation(tok, 3 /*TokenGroups: unsupported*/, ibuf,
                            (u32)sizeof(ibuf), &xl) == 0)
      bits |= 0x800u;
  }

  // 8f. SDDL conversion returns a REAL self-relative SECURITY_DESCRIPTOR
  //     (NULL DACL = allow-all; the console has no security model — the
  //     bridge says so in its log). Garbage input must still fail named.
  {
    const u16 sddl[] = {'D', ':', '(', 'A', ';', ';', 'F', 'A', ';', ';',
                        ';', 'S', '-', '1', '-', '3', '-', '4', ')', 0};
    void* sd = 0;
    u32 sl = 0;
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(
            sddl, 1 /*SDDL_REVISION_1*/, &sd, &sl) != 0 && sd && sl >= 32) {
      const u8* b = (const u8*)sd;
      // self-relative: revision 1, SE_SELF_RELATIVE (0x8000) set,
      // owner+group offsets inside the blob
      if (b[0] == 1 && (b[2] | (b[3] << 8)) == 0x8000) bits |= 0x1000u;
    }
  }

  // 8g. a garbage token handle fails (ERROR_INVALID_HANDLE)
  {
    u8 tbuf[8];
    u32 tlen = 0;
    if (GetTokenInformation((void*)0x12340000u, 1, tbuf, sizeof(tbuf),
                            &tlen) == 0)
      bits |= 0x2000u;
  }

  return bits | ((diag_type & 0xFFu) << 8) | ((diag_size & 0xFFu) << 16) |
         ((buf[0] & 0xFFu) << 24);
}
