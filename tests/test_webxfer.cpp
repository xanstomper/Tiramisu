// tiramisu/tests/test_webxfer.cpp
// End-to-end TCP HTTP verification of src/host/webxfer.cpp:
// LAN IP ranking, folder streaming upload, zip streaming extraction,
// path traversal rejection, per-game config persistence, launch/stop/rescan/delete,
// Expect: 100-continue handling, and live log tailing.
#include "../src/host/webxfer.h"
#include "../src/host/usb_volume_filter.h"
#include "../src/host/host_log.h"
#include "../src/host/vendor/miniz.h"

#include <arpa/inet.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <netinet/in.h>
#include <string>
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
                     const char* extra_hdrs = "",
                     size_t declared_length = std::string::npos) {
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

  std::string length_header;
  if (!strstr(extra_hdrs, "Transfer-Encoding:")) {
    const auto length = declared_length == std::string::npos ? body.size() : declared_length;
    length_header = "Content-Length: " + std::to_string(length) + "\r\n";
  }
  char hdr[1024];
  int hl = snprintf(hdr, sizeof(hdr),
                    "%s %s HTTP/1.1\r\nHost: 127.0.0.1:%u\r\n"
                    "%s%sConnection: close\r\n\r\n",
                    method.c_str(), path.c_str(), (unsigned)port,
                    length_header.c_str(), extra_hdrs);
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

  shutdown(fd, SHUT_WR);
  std::string resp;
  char buf[4096];
  ssize_t n;
  while ((n = recv(fd, buf, sizeof(buf), 0)) > 0) {
    resp.append(buf, (size_t)n);
  }
  close(fd);
  return resp;
}

std::string read_str_file(const std::string& p) {
  FILE* f = fopen(p.c_str(), "rb");
  if (!f) return {};
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (sz <= 0) { fclose(f); return {}; }
  std::string out((size_t)sz, '\0');
  size_t n = fread(out.data(), 1, (size_t)sz, f);
  fclose(f);
  out.resize(n);
  return out;
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

    // Issue #1 regression: legitimate folder/file names containing '..' inside a
    // component (not traversal) and 0-byte folder files must still upload cleanly.
    std::string r_dots = http_req(
        port, "PUT", "/api/upload-file?game=Game..v1&rel=sub..dir/empty..bin&batch=1", "");
    std::string r_done = http_req(port, "POST", "/api/upload-complete?game=Game..v1", "");
    CHECK(r_dots.find("200 OK") != std::string::npos &&
              r_done.find("200 OK") != std::string::npos &&
              read_str_file(root + "/games/Game..v1/sub..dir/empty..bin").empty(),
          "Folder upload accepts non-traversal '..' names, 0-byte files, and upload-complete");
    std::string rescan_game;
    CHECK(longan::webxfer_poll_rescan(&rescan_game) && rescan_game == "Game..v1",
          "/api/upload-complete triggers scoped host rescan");
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

    const std::string exe_route = "/api/upload-file?game=HollowDemo&rel=HollowDemo.exe";
    const std::string exe_path = root + "/games/HollowDemo/HollowDemo.exe";
    std::string partial = http_req(port, "PUT", exe_route, "partial", "", 12);
    CHECK(partial.find("500 Server Error") != std::string::npos &&
              read_str_file(exe_path) == exe_bytes,
          "Disconnected replacement leaves the installed file intact");
    constexpr const char* chunked = "Transfer-Encoding: chunked\r\n";
    std::string malformed = http_req(port, "PUT", exe_route, "3\r\nnew!\n0\r\n\r\n", chunked);
    CHECK(malformed.find("500 Server Error") != std::string::npos &&
              read_str_file(exe_path) == exe_bytes,
          "Malformed chunk framing leaves the installed file intact");
    std::string truncated = http_req(port, "PUT", exe_route, "3\r\nnew\r\n0\r\n", chunked);
    CHECK(truncated.find("500 Server Error") != std::string::npos &&
              read_str_file(exe_path) == exe_bytes,
          "Incomplete chunk terminator leaves the installed file intact");
    std::string complete = http_req(port, "PUT", exe_route, "3\r\nnew\r\n0\r\n\r\n", chunked);
    CHECK(complete.find("200 OK") != std::string::npos && read_str_file(exe_path) == "new",
          "Complete chunked replacement publishes the new file");
    std::string empty = http_req(port, "PUT", exe_route, "");
    CHECK(empty.find("200 OK") != std::string::npos && file_exists(exe_path) &&
              read_str_file(exe_path).empty(),
          "A complete zero-byte upload replaces the old file");
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

  // Device Portal drop folder shares the library and publishes complete ZIPs.
  {
    const std::string drop = root + "/" + longan::kPortalGamesFolder;
    const std::string folder = drop + "/PortalFolder";
    CHECK(mkdir(folder.c_str(), 0755) == 0,
          "startup created the Device Portal drop folder");
    FILE* exe = fopen((folder + "/PortalFolder.exe").c_str(), "wb");
    if (exe) {
      fputs("MZ_PORTAL_FOLDER", exe);
      fclose(exe);
    }
    char found_exe[256]{};
    std::string found_dir;
    const std::string listed = http_req(port, "GET", "/games", "");
    CHECK(listed.find("PortalFolder") != std::string::npos &&
              longan::webxfer_find_game("PortalFolder", found_exe, sizeof(found_exe), &found_dir) &&
              found_dir == folder,
          "Device Portal folders are listed and resolved by the dashboard");

    const std::string zip = drop + "/PortalZip.zip";
    mz_zip_archive writer{};
    const char* bytes = "MZ_PORTAL_ZIP";
    const bool created = mz_zip_writer_init_file(&writer, zip.c_str(), 0) &&
        mz_zip_writer_add_mem(&writer, "PortalZip/PortalZip.exe", bytes, strlen(bytes), 0) &&
        mz_zip_writer_finalize_archive(&writer);
    mz_zip_writer_end(&writer);
    CHECK(created, "created a Device Portal ZIP with a wrapping folder");

    std::string note;
    const std::string installed = root + "/games/PortalZip/PortalZip.exe";
    CHECK(longan::webxfer_import_portal_zip(root, "PortalZip.zip", &note) &&
              read_str_file(installed) == bytes,
          "Device Portal ZIP is extracted and published with its original bytes");
    std::string scope = "game";
    CHECK(longan::webxfer_poll_rescan(&scope) && scope.empty(),
          "Device Portal ZIP publication queues a full library refresh");
    CHECK(!longan::webxfer_import_portal_zip(root, "PortalZip.zip", &note) &&
              read_str_file(installed) == bytes,
          "a duplicate Portal ZIP leaves the existing game intact");

    FILE* incomplete = fopen((drop + "/Partial.zip").c_str(), "wb");
    if (incomplete) {
      fputs("PK\3\4", incomplete);
      fclose(incomplete);
    }
    CHECK(!longan::webxfer_import_portal_zip(root, "Partial.zip", &note) &&
              !file_exists(root + "/games/Partial") && file_exists(drop + "/Partial.zip"),
          "an incomplete Portal ZIP stays in place without publishing a game");
    CHECK(!longan::webxfer_import_portal_zip(root, "../PortalZip.zip", &note),
          "the Portal importer accepts only a filename in its drop folder");
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

    const auto stable_ini = read_str_file(root + "/settings.ini");
    std::string nested = http_req(port, "POST", "/api/settings",
                                  R"({"metadata":{"mouse_speed":21}})");
    std::string trailing = http_req(port, "POST", "/api/settings",
                                    R"({"mouse_speed":21} [])");
    std::string invalid_text = http_req(port, "POST", "/api/settings",
                                        R"({"default_input":"auto\n"})");
    CHECK(nested.find("400 Bad Request") != std::string::npos &&
              trailing.find("400 Bad Request") != std::string::npos &&
              invalid_text.find("400 Bad Request") != std::string::npos &&
              read_str_file(root + "/settings.ini") == stable_ini,
          "Invalid settings objects do not alter saved settings");

    std::string wrong_type = http_req(port, "POST", "/api/settings",
                                      R"({"default_input":false,"note":"auto","mouse_speed":72.5})");
    std::string after_wrong = http_req(port, "GET", "/api/settings", "");
    CHECK(wrong_type.find("200 OK") != std::string::npos &&
              after_wrong.find("\"default_input\":\"pad2kbm\"") != std::string::npos &&
              after_wrong.find("\"mouse_speed\":100") != std::string::npos,
          "A different JSON type does not overwrite a typed setting");

    std::string escaped = http_req(port, "POST", "/api/settings",
                                   R"({"default_input":"\u0061uto"})");
    std::string after_escape = http_req(port, "GET", "/api/settings", "");
    CHECK(escaped.find("200 OK") != std::string::npos &&
              after_escape.find("\"default_input\":\"auto\"") != std::string::npos,
          "JSON string escapes round-trip through the settings endpoint");

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

  // ---- External USB storage (NTFS + exFAT) & USB install workflow ----
  {
    std::string usb_ntfs_root = root + "/usb_ntfs";
    std::string usb_ntfs_game = usb_ntfs_root + "/Games/NtfsGame";
    {
      std::string mk = "mkdir -p '" + usb_ntfs_game + "'";
      (void)system(mk.c_str());
      FILE* ef = fopen((usb_ntfs_game + "/NtfsGame.exe").c_str(), "wb");
      if (ef) { fputs("MZ\x90\x00", ef); fclose(ef); }
    }
    longan::webxfer_storage_volume ntfs_vol;
    ntfs_vol.name = "E: MYUSB";
    ntfs_vol.path = usb_ntfs_root;
    ntfs_vol.label = "MYUSB";
    ntfs_vol.fs = "NTFS";
    ntfs_vol.total_bytes = 64ULL * 1024 * 1024 * 1024;
    ntfs_vol.free_bytes = 32ULL * 1024 * 1024 * 1024;
    ntfs_vol.win32_readable = true;
    ntfs_vol.broker_readable = true;
    ntfs_vol.game_roots = {usb_ntfs_root + "/Games"};
    ntfs_vol.items.push_back({"usb-1", "NtfsGame", usb_ntfs_root, "NTFS",
                              usb_ntfs_game, "folder", "NtfsGame.exe", 4096, true, false});

    longan::webxfer_storage_volume exfat_vol;
    exfat_vol.name = "F: EXFATUSB";
    exfat_vol.path = "F:\\";
    exfat_vol.label = "EXFATUSB";
    exfat_vol.fs = "exFAT";
    exfat_vol.total_bytes = 128ULL * 1024 * 1024 * 1024;
    exfat_vol.free_bytes = 96ULL * 1024 * 1024 * 1024;
    exfat_vol.win32_readable = false;
    exfat_vol.broker_readable = true;
    exfat_vol.items.push_back({"usb-2", "ExfatPacked", "F:\\", "exFAT",
                               "F:\\Games\\ExfatPacked.zip", "zip", "", 1048576, false, false});

    longan::webxfer_set_external_storage({ntfs_vol, exfat_vol}, "ready");

    const std::string storage = http_req(port, "GET", "/api/storage", "");
    CHECK(storage.find("\"fs\":\"NTFS\"") != std::string::npos &&
              storage.find("\"fs\":\"exFAT\"") != std::string::npos &&
              storage.find("\"id\":\"usb-1\"") != std::string::npos &&
              storage.find("\"id\":\"usb-2\"") != std::string::npos,
          "/api/storage exposes both NTFS & exFAT volumes and downloadable items");

    const std::string games = http_req(port, "GET", "/games", "");
    CHECK(games.find("\"name\":\"NtfsGame\"") != std::string::npos &&
              games.find("\"location\":\"usb\"") != std::string::npos &&
              games.find("\"fs\":\"NTFS\"") != std::string::npos,
          "/games includes external NTFS USB game with metadata");

    const std::string rescan_resp = http_req(port, "POST", "/api/usb/rescan", "");
    CHECK(rescan_resp.find("202 Accepted") != std::string::npos &&
              longan::webxfer_poll_rescan(),
          "POST /api/usb/rescan triggers full storage rescan");

    const std::string bad_inst = http_req(port, "POST", "/api/usb/install", "{\"id\":\"missing\"}");
    CHECK(bad_inst.find("404 Not Found") != std::string::npos,
          "POST /api/usb/install rejects unknown USB item id");

    const std::string ok_inst = http_req(port, "POST", "/api/usb/install", "{\"id\":\"usb-2\"}");
    std::string pid, pname;
    CHECK(ok_inst.find("202 Accepted") != std::string::npos &&
              longan::webxfer_poll_usb_install(&pid, &pname) &&
              pid == "usb-2" && pname == "ExfatPacked",
          "POST /api/usb/install queues USB archive download/install for host");

    longan::webxfer_set_usb_install_status(true, "Copying ExfatPacked", 512, 1024, "");
    const std::string st_usb = http_req(port, "GET", "/api/status", "");
    CHECK(st_usb.find("\"download_phase\":\"Copying ExfatPacked\"") != std::string::npos &&
              st_usb.find("\"download_done\":512") != std::string::npos &&
              st_usb.find("\"download_total\":1024") != std::string::npos,
          "/api/status surfaces live USB install progress");
    longan::webxfer_set_usb_install_status(false, "", 1024, 1024, "", "USB -> ExfatPacked");
    longan::webxfer_set_external_storage({}, "ready");
  }

  // 14. Turbo LAN binary batch upload (/api/upload-batch) + path traversal guard
  {
    std::string batch;
    auto add_rec = [&](const std::string& rel, const std::string& data) {
      uint32_t plen = (uint32_t)rel.size();
      uint64_t dlen = (uint64_t)data.size();
      batch.append(reinterpret_cast<const char*>(&plen), 4);
      batch.append(rel);
      batch.append(reinterpret_cast<const char*>(&dlen), 8);
      batch.append(data);
    };
    add_rec("TurboGame.exe", "MZ_TURBO_BIN");
    add_rec("assets/level1/map.dat", std::string(4096, 'M'));
    add_rec("assets/level2/audio.ogg", "OggS_TEST_DATA");

    const std::string resp = http_req(
        port, "POST", "/api/upload-batch?game=TurboBatch&batch=1", batch);
    CHECK(resp.find("200 OK") != std::string::npos &&
              resp.find("\"files\":3") != std::string::npos,
          "POST /api/upload-batch streams multiple nested files in one request");
    CHECK(read_str_file(root + "/games/TurboBatch/TurboGame.exe") == "MZ_TURBO_BIN" &&
              read_str_file(root + "/games/TurboBatch/assets/level1/map.dat").size() == 4096 &&
              read_str_file(root + "/games/TurboBatch/assets/level2/audio.ogg") == "OggS_TEST_DATA",
          "batch uploaded files match exact bytes on disk");

    // Path traversal inside binary batch must be rejected
    std::string evil;
    uint32_t plen = 12;
    uint64_t dlen = 4;
    evil.append(reinterpret_cast<const char*>(&plen), 4);
    evil.append("../evil.txt!");
    evil.append(reinterpret_cast<const char*>(&dlen), 8);
    evil.append("NOPE");
    const std::string eresp = http_req(
        port, "POST", "/api/upload-batch?game=TurboBatch&batch=1", evil);
    CHECK(eresp.find("500") != std::string::npos || eresp.find("400") != std::string::npos,
          "POST /api/upload-batch rejects path traversal inside binary frame");
  }

  // 15. Parallel striped chunk upload (init -> out-of-order chunks + retry -> commit)
  {
    std::string big(300000, '\0');
    for (size_t i = 0; i < big.size(); ++i) big[i] = (char)('A' + (i % 26));
    const std::string init_r = http_req(
        port, "POST",
        "/api/upload-chunk-init?game=TurboBatch&rel=data/pak0.pak&size=" +
            std::to_string(big.size()) + "&batch=1",
        "");
    CHECK(init_r.find("200 OK") != std::string::npos &&
              init_r.find("\"upload_id\":\"") != std::string::npos,
          "POST /api/upload-chunk-init creates striped upload session");
    std::string sid;
    const size_t pos = init_r.find("\"upload_id\":\"");
    if (pos != std::string::npos) {
      const size_t st = pos + 13;
      const size_t en = init_r.find('"', st);
      if (en != std::string::npos) sid = init_r.substr(st, en - st);
    }
    // Upload chunks out-of-order + retry chunk 1
    const std::string c2 = http_req(port, "PUT", "/api/upload-chunk?id=" + sid + "&offset=200000",
                                    big.substr(200000, 100000));
    const std::string c0 = http_req(port, "PUT", "/api/upload-chunk?id=" + sid + "&offset=0",
                                    big.substr(0, 100000));
    const std::string c1 = http_req(port, "PUT", "/api/upload-chunk?id=" + sid + "&offset=100000",
                                    big.substr(100000, 100000));
    const std::string c1_retry = http_req(port, "PUT", "/api/upload-chunk?id=" + sid + "&offset=100000",
                                          big.substr(100000, 100000));
    CHECK(c0.find("200 OK") != std::string::npos && c1.find("200 OK") != std::string::npos &&
              c2.find("200 OK") != std::string::npos && c1_retry.find("200 OK") != std::string::npos,
          "PUT /api/upload-chunk accepts out-of-order and retried chunks");
    const std::string com = http_req(port, "POST", "/api/upload-chunk-commit?id=" + sid, "");
    CHECK(com.find("200 OK") != std::string::npos &&
              read_str_file(root + "/games/TurboBatch/data/pak0.pak") == big,
          "POST /api/upload-chunk-commit verifies and publishes exact reassembled file");
  }

  // 16. Striped chunked .zip upload -> extract_zip with buffered callback writer
  {
    std::string zpath = root + "/chunked_src.zip";
    mz_zip_archive zw{};
    mz_zip_writer_init_file(&zw, zpath.c_str(), 0);
    const std::string exe_data(120000, 'Z');
    mz_zip_writer_add_mem(&zw, "ChunkZipGame/ChunkZipGame.exe", exe_data.data(), exe_data.size(), 0);
    mz_zip_writer_finalize_archive(&zw);
    mz_zip_writer_end(&zw);
    const std::string zbytes = read_str_file(zpath);
    const size_t half = zbytes.size() / 2;

    const std::string zir = http_req(
        port, "POST",
        "/api/upload-chunk-init?name=ChunkZipGame.zip&size=" + std::to_string(zbytes.size()), "");
    std::string zsid;
    const size_t zp = zir.find("\"upload_id\":\"");
    if (zp != std::string::npos) zsid = zir.substr(zp + 13, zir.find('"', zp + 13) - (zp + 13));
    const std::string zc1 = http_req(port, "PUT", "/api/upload-chunk?id=" + zsid + "&offset=" + std::to_string(half),
                                     zbytes.substr(half));
    const std::string zc0 = http_req(port, "PUT", "/api/upload-chunk?id=" + zsid + "&offset=0",
                                     zbytes.substr(0, half));
    const std::string zcom = http_req(port, "POST", "/api/upload-chunk-commit?id=" + zsid, "");
    CHECK(zc0.find("200 OK") != std::string::npos && zc1.find("200 OK") != std::string::npos &&
              zcom.find("200 OK") != std::string::npos &&
              read_str_file(root + "/games/ChunkZipGame/ChunkZipGame.exe") == exe_data,
          "striped chunked .zip upload reassembles and extracts cleanly");
  }

  // 17. HTTP/1.1 Keep-Alive connection reuse across multiple upload requests
  {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
    bool ka_ok = (fd >= 0 && connect(fd, (sockaddr*)&sa, sizeof(sa)) == 0);
    if (ka_ok) {
      for (int i = 0; i < 3 && ka_ok; ++i) {
        std::string payload = "KA_FILE_" + std::to_string(i);
        char hdr[512];
        int hl = snprintf(hdr, sizeof(hdr),
                          "PUT /api/upload-file?game=KeepAliveGame&rel=f%d.txt&batch=1 HTTP/1.1\r\n"
                          "Host: 127.0.0.1:%u\r\nContent-Length: %zu\r\nConnection: keep-alive\r\n\r\n",
                          i, (unsigned)port, payload.size());
        std::string req(hdr, (size_t)hl);
        req += payload;
        if (send(fd, req.data(), req.size(), 0) != (ssize_t)req.size()) { ka_ok = false; break; }
        std::string resp;
        char buf[1024];
        size_t hdr_end = std::string::npos;
        size_t clen = 0;
        while (true) {
          ssize_t n = recv(fd, buf, sizeof(buf), 0);
          if (n <= 0) { ka_ok = false; break; }
          resp.append(buf, (size_t)n);
          if (hdr_end == std::string::npos) {
            size_t p = resp.find("\r\n\r\n");
            if (p != std::string::npos) {
              hdr_end = p + 4;
              size_t cl = resp.find("Content-Length: ");
              if (cl != std::string::npos) clen = (size_t)strtoull(resp.c_str() + cl + 16, nullptr, 10);
            }
          }
          if (hdr_end != std::string::npos && resp.size() >= hdr_end + clen) break;
        }
        if (resp.find("200 OK") == std::string::npos ||
            resp.find("Connection: keep-alive") == std::string::npos) {
          ka_ok = false;
        }
      }
      close(fd);
    }
    CHECK(ka_ok &&
              read_str_file(root + "/games/KeepAliveGame/f0.txt") == "KA_FILE_0" &&
              read_str_file(root + "/games/KeepAliveGame/f2.txt") == "KA_FILE_2",
          "HTTP/1.1 Keep-Alive reuses single TCP socket across multiple upload PUTs");
  }

  // 18. Xbox internal D: exclusion & external USB stick / SSD classification
  {
    std::string why;
    // Regression: Xbox Dev Mode internal D:\ NTFS partition with no USB plugged in
    longan::usb_drive_probe_info xbox_d{};
    xbox_d.letter = 'D';
    xbox_d.is_fixed = true;
    xbox_d.fs = "NTFS";
    xbox_d.label = "DevelopmentFiles";
    xbox_d.total_bytes = 512ULL << 30;
    xbox_d.win32_readable = true;
    xbox_d.os_drive = 'C';
    xbox_d.local_drive = 'Q';
    xbox_d.pkg_drive = 'S';
    CHECK(!longan::classify_external_usb_volume(xbox_d, &why),
          "Xbox internal D: (DRIVE_FIXED NTFS) is NEVER false-positived as USB");

    // Regression: Xbox internal D: even with empty label
    xbox_d.label.clear();
    CHECK(!longan::classify_external_usb_volume(xbox_d, &why),
          "Xbox internal D: (DRIVE_FIXED, blank label) is NEVER false-positived as USB");

    // Ghost inaccessible internal Xbox partitions (S:/T:/U:/X:)
    longan::usb_drive_probe_info ghost_x{};
    ghost_x.letter = 'X';
    ghost_x.is_fixed = true;
    ghost_x.os_drive = 'C';
    CHECK(!longan::classify_external_usb_volume(ghost_x, &why),
          "inaccessible internal Xbox fixed partition X: is skipped");

    // Fixed drive with internal Windows/Xbox directory markers
    longan::usb_drive_probe_info internal_e{};
    internal_e.letter = 'E';
    internal_e.is_fixed = true;
    internal_e.fs = "NTFS";
    internal_e.total_bytes = 1000ULL << 30;
    internal_e.win32_readable = true;
    internal_e.has_internal_os_dirs = true;
    CHECK(!longan::classify_external_usb_volume(internal_e, &why),
          "fixed drive containing internal OS/DevelopmentFiles directories is skipped");

    // Genuine external USB SSD on E: (DRIVE_FIXED NTFS, no internal markers)
    longan::usb_drive_probe_info ext_ssd{};
    ext_ssd.letter = 'E';
    ext_ssd.is_fixed = true;
    ext_ssd.fs = "NTFS";
    ext_ssd.label = "Samsung T7";
    ext_ssd.total_bytes = 931ULL << 30;
    ext_ssd.win32_readable = true;
    ext_ssd.os_drive = 'C';
    ext_ssd.local_drive = 'Q';
    ext_ssd.pkg_drive = 'S';
    CHECK(longan::classify_external_usb_volume(ext_ssd, &why),
          "real external NTFS USB SSD on E: (DRIVE_FIXED) IS detected");

    // Genuine external exFAT USB flash stick on F: (DRIVE_REMOVABLE)
    longan::usb_drive_probe_info ext_stick{};
    ext_stick.letter = 'F';
    ext_stick.is_removable = true;
    ext_stick.fs = "exFAT";
    CHECK(longan::classify_external_usb_volume(ext_stick, &why),
          "real external exFAT USB stick on F: (DRIVE_REMOVABLE) IS detected");

    // Authoritative WinRT RemovableDevices / StorageDevice volume (even on D:)
    longan::usb_drive_probe_info os_usb{};
    os_usb.letter = 'D';
    os_usb.is_fixed = true;
    os_usb.from_os_usb_api = true;
    os_usb.fs = "NTFS";
    CHECK(longan::classify_external_usb_volume(os_usb, &why),
          "OS RemovableDevices / Portable StorageDevice volume is always accepted");
  }

  longan::webxfer_shutdown();
  std::string cmd = "rm -rf " + root;
  (void)system(cmd.c_str());

  printf("  result: %d passed, %d failed\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
