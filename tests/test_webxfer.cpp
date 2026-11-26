// tiramisu/tests/test_webxfer.cpp
// End-to-end TCP HTTP verification of src/host/webxfer.cpp:
// LAN IP ranking, folder streaming upload, zip streaming extraction,
// path traversal rejection, per-game config persistence, launch/stop/rescan/delete,
// Expect: 100-continue handling, and live log tailing.
#include "../src/host/webxfer.h"
#include "../src/host/host_log.h"
#include "../src/host/vendor/miniz.h"

#include <arpa/inet.h>
#include <cstdio>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <netinet/in.h>
#include <string>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace longan {
void log_line(const char*) {}
}

static int g_pass = 0;
static int g_fail = 0;
#define CHECK(cond, msg)                                                      \
  do {                                                                        \
    if (cond) { ++g_pass; }                                                   \
    else { ++g_fail; printf("  FAIL: %s (line %d)\n", msg, __LINE__); }       \
  } while (0)

namespace {

std::string http_req(unsigned short port, const std::string& method,
                     const std::string& path, const std::string& body,
                     const char* extra_hdrs = "") {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return {};
  sockaddr_in sa{};
  sa.sin_family = AF_INET;
  sa.sin_port = htons(port);
  inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
  if (connect(fd, (sockaddr*)&sa, sizeof(sa)) != 0) {
    close(fd);
    return {};
  }

  char hdr[1024];
  int hl = snprintf(hdr, sizeof(hdr),
                    "%s %s HTTP/1.1\r\nHost: 127.0.0.1:%u\r\n"
                    "Content-Length: %zu\r\n%sConnection: close\r\n\r\n",
                    method.c_str(), path.c_str(), (unsigned)port, body.size(),
                    extra_hdrs ? extra_hdrs : "");
  std::string req(hdr, (size_t)hl);
  if (!extra_hdrs || !strstr(extra_hdrs, "100-continue")) {
    req += body;
    size_t sent = 0;
    while (sent < req.size()) {
      ssize_t n = send(fd, req.data() + sent, req.size() - sent, 0);
      if (n <= 0) break;
      sent += (size_t)n;
    }
  } else {
    size_t sent = 0;
    while (sent < req.size()) {
      ssize_t n = send(fd, req.data() + sent, req.size() - sent, 0);
      if (n <= 0) break;
      sent += (size_t)n;
    }
    char cbuf[256] = {};
    ssize_t cn = recv(fd, cbuf, sizeof(cbuf) - 1, 0);
    if (cn > 0 && strstr(cbuf, "100 Continue")) {
      sent = 0;
      while (sent < body.size()) {
        ssize_t n = send(fd, body.data() + sent, body.size() - sent, 0);
        if (n <= 0) break;
        sent += (size_t)n;
      }
    }
  }

  std::string resp;
  char buf[4096];
  ssize_t n;
  while ((n = recv(fd, buf, sizeof(buf), 0)) > 0) {
    resp.append(buf, (size_t)n);
  }
  close(fd);
  return resp;
}

std::string interrupted_upload(unsigned short port, const char* headers,
                               const char* body) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return {};
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  std::string response;
  if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
    const std::string req =
        "PUT /api/upload-file?game=HollowDemo&rel=HollowDemo.exe HTTP/1.1\r\n"
        "Host: localhost\r\nConnection: close\r\n" +
        std::string(headers) + "\r\n" + body;
    const ssize_t sent = send(fd, req.data(), req.size(), 0);
    shutdown(fd, SHUT_WR);
    if (sent == static_cast<ssize_t>(req.size())) {
      char reply[512];
      ssize_t n;
      while ((n = recv(fd, reply, sizeof(reply), 0)) > 0) response.append(reply, n);
    }
  }
  close(fd);
  return response;
}

std::string read_str_file(const std::string& p) {
  FILE* f = fopen(p.c_str(), "rb");
  if (!f) return {};
  char buf[4096];
  size_t n = fread(buf, 1, sizeof(buf), f);
  fclose(f);
  return std::string(buf, n);
}

bool file_exists(const std::string& p) {
  struct stat st{};
  return stat(p.c_str(), &st) == 0;
}

}  // namespace

int main() {
  printf("=== WebXfer LAN Dashboard & Streaming Upload Tests ===\n");

  // 1. IPv4 ranking tests
  CHECK(longan::webxfer_score_ipv4("192.168.1.42") >
            longan::webxfer_score_ipv4("10.0.0.15"),
        "192.168.x ranks above 10.x");
  CHECK(longan::webxfer_score_ipv4("10.0.0.15") >
            longan::webxfer_score_ipv4("172.16.0.5"),
        "10.x ranks above 172.16.x");
  CHECK(longan::webxfer_score_ipv4("172.16.0.5") >
            longan::webxfer_score_ipv4("8.8.8.8"),
        "172.16.x ranks above public IP");
  CHECK(longan::webxfer_score_ipv4("127.0.0.1") == 0 &&
            longan::webxfer_score_ipv4("169.254.10.20") == 0,
        "Loopback and APIPA link-local rejected cleanly");

  // Create temp local_state directory
  char tmpl[] = "/tmp/tiramisu_webxfer_XXXXXX";
  char* tmp = mkdtemp(tmpl);
  CHECK(tmp != nullptr, "Created temp LocalState directory");
  if (!tmp) return 1;
  const std::string root = tmp;
  {
    FILE* lf = fopen((root + "/tiramisu.log").c_str(), "wb");
    if (lf) {
      fputs("boot line 1\n[VK->D3D OK] vulkan-1 ready\n", lf);
      fclose(lf);
    }
  }

  // 2. Start server on ephemeral port
  const std::string url = longan::webxfer_start_port(root, 0);
  const unsigned short port = longan::webxfer_port();
  CHECK(!url.empty() && url.rfind("http://", 0) == 0 && port > 0,
        "webxfer_start_port bound OS ephemeral port cleanly");

  // 3. Dashboard HTML + API logs
  {
    std::string r = http_req(port, "GET", "/", "");
    CHECK(r.find("200 OK") != std::string::npos &&
              r.find("/api/upload-file") != std::string::npos &&
              r.find("/api/stop") != std::string::npos &&
              r.find("/api/config/") != std::string::npos,
          "GET / returns full folder-drop + game-config dashboard HTML");
    std::string l = http_req(port, "GET", "/api/logs", "");
    CHECK(l.find("200 OK") != std::string::npos &&
              l.find("[VK->D3D OK]") != std::string::npos,
          "GET /api/logs returns tiramisu.log tail");
  }

  // 4. Security path traversal rejections
  {
    std::string r1 = http_req(
        port, "PUT", "/api/upload-file?game=../escape&rel=bad.txt", "abc");
    std::string r2 = http_req(
        port, "PUT", "/api/upload-file?game=GoodGame&rel=../../bad.txt", "abc");
    std::string r3 = http_req(port, "POST", "/launch/..", "");
    std::string r4 = http_req(port, "DELETE", "/api/game/..", "");
    CHECK(r1.find("400 Bad Request") != std::string::npos &&
              r2.find("400 Bad Request") != std::string::npos &&
              r3.find("400 Bad Request") != std::string::npos &&
              r4.find("400 Bad Request") != std::string::npos,
          "All path traversal attempts rejected with 400 Bad Request");
  }

  // 5. Unpacked folder streaming upload
  {
    const std::string exe_bytes = "MZ_FAKE_PE64_BYTES_0123456789";
    const std::string asset_bytes = "UNITY_ASSETS_PAYLOAD";
    std::string r1 = http_req(
        port, "PUT", "/api/upload-file?game=HollowDemo&rel=HollowDemo.exe",
        exe_bytes);
    std::string r2 = http_req(
        port, "PUT",
        "/api/upload-file?game=HollowDemo&rel=HollowDemo_Data/resources.assets",
        asset_bytes);
    CHECK(r1.find("200 OK") != std::string::npos &&
              r2.find("200 OK") != std::string::npos,
          "Unpacked folder files streamed cleanly over /api/upload-file");
    CHECK(read_str_file(root + "/games/HollowDemo/HollowDemo.exe") == exe_bytes &&
              read_str_file(root + "/games/HollowDemo/HollowDemo_Data/resources.assets") == asset_bytes,
          "Uploaded folder hierarchy & bytes persisted identically on disk");
    CHECK(longan::webxfer_poll_rescan(), "Folder upload flagged host rescan");

    const auto fixed = interrupted_upload(port, "Content-Length: 100\r\n", "partial");
    const auto chunked = interrupted_upload(port, "Transfer-Encoding: chunked\r\n",
                                            "7\r\npartial\r\n");
    CHECK(fixed.find("500 Server Error") != std::string::npos &&
              chunked.find("500 Server Error") != std::string::npos &&
              read_str_file(root + "/games/HollowDemo/HollowDemo.exe") == exe_bytes,
          "Interrupted fixed and chunked replacements preserve the existing file");

    struct rlimit saved_limit{};
    const bool got_limit = getrlimit(RLIMIT_FSIZE, &saved_limit) == 0;
    struct rlimit short_limit = saved_limit;
    short_limit.rlim_cur = 1;
    const auto saved_handler = signal(SIGXFSZ, SIG_IGN);
    const bool limited = got_limit && setrlimit(RLIMIT_FSIZE, &short_limit) == 0;
    std::string write_failure;
    if (limited) {
      write_failure = http_req(port, "PUT",
          "/api/upload-file?game=HollowDemo&rel=HollowDemo.exe", "replacement");
      setrlimit(RLIMIT_FSIZE, &saved_limit);
    }
    signal(SIGXFSZ, saved_handler);
    CHECK(limited && write_failure.find("500 Server Error") != std::string::npos &&
              read_str_file(root + "/games/HollowDemo/HollowDemo.exe") == exe_bytes,
          "Storage write failure preserves the existing file");

    const auto empty = http_req(port, "PUT",
        "/api/upload-file?game=HollowDemo&rel=empty.dat&batch=1", "");
    CHECK(empty.find("200 OK") != std::string::npos &&
              file_exists(root + "/games/HollowDemo/empty.dat"),
          "An intentional empty file upload succeeds");
  }

  // 6. .zip streaming upload + Expect: 100-continue + zip-slip protection
  {
    const std::string zpath = root + "/make_test.zip";
    mz_zip_archive z{};
    mz_zip_writer_init_file(&z, zpath.c_str(), 0);
    const char* exe_data = "MZ_ZIPQUEST_EXE";
    const char* slip_data = "SHOULD_NOT_ESCAPE";
    mz_zip_writer_add_mem(&z, "bin/ZipQuest.exe", exe_data, strlen(exe_data), 0);
    mz_zip_writer_add_mem(&z, "../escaped.txt", slip_data, strlen(slip_data), 0);
    mz_zip_writer_finalize_archive(&z);
    mz_zip_writer_end(&z);

    std::string zbytes = read_str_file(zpath);
    std::string rz = http_req(port, "PUT", "/upload/ZipQuest.zip", zbytes,
                              "Expect: 100-continue\r\n");
    CHECK(rz.find("200 OK") != std::string::npos &&
              rz.find("extracted: ZipQuest") != std::string::npos,
          ".zip upload with Expect: 100-continue extracted cleanly");
    CHECK(read_str_file(root + "/games/ZipQuest/bin/ZipQuest.exe") == exe_data &&
              !file_exists(root + "/games/escaped.txt"),
          ".zip extraction wrote inner exe and blocked ../ zip-slip entry");
  }

  // 7. Config persistence + GET /games
  {
    std::string cfg =
        "{\"exe\":\"HollowDemo.exe\",\"appid\":\"367520\","
        "\"args\":\"-windowed -dx11\",\"input\":\"pad2kbm\"}";
    std::string rc = http_req(port, "POST", "/api/config/HollowDemo", cfg);
    CHECK(rc.find("200 OK") != std::string::npos, "POST /api/config succeeded");
    CHECK(read_str_file(root + "/games/HollowDemo/steam_appid.txt") == "367520" &&
              read_str_file(root + "/games/HollowDemo/tiramisu_args.txt") == "-windowed -dx11" &&
              read_str_file(root + "/games/HollowDemo/tiramisu_input.txt") == "pad2kbm",
          "Config persisted steam_appid.txt, tiramisu_args.txt, tiramisu_input.txt");

    std::string rg = http_req(port, "GET", "/games", "");
    CHECK(rg.find("HollowDemo") != std::string::npos &&
              rg.find("ZipQuest") != std::string::npos &&
              rg.find("\"appid\":\"367520\"") != std::string::npos &&
              rg.find("\"input\":\"pad2kbm\"") != std::string::npos,
          "GET /games reflects uploaded games + persisted config");
  }

  // 8. Launch, Stop, Status, Delete
  {
    longan::webxfer_set_running_game("HollowDemo");
    std::string st = http_req(port, "GET", "/api/status", "");
    CHECK(st.find("\"running_game\":\"HollowDemo\"") != std::string::npos,
          "GET /api/status reports active game");

    // launch honesty (item 9): a launch while a game runs is a 409, not a
    // silent overwrite of the running game
    std::string rl_busy = http_req(port, "POST", "/launch/HollowDemo", "");
    CHECK(rl_busy.find("409 Conflict") != std::string::npos,
          "POST /launch while a game runs returns 409");

    // with no game running, a launch queues (202) and the host poll sees it
    longan::webxfer_set_running_game("");
    std::string rl = http_req(port, "POST", "/launch/HollowDemo", "");
    std::string ln;
    CHECK(rl.find("202 Accepted") != std::string::npos &&
              rl.find("\"queued\":true") != std::string::npos &&
              longan::webxfer_poll_launch(&ln) && ln == "HollowDemo",
          "POST /launch queues (202) and host launch poll fires");

    // a launch queued while another is still queued is a 409
    std::string rl2 = http_req(port, "POST", "/launch/HollowDemo", "");
    std::string rl3 = http_req(port, "POST", "/launch/ZipQuest", "");
    CHECK(rl2.find("202 Accepted") != std::string::npos &&
              rl3.find("409 Conflict") != std::string::npos,
          "POST /launch while a launch is queued returns 409");
    std::string drained;
    CHECK(longan::webxfer_poll_launch(&drained) && drained == "HollowDemo",
          "queued launch drains via poll");

    // the host loop's launch outcome lands in /api/status
    longan::webxfer_set_launch_result("HollowDemo", "started");
    std::string st2 = http_req(port, "GET", "/api/status", "");
    CHECK(st2.find("\"last_launch_result\":\"started\"") != std::string::npos &&
              st2.find("\"last_launch_name\":\"HollowDemo\"") != std::string::npos,
          "GET /api/status reports last launch result");

    // scoped rescan (item 4): an upload flags the changed game, not a blank rescan
    std::string r_up = http_req(
        port, "PUT", "/api/upload-file?game=HollowDemo&rel=extra.txt", "x");
    std::string scoped;
    CHECK(r_up.find("200 OK") != std::string::npos &&
              longan::webxfer_poll_rescan(&scoped) && scoped == "HollowDemo",
          "folder upload flags a scoped rescan for its game");

    std::string rs = http_req(port, "POST", "/api/stop", "");
    CHECK(rs.find("200 OK") != std::string::npos && longan::webxfer_poll_stop(),
          "POST /api/stop triggers host stop poll");

    std::string rd = http_req(port, "DELETE", "/api/game/ZipQuest", "");
    std::string rg2 = http_req(port, "GET", "/games", "");
    CHECK(rd.find("200 OK") != std::string::npos &&
              !file_exists(root + "/games/ZipQuest") &&
              rg2.find("ZipQuest") == std::string::npos &&
              rg2.find("HollowDemo") != std::string::npos,
          "DELETE /api/game removes game directory cleanly");
  }

  // 9. Settings: GET defaults, POST partial update, persistence, bad input
  {
    std::string g = http_req(port, "GET", "/api/settings", "");
    CHECK(g.find("200 OK") != std::string::npos &&
              g.find("\"default_input\":\"auto\"") != std::string::npos &&
              g.find("\"os_build\":19045") != std::string::npos &&
              g.find("\"write_watch\":true") != std::string::npos,
          "GET /api/settings returns defaults");

    std::string p1 = http_req(port, "POST", "/api/settings",
                              "{\"mouse_speed\":77,\"pad_vibration\":false,"
                              "\"default_input\":\"pad2kbm\"}");
    CHECK(p1.find("200 OK") != std::string::npos && p1.find("saved") != std::string::npos,
          "POST /api/settings partial update saved");

    std::string g2 = http_req(port, "GET", "/api/settings", "");
    CHECK(g2.find("\"mouse_speed\":77") != std::string::npos &&
              g2.find("\"pad_vibration\":false") != std::string::npos &&
              g2.find("\"default_input\":\"pad2kbm\"") != std::string::npos &&
              g2.find("\"write_watch\":true") != std::string::npos,
          "POST /api/settings partial update visible in GET");

    // persistence: settings.ini written under LocalState with the values
    const std::string ini = read_str_file(root + "/settings.ini");
    CHECK(ini.find("mouse_speed=77") != std::string::npos &&
              ini.find("pad_vibration=0") != std::string::npos &&
              ini.find("default_input=pad2kbm") != std::string::npos,
          "settings.ini persisted to LocalState");

    // out-of-range ints are clamped, not rejected
    std::string p2 = http_req(port, "POST", "/api/settings",
                              "{\"mouse_speed\":9999,\"log_lines\":-5}");
    std::string g3 = http_req(port, "GET", "/api/settings", "");
    CHECK(p2.find("200 OK") != std::string::npos &&
              g3.find("\"mouse_speed\":100") != std::string::npos &&
              g3.find("\"log_lines\":4") != std::string::npos,
          "POST /api/settings clamps out-of-range values");

    // dashboard page carries the settings, connect, saves backup/debug & mobile folder guidance
    std::string page = http_req(port, "GET", "/", "");
    CHECK(page.find("200 OK") != std::string::npos &&
              page.find("id=settings") != std::string::npos &&
              page.find("/api/settings") != std::string::npos &&
              page.find("Console connection") != std::string::npos &&
              page.find("openDirPicker") != std::string::npos &&
              page.find("/api/backup-saves") != std::string::npos &&
              page.find("/api/debug-bundle") != std::string::npos,
          "dashboard HTML ships settings, connect, mobile folder help, save backup & debug bundle");
  }

  // 8b. Pre-launch PE compat badges + save backup/restore + debug bundle endpoints
  {
    const std::string cgame = root + "/games/Compat32";
    const std::string agame = root + "/games/CompatArm64";
    std::string mk = "mkdir -p '" + cgame + "' '" + agame + "' '" +
                     root + "/drive_c/users/SaveDir' '" + root + "/profiles'";
    (void)system(mk.c_str());
    // Minimal 32-bit x86 PE header (Machine = 0x014C) -> supported via x86 JIT
    uint8_t pe32[256] = {};
    pe32[0] = 'M'; pe32[1] = 'Z';
    pe32[0x3C] = 0x80;
    pe32[0x80] = 'P'; pe32[0x81] = 'E';
    pe32[0x84] = 0x4C; pe32[0x85] = 0x01; // x86
    pe32[0x94] = 0xE0; // OptionalHeader size
    pe32[0x98] = 0x0B; pe32[0x99] = 0x01; // PE32 magic
    FILE* pf = fopen((cgame + "/Compat32.exe").c_str(), "wb");
    if (pf) { fwrite(pe32, 1, sizeof(pe32), pf); fclose(pf); }
    // Minimal ARM64 PE header (Machine = 0xAA64) -> flagged unsupported
    uint8_t arm64[256] = {};
    memcpy(arm64, pe32, sizeof(pe32));
    arm64[0x84] = 0x64; arm64[0x85] = 0xAA;
    FILE* af = fopen((agame + "/CompatArm64.exe").c_str(), "wb");
    if (af) { fwrite(arm64, 1, sizeof(arm64), af); fclose(af); }
    std::string gl_compat = http_req(port, "GET", "/games", "");
    CHECK(gl_compat.find("Compat32") != std::string::npos &&
              gl_compat.find("x86 32-bit (JIT)") != std::string::npos &&
              gl_compat.find("ARM64 (Unsupported)") != std::string::npos &&
              gl_compat.find("\"compat_ok\":false") != std::string::npos,
          "/games tags 32-bit x86 as JIT and flags ARM64 as unsupported");

    // An Xbox GDK / GameOS console payload is x64 but not a Win32 target.
    // The dashboard must say so instead of showing a misleading D3D11 badge.
    const std::string ggame = root + "/games/CompatGdk";
    mk = "mkdir -p '" + ggame + "'";
    (void)system(mk.c_str());
    uint8_t pe64[320] = {};
    pe64[0] = 'M'; pe64[1] = 'Z';
    pe64[0x3C] = 0x80;
    pe64[0x80] = 'P'; pe64[0x81] = 'E';
    pe64[0x84] = 0x64; pe64[0x85] = 0x86; // Machine = 0x8664 (x64)
    pe64[0x94] = 0xF0;                    // SizeOfOptionalHeader
    pe64[0x98] = 0x0B; pe64[0x99] = 0x02; // PE32+ magic
    FILE* gf = fopen((ggame + "/CompatGdk.exe").c_str(), "wb");
    if (gf) { fwrite(pe64, 1, sizeof(pe64), gf); fclose(gf); }
    FILE* gc = fopen((ggame + "/MicrosoftGame.Config").c_str(), "wb");
    if (gc) { fputs("<Game config />\n", gc); fclose(gc); }
    std::string gl_gdk = http_req(port, "GET", "/games", "");
    CHECK(gl_gdk.find("CompatGdk") != std::string::npos &&
              gl_gdk.find("Xbox GDK / GameOS build (Unsupported)") != std::string::npos &&
              gl_gdk.find("\"compat_ok\":false") != std::string::npos,
          "/games flags Xbox GDK / GameOS packages as unsupported pre-launch");

    // Seed save file + log, download backup zip & debug bundle, delete save, restore zip
    const std::string save_path = root + "/drive_c/users/SaveDir/slot1.sav";
    FILE* sf = fopen(save_path.c_str(), "wb");
    if (sf) { fputs("SAVE_PAYLOAD_99", sf); fclose(sf); }
    FILE* lf = fopen((root + "/tiramisu.log").c_str(), "wb");
    if (lf) { fputs("tiramisu: test log line\n", lf); fclose(lf); }

    std::string bresp = http_req(port, "GET", "/api/backup-saves", "");
    const size_t bsplit = bresp.find("\r\n\r\n");
    std::string bzip = (bsplit == std::string::npos) ? "" : bresp.substr(bsplit + 4);
    CHECK(bresp.find("200 OK") != std::string::npos &&
              bresp.find("tiramisu-saves-backup.zip") != std::string::npos &&
              bzip.size() > 64 && bzip[0] == 'P' && bzip[1] == 'K',
          "GET /api/backup-saves streams valid zip");

    remove(save_path.c_str());
    CHECK(read_str_file(save_path).empty(), "save deleted before restore test");

    std::string rresp = http_req(port, "POST", "/api/restore-saves", bzip);
    CHECK(rresp.find("200 OK") != std::string::npos &&
              rresp.find("\"ok\":true") != std::string::npos &&
              read_str_file(save_path) == "SAVE_PAYLOAD_99",
          "POST /api/restore-saves restores drive_c saves cleanly");

    std::string dresp = http_req(port, "GET", "/api/debug-bundle", "");
    const size_t dsplit = dresp.find("\r\n\r\n");
    std::string dzip = (dsplit == std::string::npos) ? "" : dresp.substr(dsplit + 4);
    CHECK(dresp.find("200 OK") != std::string::npos &&
              dresp.find("tiramisu-debug-bundle.zip") != std::string::npos &&
              dzip.size() > 64 && dzip[0] == 'P' && dzip[1] == 'K',
          "GET /api/debug-bundle streams valid support zip");
  }

  // 9. In-console autopilot: armed by transfer, driven by the launch result,
  //    applies a real repair and re-queues; stops named when no fix is known.
  {
    std::string stale;
    while (longan::webxfer_poll_launch(&stale)) {}  // clear earlier test state

    const std::string game = "AutopilotGame";
    const std::string gdir = root + "/games/" + game;
    {
      std::string mk = "mkdir -p '" + gdir + "/.tiramisu'";
      (void)system(mk.c_str());
    }
    // A failed boot's log, with a signature declared in tools/fix_db.json.
    {
      FILE* lf = fopen((root + "/tiramisu.log").c_str(), "wb");
      if (lf) {
        fputs("trt: FAIL IMPORT msvcp140.dll!?_Xlength_error@std@@YAXPEBD@Z\n"
              "trt: STUB d3dcompiler_47.dll!D3DCompile -> fail-visible thunk\n"
              "host: guest did not reach PUMP\n", lf);
        fclose(lf);
      }
    }

    longan::webxfer_autopilot_begin(game);
    std::string res; int attempt = -1;
    longan::webxfer_autopilot_state(nullptr, &attempt, &res);
    CHECK(attempt == 0 && res == "armed",
          "transfer arms the in-console autopilot");

    longan::webxfer_set_launch_result(game, "started");
    longan::webxfer_autopilot_state(nullptr, &attempt, &res);
    CHECK(attempt == 1 && res == "fix",
          "launch outcome matched a declared failure and applied a fix");
    std::string queued;
    CHECK(longan::webxfer_poll_launch(&queued) && queued == game,
          "repaired title is re-queued through main.cpp's launch handoff");

    const std::string aliases = read_str_file(gdir + "/.tiramisu/aliases.json");
    CHECK(aliases.find("msvcp140_app.dll") != std::string::npos &&
              aliases.find("d3dcompiler_47.dll") != std::string::npos,
          "real DLL forwarders written into the title package");
    const std::string hist = read_str_file(gdir + "/.tiramisu/autopilot.json");
    CHECK(hist.find("\"result\":\"fix\"") != std::string::npos,
          "attempt history records the applied fix");
    std::string status = http_req(port, "GET", "/api/status", "");
    CHECK(status.find("\"autopilot_game\":\"AutopilotGame\"") != std::string::npos &&
              status.find("\"autopilot_attempt\":1") != std::string::npos &&
              status.find("\"autopilot_result\":\"fix\"") != std::string::npos,
          "/api/status exposes in-console autopilot state");

    // A failure with no declared repair must stop named, not retry forever.
    {
      FILE* lf = fopen((root + "/tiramisu.log").c_str(), "wb");
      if (lf) { fputs("trt: FAIL IMPORT mystery.dll!MysteryFn\n", lf); fclose(lf); }
    }
    longan::webxfer_set_launch_result(game, "started");
    longan::webxfer_autopilot_state(nullptr, &attempt, &res);
    CHECK(res == "no-fix" && !longan::webxfer_poll_launch(&stale),
          "unknown failure stops named with no retry");
    const std::string hist2 = read_str_file(gdir + "/.tiramisu/autopilot.json");
    CHECK(hist2.find("no-fix") != std::string::npos,
          "named stopping point persisted in attempt history");
  }

  longan::webxfer_shutdown();
  std::string cmd = "rm -rf " + root;
  (void)system(cmd.c_str());

  printf("  result: %d passed, %d failed\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
