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
#include <cstdlib>
#include <cstring>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <sys/resource.h>
#include <signal.h>
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

    // Interrupt a replacement after sending fewer bytes than Content-Length.
    // The already uploaded executable must remain intact.
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bool connected = fd >= 0 && connect(fd, (sockaddr*)&addr, sizeof(addr)) == 0;
    if (connected) {
      const std::string request =
          "PUT /api/upload-file?game=HollowDemo&rel=HollowDemo.exe HTTP/1.1\r\n"
          "Host: localhost\r\nContent-Length: 100\r\nConnection: close\r\n\r\npartial";
      send(fd, request.data(), request.size(), 0);
      shutdown(fd, SHUT_WR);
      char reply[512];
      while (recv(fd, reply, sizeof(reply), 0) > 0) {}
    }
    if (fd >= 0) close(fd);
    CHECK(connected && read_str_file(root + "/games/HollowDemo/HollowDemo.exe") == exe_bytes,
          "Interrupted replacement preserves previously uploaded file");

    // Limit process file writes to force fwrite/fclose failure without filling
    // the test machine's disk. A failed write must never return success.
    struct rlimit old_limit{};
    const bool got_limit = getrlimit(RLIMIT_FSIZE, &old_limit) == 0;
    struct rlimit small_limit = old_limit;
    small_limit.rlim_cur = 1;
    auto old_handler = signal(SIGXFSZ, SIG_IGN);
    const bool limited = got_limit && setrlimit(RLIMIT_FSIZE, &small_limit) == 0;
    std::string failed_reply;
    if (limited)
      failed_reply = http_req(port, "PUT",
          "/api/upload-file?game=HollowDemo&rel=diskfull.bin", "more than one byte");
    if (limited) setrlimit(RLIMIT_FSIZE, &old_limit);
    signal(SIGXFSZ, old_handler);
    CHECK(limited && failed_reply.find("500 Server Error") != std::string::npos &&
              !file_exists(root + "/games/HollowDemo/diskfull.bin"),
          "Storage write failure returns error and leaves no committed file");
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

    std::string rl = http_req(port, "POST", "/launch/HollowDemo", "");
    std::string ln;
    CHECK(rl.find("200 OK") != std::string::npos &&
              longan::webxfer_poll_launch(&ln) && ln == "HollowDemo",
          "POST /launch triggers host launch poll");

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

    // dashboard page carries the settings card + connect card
    std::string page = http_req(port, "GET", "/", "");
    CHECK(page.find("200 OK") != std::string::npos &&
              page.find("id=settings") != std::string::npos &&
              page.find("/api/settings") != std::string::npos &&
              page.find("Console connection") != std::string::npos,
          "dashboard HTML ships settings + connect cards");
  }

  longan::webxfer_shutdown();
  std::string cmd = "rm -rf " + root;
  (void)system(cmd.c_str());

  printf("  result: %d passed, %d failed\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
