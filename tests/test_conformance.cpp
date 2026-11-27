// tiramisu/tests/test_conformance.cpp
// Wine-style conformance ratchet for the bridge surface.
//
// Tiramisu reimplements Windows, so its worst failure mode is not a missing
// function - it is a registered function whose calling convention the x86
// dispatcher cannot verify. Guessing a 4-argument stdcall used to pop 12
// extra bytes for a 1-argument API (OpenTTD jumped to 0x0000B5E3 that way).
// The dispatcher now stops at the named call when ABI metadata is missing.
//
// This test enumerates the whole registry, requires an ABI answer for every
// symbol Tiramisu claims to implement, and compares the remaining gaps with
// tests/abi-gaps.baseline: the list may shrink, never grow. Regenerate with
// `--write-baseline` after deliberately adding symbols that are not yet
// covered.
#include "../src/trt/trt.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>

namespace {

void* be_alloc(size_t bytes, u64 preferred_base) {
  (void)preferred_base;
  return malloc(bytes);
}
// 0 = success for this backend contract.
int be_protect(void* base, size_t bytes, unsigned prot) {
  (void)base; (void)bytes; (void)prot;
  return 0;
}
void be_release(void* base, size_t bytes) { (void)bytes; free(base); }
void be_log(const char* line) { (void)line; }

std::set<std::string> load_lines(const char* path) {
  std::set<std::string> out;
  FILE* f = fopen(path, "rb");
  if (!f) return out;
  char line[320];
  while (fgets(line, sizeof(line), f)) {
    size_t n = strlen(line);
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
    if (n == 0 || line[0] == '#') continue;
    out.insert(line);
  }
  fclose(f);
  return out;
}

}  // namespace

int main(int argc, char** argv) {
  const bool write_baseline = argc > 1 && strcmp(argv[1], "--write-baseline") == 0;
  const char* baseline_path = "tests/abi-gaps.baseline";

  trt_backend be{&be_alloc, &be_protect, &be_release, &be_log, nullptr};
  trt_bridge_boot(&be);

  std::vector<std::string> gaps;
  int exports = 0, builtin = 0, with_abi = 0, bad_class = 0, empty_names = 0;

  for (int i = 0;; ++i) {
    char mod[64] = {0};
    char sym[256] = {0};
    int cls = -1;
    int route = -1;
    if (!trt_module_export_at(i, mod, (int)sizeof mod, sym, (int)sizeof sym, &cls,
                              &route))
      break;
    ++exports;
    if (!mod[0] || !sym[0]) ++empty_names;
    if (cls < TRT_IMPL_REAL || cls > TRT_IMPL_UNRESOLVED) ++bad_class;
    // Only BUILTIN modules are ours: SYSTEM routes are the console's real DLLs
    // and STUB routes are fail-visible on purpose, so neither is dispatched
    // through the x86 argument table.
    if (route != TRT_ROUTE_BUILTIN) continue;
    ++builtin;
    if (trt_abi_arg_count(mod, sym) >= 0) {
      ++with_abi;
      continue;
    }
    gaps.push_back(std::string(mod) + "!" + sym);
  }
  std::sort(gaps.begin(), gaps.end());
  gaps.erase(std::unique(gaps.begin(), gaps.end()), gaps.end());

  printf("CONFORMANCE: exports=%d builtin=%d with_abi=%d gaps=%d\n", exports,
         builtin, with_abi, (int)gaps.size());

  if (write_baseline) {
    FILE* f = fopen(baseline_path, "wb");
    if (!f) {
      printf("CONFORMANCE FAIL: cannot write %s\n", baseline_path);
      return 1;
    }
    fprintf(f,
            "# module!symbol that still need an x86 ABI entry. Regenerate:\n"
            "#   tests/out/test_conformance --write-baseline\n"
            "# Sorted. The suite fails when a NEW symbol appears here.\n");
    for (const auto& g : gaps) fprintf(f, "%s\n", g.c_str());
    fclose(f);
    printf("CONFORMANCE: wrote %s (%d entries)\n", baseline_path,
           (int)gaps.size());
    return 0;
  }

  const std::set<std::string> baseline = load_lines(baseline_path);
  if (baseline.empty()) {
    printf("CONFORMANCE FAIL: %s missing or empty; run --write-baseline once\n",
           baseline_path);
    return 1;
  }

  int fresh = 0, fixed = 0;
  for (const auto& g : gaps) {
    if (baseline.count(g)) continue;
    if (fresh < 12) printf("NEW GAP %s (no ABI entry: the dispatcher would guess)\n",
                           g.c_str());
    ++fresh;
  }
  for (const auto& b : baseline) {
    if (std::binary_search(gaps.begin(), gaps.end(), b)) continue;
    if (fixed < 12) printf("NOW COVERED %s (remove from the baseline)\n", b.c_str());
    ++fixed;
  }

  printf("CONFORMANCE: new=%d now_covered=%d baseline=%d remaining=%d\n", fresh,
         fixed, (int)baseline.size(), (int)gaps.size());
  // Scope matters: this table is the PE32 trap dispatcher's. A 64-bit guest
  // passes arguments in registers and the callee never pops, so a missing
  // entry is harmless there. PE32 execution needs this metadata to safely
  // pass arguments and return to the guest.
  printf("CONFORMANCE: %d of %d claimed BUILTIN symbol(s) have an exact x86 "
         "ABI entry (gaps affect PE32 titles only)\n",
         with_abi, builtin);

  int failures = 0;
  if (empty_names) {
    printf("FAIL %d registered entry has an empty module or symbol name\n",
           empty_names);
    ++failures;
  }
  if (bad_class) {
    printf("FAIL %d registered symbol has an out-of-range impl class\n", bad_class);
    ++failures;
  }
  if (fresh) {
    printf("FAIL %d symbol(s) lost their ABI entry (the guest stack would "
           "corrupt at runtime)\n",
           fresh);
    ++failures;
  }
  printf("%s\n", failures ? "RESULT: FAIL" : "RESULT: ALL PASS");
  return failures ? 1 : 0;
}
