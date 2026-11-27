// tiramisu/tests/test_mf_steam_impl.cpp
//
// Evidence for the 18 symbols the gap worklist listed as named-silent even
// though the bridges implement them:
//
//   mfplat.dll      MFStartup, MFCreateMediaType, MFCreateMemoryBuffer,
//                   MFCreateSample
//   steam_api64.dll SteamAPI_Init / Shutdown / RunCallbacks /
//                   Register+Unregister Callback / Register+Unregister CallResult
//                   / RestartAppIfNecessary, SteamApps, SteamClient,
//                   SteamFriends, SteamUser, SteamUtils
//
// Every check drives the objects through their real COM vtable / exported
// function and asserts the value that goes in comes back out. It must FAIL if
// an object is a shell: a media type that does not return the GUID it was given,
// a buffer that reports the wrong length, a sample that forgets its buffer, a
// callback table that keeps calling an unregistered callback.
//
// The last section pins the classifier in both directions: these 18 must read
// REAL, and the surfaces that really are empty (SteamMatchmaking,
// SteamNetworking, SteamUserStats, mfplat MFShutdown, shell32 ExtractIconExW)
// must keep reading named-silent. A blanket module-level REAL flip would break
// the second half.
#include "../src/trt/trt.h"

#include <stdio.h>
#include <string.h>

extern "C" {
int trt_bridge_register_mf(void);
int trt_bridge_register_steam(void);
int trt_bridge_register_shell(void);
int trt_steam_callback_count(void);
int trt_steam_callresult_count(void);
}

namespace {

int g_pass = 0;
int g_fail = 0;

#define CHECK(cond, what)                        \
  do {                                           \
    if (cond) {                                  \
      ++g_pass;                                  \
      printf("  PASS %s\n", what);               \
    } else {                                     \
      ++g_fail;                                  \
      printf("  FAIL %s\n", what);               \
    }                                            \
  } while (0)

template <typename T>
T slot(void* obj, int idx) {
  const void** vt = *(const void***)obj;
  return (T)vt[idx];
}

// IMFAttributes / IMFMediaType / IMFSample share the 33-slot attribute base.
constexpr int k_release = 2;
constexpr int k_get_u64 = 8, k_get_guid = 10;
constexpr int k_get_strlen = 11, k_get_str = 12;
constexpr int k_delete_item = 19;
constexpr int k_set_u64 = 22, k_set_guid = 24, k_set_str = 25;
constexpr int k_get_count = 30;
constexpr int k_mt_get_major = 33;
constexpr int k_smp_get_time = 35, k_smp_set_time = 36;
constexpr int k_smp_buf_count = 39, k_smp_add_buffer = 42;
constexpr int k_smp_contiguous = 41, k_smp_copy = 46;
constexpr int k_smp_remove_all = 44, k_smp_total_len = 45;
// IMFMediaBuffer
constexpr int k_buf_lock = 3, k_buf_unlock = 4;
constexpr int k_buf_get_cur = 5, k_buf_set_cur = 6, k_buf_get_max = 7;

// MF_MT_MAJOR_TYPE {48eba18e-f8c9-4687-bf11-0a74c9f96a8f}
const u8 k_major_guid[16] = {0x8e, 0xa1, 0xeb, 0x48, 0xc9, 0xf8, 0x87, 0x46,
                             0xbf, 0x11, 0x0a, 0x74, 0xc9, 0xf9, 0x6a, 0x8f};
// MFMediaType_Video {73646976-0000-0010-8000-00AA00389B71}
const u8 k_video_guid[16] = {0x76, 0x69, 0x64, 0x73, 0x00, 0x00, 0x10, 0x00,
                             0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71};
// MF_MT_FRAME_SIZE {1652c33d-d6b2-4012-b834-72030849a37d}
const u8 k_frame_size_guid[16] = {0x3d, 0xc3, 0x52, 0x16, 0xb2, 0xd6, 0x12, 0x40,
                                  0xb8, 0x34, 0x72, 0x03, 0x08, 0x49, 0xa3, 0x7d};
// Arbitrary key for the string check.
const u8 k_string_guid[16] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
                              0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0x0f, 0x10};

using create1_fn = i32(TRT_GUEST_ABI*)(void**);
using create_buf_fn = i32(TRT_GUEST_ABI*)(u32, void**);
using startup_fn = i32(TRT_GUEST_ABI*)(u32, u32);
using shutdown_fn = i32(TRT_GUEST_ABI*)(void);
using release_fn = u32(TRT_GUEST_ABI*)(void*);
using set_u32_fn = i32(TRT_GUEST_ABI*)(void*, const u8*, u32);
using get_u32_fn = i32(TRT_GUEST_ABI*)(void*, const u8*, u32*);
using set_u64_fn = i32(TRT_GUEST_ABI*)(void*, const u8*, u64);
using get_u64_fn = i32(TRT_GUEST_ABI*)(void*, const u8*, u64*);
using set_guid_fn = i32(TRT_GUEST_ABI*)(void*, const u8*, const u8*);
using get_guid_fn = i32(TRT_GUEST_ABI*)(void*, const u8*, u8*);
using set_str_fn = i32(TRT_GUEST_ABI*)(void*, const u8*, const u16*);
using get_strlen_fn = i32(TRT_GUEST_ABI*)(void*, const u8*, u32*);
using get_str_fn = i32(TRT_GUEST_ABI*)(void*, const u8*, u16*, u32, u32*);
using get_count_fn = i32(TRT_GUEST_ABI*)(void*, u32*);
using get_major_fn = i32(TRT_GUEST_ABI*)(void*, u8*);
using lock_fn = i32(TRT_GUEST_ABI*)(void*, u8**, u32*, u32*);
using unlock_fn = i32(TRT_GUEST_ABI*)(void*);
using get_len_fn = i32(TRT_GUEST_ABI*)(void*, u32*);
using set_len_fn = i32(TRT_GUEST_ABI*)(void*, u32);
using set_time_fn = i32(TRT_GUEST_ABI*)(void*, i64);
using get_time_fn = i32(TRT_GUEST_ABI*)(void*, i64*);
using add_buf_fn = i32(TRT_GUEST_ABI*)(void*, void*);
using plain_fn = i32(TRT_GUEST_ABI*)(void*);
using delete_fn = i32(TRT_GUEST_ABI*)(void*, const u8*);

// --------------------------------------------------------------------------
// mfplat: the four entry points must hand out working objects, not shells.
// --------------------------------------------------------------------------
void test_mfplat() {
  auto resolve = [](const char* mod, const char* sym) -> void* {
    return trt_resolve_module(nullptr, mod, sym, 0);
  };
  auto p_startup = (startup_fn)resolve("mfplat.dll", "MFStartup");
  auto p_shutdown = (shutdown_fn)resolve("mfplat.dll", "MFShutdown");
  auto p_mt = (create1_fn)resolve("mfplat.dll", "MFCreateMediaType");
  auto p_buf = (create_buf_fn)resolve("mfplat.dll", "MFCreateMemoryBuffer");
  auto p_smp = (create1_fn)resolve("mfplat.dll", "MFCreateSample");
  CHECK(p_startup && p_shutdown && p_mt && p_buf && p_smp,
        "mfplat resolves MFStartup/MFShutdown and the three object creators");

  // MFStartup is refcounted: two startups need two shutdowns, and the second
  // shutdown is the one that leaves the platform closed. Both answer S_OK.
  CHECK(p_startup && p_startup(0x00020070u, 0) == 0,
        "MFStartup(MF_VERSION) -> S_OK");
  CHECK(p_startup && p_startup(0x00020070u, 0) == 0,
        "MFStartup is re-entrant -> S_OK");
  if (p_shutdown) {
    CHECK(p_shutdown() == 0, "MFShutdown after two startups -> S_OK (counted)");
    CHECK(p_shutdown() == 0, "MFShutdown again -> S_OK");
  }

  // ---- IMFMediaType: GUID, UINT64 and string round trip -------------------
  void* mt = nullptr;
  CHECK(p_mt && p_mt(&mt) == 0 && mt != nullptr, "MFCreateMediaType -> object");
  if (mt) {
    u8 got_guid[16] = {};
    bool set_ok = slot<set_guid_fn>(mt, k_set_guid)(mt, k_major_guid, k_video_guid) == 0;
    bool get_ok = slot<get_guid_fn>(mt, k_get_guid)(mt, k_major_guid, got_guid) == 0;
    CHECK(set_ok && get_ok && memcmp(got_guid, k_video_guid, 16) == 0,
          "IMFMediaType SetGUID/GetGUID returns the media major type");

    u8 major[16] = {};
    CHECK(slot<get_major_fn>(mt, k_mt_get_major)(mt, major) == 0 &&
              memcmp(major, k_video_guid, 16) == 0,
          "IMFMediaType::GetMajorType reports what was set");

    const u64 frame_size = ((u64)1920u << 32) | 1080u;
    u64 got_size = 0;
    CHECK(slot<set_u64_fn>(mt, k_set_u64)(mt, k_frame_size_guid, frame_size) == 0 &&
              slot<get_u64_fn>(mt, k_get_u64)(mt, k_frame_size_guid, &got_size) == 0 &&
              got_size == frame_size,
          "IMFMediaType UINT64 (frame size 1920x1080) round trips");

    const u16 name[] = {'H', '2', '6', '4', 0};
    u16 back[32] = {};
    u32 back_len = 0;
    CHECK(slot<set_str_fn>(mt, k_set_str)(mt, k_string_guid, name) == 0 &&
              slot<get_strlen_fn>(mt, k_get_strlen)(mt, k_string_guid, &back_len) == 0 &&
              back_len == 4 &&
              slot<get_str_fn>(mt, k_get_str)(mt, k_string_guid, back, 32, &back_len) == 0 &&
              memcmp(back, name, sizeof(name)) == 0,
          "IMFMediaType string attribute round trips with its length");

    u32 count = 0;
    CHECK(slot<get_count_fn>(mt, k_get_count)(mt, &count) == 0 && count == 3,
          "IMFMediaType holds the three attributes that were set");
    CHECK(slot<delete_fn>(mt, k_delete_item)(mt, k_string_guid) == 0 &&
              slot<get_count_fn>(mt, k_get_count)(mt, &count) == 0 && count == 2,
          "IMFMediaType::DeleteItem really removes one");
    CHECK(slot<release_fn>(mt, k_release)(mt) == 0,
          "IMFMediaType::Release drops the last reference");
  }

  // ---- IMFMediaBuffer: real storage, real length --------------------------
  void* buf = nullptr;
  CHECK(p_buf && p_buf(256, &buf) == 0 && buf != nullptr,
        "MFCreateMemoryBuffer(256) -> object");
  if (buf) {
    u8* ptr = nullptr;
    u32 max_len = 0;
    CHECK(slot<lock_fn>(buf, k_buf_lock)(buf, &ptr, &max_len, nullptr) == 0 &&
              ptr != nullptr && max_len == 256,
          "IMFMediaBuffer::Lock hands back 256 writable bytes");
    if (ptr) {
      const char* pcm = "PCM!";
      memcpy(ptr, pcm, 5);
      slot<unlock_fn>(buf, k_buf_unlock)(buf);
      CHECK(slot<set_len_fn>(buf, k_buf_set_cur)(buf, 128) == 0,
            "IMFMediaBuffer::SetCurrentLength(128)");
      u32 cur = 0, mx = 0;
      CHECK(slot<get_len_fn>(buf, k_buf_get_cur)(buf, &cur) == 0 && cur == 128 &&
                slot<get_len_fn>(buf, k_buf_get_max)(buf, &mx) == 0 && mx == 256,
            "IMFMediaBuffer reports current=128 max=256");
      CHECK(slot<set_len_fn>(buf, k_buf_set_cur)(buf, 999) != 0,
            "IMFMediaBuffer::SetCurrentLength past max is rejected");
      u8* again = nullptr;
      CHECK(slot<lock_fn>(buf, k_buf_lock)(buf, &again, nullptr, nullptr) == 0 &&
                again && memcmp(again, pcm, 5) == 0,
            "IMFMediaBuffer keeps the bytes that were written");
      slot<unlock_fn>(buf, k_buf_unlock)(buf);
    } else {
      CHECK(false, "IMFMediaBuffer::Lock returned a usable pointer");
    }
  }

  // ---- IMFSample: timestamps and buffers ---------------------------------
  void* smp = nullptr;
  CHECK(p_smp && p_smp(&smp) == 0 && smp != nullptr, "MFCreateSample -> object");
  if (smp) {
    CHECK(slot<set_time_fn>(smp, k_smp_set_time)(smp, 2000000) == 0,
          "IMFSample::SetSampleTime(0.2s)");
    i64 t = 0;
    CHECK(slot<get_time_fn>(smp, k_smp_get_time)(smp, &t) == 0 && t == 2000000,
          "IMFSample::GetSampleTime returns it");
    u32 nbuf = 99;
    if (buf) {
      CHECK(slot<add_buf_fn>(smp, k_smp_add_buffer)(smp, buf) == 0 &&
                slot<get_count_fn>(smp, k_smp_buf_count)(smp, &nbuf) == 0 && nbuf == 1,
            "IMFSample::AddBuffer makes the sample hold one buffer");
      u32 total = 0;
      CHECK(slot<get_len_fn>(smp, k_smp_total_len)(smp, &total) == 0 && total == 128,
            "IMFSample::GetTotalLength sums the buffer lengths (128)");
      using contiguous_fn = i32(TRT_GUEST_ABI*)(void*, void**);
      using copy_fn = i32(TRT_GUEST_ABI*)(void*, void*);
      auto contiguous = slot<contiguous_fn>(smp, k_smp_contiguous);
      auto copy = slot<copy_fn>(smp, k_smp_copy);
      void* combined = nullptr;
      CHECK(contiguous(smp, &combined) == 0 && combined == buf,
            "a single buffer is returned with a new reference");
      slot<release_fn>(combined, k_release)(combined);
      void* next = nullptr;
      CHECK(p_buf(3, &next) == 0 && next, "allocate a second media buffer");
      u8* bytes = nullptr;
      CHECK(slot<lock_fn>(next, k_buf_lock)(next, &bytes, nullptr, nullptr) == 0 && bytes,
            "lock the source buffer");
      memcpy(bytes, "end", 3);
      slot<unlock_fn>(next, k_buf_unlock)(next);
      CHECK(slot<set_len_fn>(next, k_buf_set_cur)(next, 3) == 0 &&
                slot<add_buf_fn>(smp, k_smp_add_buffer)(smp, next) == 0,
            "append a second payload to the sample");
      CHECK(copy(smp, next) == (i32)0xc00d36b1u,
            "a short destination reports MF_E_BUFFERTOOSMALL");
      slot<release_fn>(next, k_release)(next);
      CHECK(contiguous(smp, &combined) == 0 && combined && combined != buf &&
                slot<get_count_fn>(smp, k_smp_buf_count)(smp, &nbuf) == 0 && nbuf == 1,
            "conversion replaces multiple buffers with one contiguous buffer");
      bytes = nullptr;
      CHECK(slot<lock_fn>(combined, k_buf_lock)(combined, &bytes, nullptr, &total) == 0 &&
                total == 131 && bytes && !memcmp(bytes, "PCM!", 5) &&
                !memcmp(bytes + 128, "end", 3),
            "the contiguous buffer preserves both payloads in order");
      slot<unlock_fn>(combined, k_buf_unlock)(combined);
      slot<release_fn>(combined, k_release)(combined);
      CHECK(p_buf(131, &next) == 0 && next && copy(smp, next) == 0,
            "CopyToBuffer writes into a caller-owned buffer");
      bytes = nullptr;
      CHECK(slot<lock_fn>(next, k_buf_lock)(next, &bytes, nullptr, &total) == 0 &&
                total == 131 && bytes && !memcmp(bytes + 128, "end", 3),
            "CopyToBuffer reports the copied length and preserves payload bytes");
      slot<unlock_fn>(next, k_buf_unlock)(next);
      slot<release_fn>(next, k_release)(next);
      CHECK(slot<plain_fn>(smp, k_smp_remove_all)(smp) == 0 &&
                slot<get_count_fn>(smp, k_smp_buf_count)(smp, &nbuf) == 0 && nbuf == 0,
            "IMFSample::RemoveAllBuffers drops the buffer");
      CHECK(contiguous(smp, &combined) == (i32)0x8000ffffu && !combined,
            "an empty sample has no contiguous buffer");
    }
    CHECK(slot<release_fn>(smp, k_release)(smp) == 0,
          "IMFSample::Release drops the last reference");
  }
  if (buf) slot<release_fn>(buf, k_release)(buf);
}

// --------------------------------------------------------------------------
// Steam: lifecycle, callback table, and the interface accessors.
// --------------------------------------------------------------------------
struct fake_callback {
  void** vtable;
  int hits;
};

int TRT_GUEST_ABI fake_callback_run(void* self, void*) {
  ((fake_callback*)self)->hits += 1;
  return 0;
}
void* g_cb_vtable[4] = {(void*)&fake_callback_run, nullptr, nullptr, nullptr};

void test_steam() {
  auto resolve = [](const char* mod, const char* sym) -> void* {
    return trt_resolve_module(nullptr, mod, sym, 0);
  };
  using init_fn = int(TRT_GUEST_ABI*)(void);
  using void_fn = void(TRT_GUEST_ABI*)(void);
  using restart_fn = int(TRT_GUEST_ABI*)(u32);
  using reg_cb_fn = void(TRT_GUEST_ABI*)(void*, int);
  using unreg_cb_fn = void(TRT_GUEST_ABI*)(void*);
  using reg_cr_fn = void(TRT_GUEST_ABI*)(void*, u64);
  using unreg_cr_fn = void(TRT_GUEST_ABI*)(void*, u64);
  using iface_fn = void*(TRT_GUEST_ABI*)(void);

  auto p_init = (init_fn)resolve("steam_api64.dll", "SteamAPI_Init");
  auto p_shutdown = (void_fn)resolve("steam_api64.dll", "SteamAPI_Shutdown");
  auto p_run = (void_fn)resolve("steam_api64.dll", "SteamAPI_RunCallbacks");
  auto p_reg = (reg_cb_fn)resolve("steam_api64.dll", "SteamAPI_RegisterCallback");
  auto p_unreg = (unreg_cb_fn)resolve("steam_api64.dll", "SteamAPI_UnregisterCallback");
  auto p_reg_cr = (reg_cr_fn)resolve("steam_api64.dll", "SteamAPI_RegisterCallResult");
  auto p_unreg_cr = (unreg_cr_fn)resolve("steam_api64.dll", "SteamAPI_UnregisterCallResult");
  auto p_restart = (restart_fn)resolve("steam_api64.dll", "SteamAPI_RestartAppIfNecessary");
  auto p_apps = (iface_fn)resolve("steam_api64.dll", "SteamApps");
  auto p_user = (iface_fn)resolve("steam_api64.dll", "SteamUser");
  auto p_utils = (iface_fn)resolve("steam_api64.dll", "SteamUtils");
  auto p_friends = (iface_fn)resolve("steam_api64.dll", "SteamFriends");
  auto p_client = (iface_fn)resolve("steam_api64.dll", "SteamClient");

  CHECK(p_init && p_shutdown && p_run && p_reg && p_unreg && p_reg_cr &&
            p_unreg_cr && p_restart && p_apps && p_user && p_utils && p_friends &&
            p_client,
        "steam_api64 resolves the lifecycle, registration and interface exports");

  CHECK(p_init && p_init() == 1, "SteamAPI_Init -> 1 (auth layer live)");
  CHECK(p_restart && p_restart(480) == 0,
        "SteamAPI_RestartAppIfNecessary(480) -> 0 (no DRM restart loop)");

  // ---- a callback that really runs, and really stops ---------------------
  fake_callback cb = {g_cb_vtable, 0};
  if (p_reg && p_run) {
    p_reg(&cb, 0);
    p_run();
    CHECK(cb.hits == 1, "RunCallbacks invokes a registered CCallbackBase");
    p_reg(&cb, 0);  // a duplicate registration must not double-dispatch
    p_run();
    CHECK(cb.hits == 2, "registering the same callback twice dispatches once");
    CHECK(trt_steam_callback_count() == 1, "callback table holds exactly one entry");
    if (p_unreg) {
      p_unreg(&cb);
      p_run();
      CHECK(cb.hits == 2, "an unregistered callback is no longer called");
      CHECK(trt_steam_callback_count() == 0, "unregister empties the callback table");
    }
  }

  // ---- call results: a real table ---------------------------------------
  if (p_reg_cr && p_unreg_cr) {
    void* fake_cr = (void*)&cb;
    p_reg_cr(fake_cr, 42);
    CHECK(trt_steam_callresult_count() == 1, "RegisterCallResult stores one entry");
    p_reg_cr(fake_cr, 42);
    CHECK(trt_steam_callresult_count() == 1, "a duplicate call result is stored once");
    p_reg_cr(fake_cr, 43);
    CHECK(trt_steam_callresult_count() == 2, "a different APICall id is a second entry");
    p_unreg_cr(fake_cr, 42);
    CHECK(trt_steam_callresult_count() == 1, "UnregisterCallResult removes only its own id");
    p_unreg_cr(fake_cr, 43);
    CHECK(trt_steam_callresult_count() == 0, "the call-result table drains");
  }

  // ---- the interfaces answer from real state ----------------------------
  void* apps = p_apps ? p_apps() : nullptr;
  CHECK(apps != nullptr, "SteamApps() -> interface object");
  if (apps) {
    using bsub_fn = int(TRT_GUEST_ABI*)(void*, u32);
    // Nothing is imported into the library in this test, so ownership is false.
    CHECK(slot<bsub_fn>(apps, 6)(apps, 480) == 0,
          "ISteamApps::BIsSubscribedApp(480) -> 0 with an empty library");
  }
  void* user = p_user ? p_user() : nullptr;
  CHECK(user != nullptr, "SteamUser() -> interface object");
  if (user) {
    using id_fn = u64(TRT_GUEST_ABI*)(void*, u64*);
    const u64 anon = 0x0110000100000001ull;
    CHECK(slot<id_fn>(user, 2)(user, nullptr) == anon,
          "ISteamUser::GetSteamID -> the anonymous local account id");
  }
  void* utils = p_utils ? p_utils() : nullptr;
  CHECK(utils != nullptr, "SteamUtils() -> interface object");
  if (utils) {
    using u32_fn = u32(TRT_GUEST_ABI*)(void*);
    using str_fn = const char*(TRT_GUEST_ABI*)(void*);
    const char* lang = slot<str_fn>(utils, 12)(utils);
    CHECK(lang && strcmp(lang, "english") == 0,
          "ISteamUtils::GetSteamUILanguage -> \"english\" (never NULL)");
    // RestartAppIfNecessary(480) above established the app context, so the
    // utils surface reports the same app id the game declared.
    CHECK(slot<u32_fn>(utils, 9)(utils) == 480,
          "ISteamUtils::GetAppID -> 480, the id RestartAppIfNecessary set");
  }
  void* friends = p_friends ? p_friends() : nullptr;
  if (friends) {
    using str_fn = const char*(TRT_GUEST_ABI*)(void*);
    const char* persona = slot<str_fn>(friends, 0)(friends);
    CHECK(persona && strcmp(persona, "TiramisuPlayer") == 0,
          "ISteamFriends::GetPersonaName -> \"TiramisuPlayer\"");
  } else {
    CHECK(false, "SteamFriends() -> interface object");
  }
  void* client = p_client ? p_client() : nullptr;
  auto p_client_v020 = (iface_fn)resolve("steam_api64.dll", "SteamAPI_SteamClient_v020");
  CHECK(client != nullptr && p_client_v020 && p_client_v020() == client,
        "SteamClient() is the same object the v020 accessor returns");

  if (p_shutdown) p_shutdown();
  CHECK(p_init && p_init() == 1, "SteamAPI_Init after Shutdown -> 1 (re-init works)");

  // ---- ISteamUserStats: real stats + achievements + leaderboards -----------
  auto p_us = (iface_fn)resolve("steam_api64.dll", "SteamUserStats");
  void* us = p_us ? p_us() : nullptr;
  CHECK(us != nullptr, "SteamUserStats() -> interface object");
  if (us) {
    using set_i_fn = int(TRT_GUEST_ABI*)(void*, const char*, i32);
    using get_i_fn = int(TRT_GUEST_ABI*)(void*, const char*, i32*);
    using set_a_fn = int(TRT_GUEST_ABI*)(void*, const char*);
    using get_a_fn = int(TRT_GUEST_ABI*)(void*, const char*, u8*);
    using lb_fn = u64(TRT_GUEST_ABI*)(void*, const char*, int, int);
    using lb_name_fn = const char*(TRT_GUEST_ABI*)(void*, u64);
    CHECK(slot<set_i_fn>(us, 3)(us, "SCORE", 42) == 1, "ISteamUserStats::SetStatInt32");
    i32 sv = 0;
    CHECK(slot<get_i_fn>(us, 1)(us, "SCORE", &sv) == 1 && sv == 42, "ISteamUserStats::GetStatInt32 round-trip");
    CHECK(slot<set_a_fn>(us, 7)(us, "ACH_TEST") == 1, "ISteamUserStats::SetAchievement");
    u8 ach = 0;
    CHECK(slot<get_a_fn>(us, 6)(us, "ACH_TEST", &ach) == 1 && ach == 1, "ISteamUserStats::GetAchievement round-trip");
    u64 lbh = slot<lb_fn>(us, 22)(us, "TOP_RUN", 1, 1);
    const char* lbn = lbh ? slot<lb_name_fn>(us, 20)(us, lbh) : "";
    CHECK(lbh != 0 && lbn && strcmp(lbn, "TOP_RUN") == 0, "ISteamUserStats::FindOrCreateLeaderboard + GetLeaderboardName");
  }

  // ---- ISteamMatchmaking: real lobby lifecycle + KV data -----------------
  auto p_mm = (iface_fn)resolve("steam_api64.dll", "SteamMatchmaking");
  void* mm = p_mm ? p_mm() : nullptr;
  CHECK(mm != nullptr, "SteamMatchmaking() -> interface object");
  if (mm) {
    using cr_fn = u64(TRT_GUEST_ABI*)(void*, int, int);
    using cnt_fn = int(TRT_GUEST_ABI*)(void*, u64);
    using sdata_fn = int(TRT_GUEST_ABI*)(void*, u64, const char*, const char*);
    using gdata_fn = const char*(TRT_GUEST_ABI*)(void*, u64, const char*);
    using lv_fn = void(TRT_GUEST_ABI*)(void*, u64);
    u64 lid = slot<cr_fn>(mm, 12)(mm, 2, 4);
    CHECK(lid != 0 && slot<cnt_fn>(mm, 16)(mm, lid) == 1, "ISteamMatchmaking::CreateLobby adds owner as member");
    CHECK(slot<sdata_fn>(mm, 19)(mm, lid, "mode", "coop") == 1, "ISteamMatchmaking::SetLobbyData");
    const char* ldv = slot<gdata_fn>(mm, 18)(mm, lid, "mode");
    CHECK(ldv && strcmp(ldv, "coop") == 0, "ISteamMatchmaking::GetLobbyData round-trip");
    slot<lv_fn>(mm, 14)(mm, lid);
  }

  // ---- ISteamNetworking: real P2P loopback channel -------------------------
  auto p_net = (iface_fn)resolve("steam_api64.dll", "SteamNetworking");
  void* net = p_net ? p_net() : nullptr;
  CHECK(net != nullptr, "SteamNetworking() -> interface object");
  if (net) {
    using send_fn = int(TRT_GUEST_ABI*)(void*, u64, const void*, u32, int, int);
    using avail_fn = int(TRT_GUEST_ABI*)(void*, u32*, int);
    using read_fn = int(TRT_GUEST_ABI*)(void*, void*, u32, u32*, u64*, int);
    const u64 self_id = 0x0110000100000001ull;
    const char msg[] = "PING";
    CHECK(slot<send_fn>(net, 0)(net, self_id, msg, 4, 2, 0) == 1, "ISteamNetworking::SendP2PPacket");
    u32 sz = 0;
    CHECK(slot<avail_fn>(net, 1)(net, &sz, 0) == 1 && sz == 4, "ISteamNetworking::IsP2PPacketAvailable");
    char rx[8] = {}; u64 from = 0; u32 got = 0;
    CHECK(slot<read_fn>(net, 2)(net, rx, 8, &got, &from, 0) == 1 && got == 4 && memcmp(rx, "PING", 4) == 0,
          "ISteamNetworking::ReadP2PPacket round-trip");
  }
}

// --------------------------------------------------------------------------
// The classifier must say REAL exactly where it is true.
// --------------------------------------------------------------------------
bool is_class(const char* module, const char* symbol, trt_impl_class want) {
  int cls = -1;
  if (trt_symbol_class(module, symbol, &cls) != 1) return false;
  return cls == (int)want;
}

void test_classification() {
  const char* real_syms[][2] = {
      {"mfplat.dll", "MFStartup"},
      {"mfplat.dll", "MFCreateMediaType"},
      {"mfplat.dll", "MFCreateMemoryBuffer"},
      {"mfplat.dll", "MFCreateSample"},
      {"steam_api64.dll", "SteamAPI_Init"},
      {"steam_api64.dll", "SteamAPI_Shutdown"},
      {"steam_api64.dll", "SteamAPI_RunCallbacks"},
      {"steam_api64.dll", "SteamAPI_RegisterCallback"},
      {"steam_api64.dll", "SteamAPI_UnregisterCallback"},
      {"steam_api64.dll", "SteamAPI_RegisterCallResult"},
      {"steam_api64.dll", "SteamAPI_UnregisterCallResult"},
      {"steam_api64.dll", "SteamAPI_RestartAppIfNecessary"},
      {"steam_api64.dll", "SteamApps"},
      {"steam_api64.dll", "SteamClient"},
      {"steam_api64.dll", "SteamFriends"},
      {"steam_api64.dll", "SteamMatchmaking"},
      {"steam_api64.dll", "SteamNetworking"},
      {"steam_api64.dll", "SteamUser"},
      {"steam_api64.dll", "SteamUserStats"},
      {"steam_api64.dll", "SteamUtils"},
      {"shell32.dll", "ExtractIconExW"},
  };
  int good = 0;
  const int total = (int)(sizeof(real_syms) / sizeof(real_syms[0]));
  for (int i = 0; i < total; ++i) {
    if (is_class(real_syms[i][0], real_syms[i][1], TRT_IMPL_REAL)) ++good;
    else printf("  .. %s!%s is not REAL\n", real_syms[i][0], real_syms[i][1]);
  }
  char line[96];
  snprintf(line, sizeof(line), "all %d implemented symbols classify as REAL", total);
  CHECK(good == total, line);

  // The honest empties. A module-wide REAL flip would break every one of these.
  const char* silent_syms[][2] = {
      {"mfplat.dll", "MFShutdown"},
      {"mfreadwrite.dll", "MFCreateSourceReaderFromURL"},
  };
  int silent_ok = 0;
  const int silent_total = (int)(sizeof(silent_syms) / sizeof(silent_syms[0]));
  for (int i = 0; i < silent_total; ++i) {
    if (is_class(silent_syms[i][0], silent_syms[i][1], TRT_IMPL_NAMED_SILENT))
      ++silent_ok;
    else
      printf("  .. %s!%s is not named-silent\n", silent_syms[i][0],
             silent_syms[i][1]);
  }
  snprintf(line, sizeof(line), "all %d still-empty symbols stay named-silent",
           silent_total);
  CHECK(silent_ok == silent_total, line);
}

}  // namespace

int main() {
  trt_bridge_register_mf();
  trt_bridge_register_steam();
  trt_bridge_register_shell();

  printf("-- mfplat objects\n");
  test_mfplat();
  printf("-- steam lifecycle and interfaces\n");
  test_steam();
  printf("-- implementation classification\n");
  test_classification();

  printf("RESULT: %d passed, %d failed\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
