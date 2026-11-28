// tiramisu/src/host/webxfer_pe_scan.h
// Lightweight pre-launch PE header & import table inspector for the LAN dashboard.
// Included inside webxfer.cpp's anonymous namespace (uses XFER_SEP, xfer_lower,
// xfer_fseek, dir_ent, and list_dir_entries).
#ifndef TIRAMISU_WEBXFER_PE_SCAN_H
#define TIRAMISU_WEBXFER_PE_SCAN_H

struct pe_compat_info {
  std::string arch = "x64";
  std::string badge = "Windows x64";
  bool ok = true;
};

static pe_compat_info inspect_pe_compat(const std::string& gdir,
                                        const std::string& exe_rel) {
  pe_compat_info out;
  const std::string sep(1, XFER_SEP);
  const std::string full = gdir + sep + exe_rel;
  FILE* f = fopen(full.c_str(), "rb");
  if (!f) {
    out.arch = "unknown";
    out.badge = "exe unreadable";
    out.ok = false;
    return out;
  }
  uint8_t hdr[4096] = {};
  const size_t got = fread(hdr, 1, sizeof(hdr), f);
  if (got < 0x40 || hdr[0] != 'M' || hdr[1] != 'Z') {
    fclose(f);
    out.arch = "unknown";
    out.badge = "not a Windows PE";
    out.ok = false;
    return out;
  }
  uint32_t e_lfanew = 0;
  memcpy(&e_lfanew, hdr + 0x3C, 4);
  if (e_lfanew + 26 > got || hdr[e_lfanew] != 'P' || hdr[e_lfanew + 1] != 'E') {
    fclose(f);
    out.arch = "unknown";
    out.badge = "invalid PE header";
    out.ok = false;
    return out;
  }
  const uint16_t machine = (uint16_t)(hdr[e_lfanew + 4] | (hdr[e_lfanew + 5] << 8));
  const uint16_t num_sec = (uint16_t)(hdr[e_lfanew + 6] | (hdr[e_lfanew + 7] << 8));
  const uint16_t opt_size = (uint16_t)(hdr[e_lfanew + 20] | (hdr[e_lfanew + 21] << 8));
  const uint16_t opt_magic = (uint16_t)(hdr[e_lfanew + 24] | (hdr[e_lfanew + 25] << 8));
  if (machine == 0x8664 && opt_magic == 0x20B) {
    out.arch = "x64";
  } else if (machine == 0x014C) {
    // Tiramisu routes PE32 binaries through the x86-32 interpreter/JIT (x86_run_pe32).
    out.arch = "x86";
    out.badge = "x86 32-bit (JIT)";
    out.ok = true;
    fclose(f);
    return out;
  } else if (machine == 0xAA64) {
    fclose(f);
    out.arch = "arm64";
    out.badge = "ARM64 (Unsupported)";
    out.ok = false;
    return out;
  } else {
    fclose(f);
    out.arch = "unknown";
    out.badge = "Unsupported Arch";
    out.ok = false;
    return out;
  }

  uint32_t imp_rva = 0;
  const uint32_t opt_off = e_lfanew + 24;
  if (opt_off + 128 <= got) memcpy(&imp_rva, hdr + opt_off + 120, 4);
  const uint32_t sec_off = opt_off + opt_size;
  auto rva_to_file = [&](uint32_t rva) -> uint64_t {
    const uint32_t secs = num_sec < 96 ? num_sec : 96;
    for (uint32_t s = 0; s < secs; ++s) {
      const uint32_t so = sec_off + s * 40;
      if (so + 40 > got) break;
      uint32_t vaddr = 0, vsize = 0, raw = 0, rawsz = 0;
      memcpy(&vsize, hdr + so + 8, 4);
      memcpy(&vaddr, hdr + so + 12, 4);
      memcpy(&rawsz, hdr + so + 16, 4);
      memcpy(&raw, hdr + so + 20, 4);
      const uint32_t span = vsize > rawsz ? vsize : rawsz;
      if (rva >= vaddr && rva < vaddr + span && (rva - vaddr) < rawsz) {
        return (uint64_t)raw + (uint64_t)(rva - vaddr);
      }
    }
    return 0;
  };

  bool has_d3d11 = false, has_d3d12 = false, has_vk = false;
  bool has_gl = false, has_d3d9 = false, has_unity = false, has_ac = false;
  bool has_gdk = false;
  const uint64_t imp_file = imp_rva ? rva_to_file(imp_rva) : 0;
  if (imp_file > 0) {
    uint8_t desc[2048] = {};
    if (xfer_fseek(f, (long long)imp_file, SEEK_SET) == 0) {
      const size_t dgot = fread(desc, 1, sizeof(desc), f);
      for (uint32_t d = 0; d * 20 + 20 <= dgot; ++d) {
        uint32_t name_rva = 0;
        memcpy(&name_rva, desc + d * 20 + 12, 4);
        if (!name_rva) break;
        const uint64_t nfile = rva_to_file(name_rva);
        if (!nfile || xfer_fseek(f, (long long)nfile, SEEK_SET) != 0) break;
        char nm[64] = {};
        if (fread(nm, 1, sizeof(nm) - 1, f) == 0) continue;
        const std::string low = xfer_lower(nm);
        if (low.rfind("d3d11", 0) == 0) has_d3d11 = true;
        else if (low.rfind("d3d12", 0) == 0) has_d3d12 = true;
        else if (low.rfind("vulkan-1", 0) == 0) has_vk = true;
        else if (low.rfind("opengl32", 0) == 0) has_gl = true;
        else if (low.rfind("d3d9", 0) == 0 || low.rfind("d3d8", 0) == 0 ||
                 low.rfind("ddraw", 0) == 0) has_d3d9 = true;
        else if (low.rfind("unityplayer", 0) == 0) has_unity = true;
        else if (low.find("easyanticheat") != std::string::npos ||
                 low.find("battleye") != std::string::npos) has_ac = true;
        else if (low.rfind("xgameruntime", 0) == 0 ||
                 low.rfind("gamingservices", 0) == 0 ||
                 low.rfind("gameos", 0) == 0 ||
                 low.rfind("libhttpclient.gdk", 0) == 0) has_gdk = true;
      }
    }
  }
  fclose(f);

  std::vector<dir_ent> top;
  list_dir_entries(gdir, &top);
  for (const auto& e : top) {
    const std::string low = xfer_lower(e.name);
    if (low.find("easyanticheat") != std::string::npos ||
        low.find("battleye") != std::string::npos) {
      has_ac = true;
    }
    if (low == "microsoftgame.config" || low == "gameos.xvd" ||
        low.rfind("libhttpclient.gdk", 0) == 0) {
      has_gdk = true;
    }
  }

  if (has_ac) {
    out.badge = "Anti-Cheat Detected (Blocked)";
    out.ok = false;
  } else if (has_gdk) {
    out.badge = "Xbox GDK / GameOS build (Unsupported)";
    out.ok = false;
  } else if (has_d3d12 && !has_d3d11) {
    out.badge = "x64 D3D12 (Experimental)";
  } else if (has_vk) {
    out.badge = "x64 Vulkan Ready";
  } else if (has_unity) {
    out.badge = "x64 Unity Ready";
  } else if (has_d3d11) {
    out.badge = "x64 D3D11 Ready";
  } else if (has_gl && !has_d3d11) {
    out.badge = "x64 OpenGL (Experimental)";
  } else if (has_d3d9) {
    out.badge = "x64 D3D9 Ready";
  } else {
    out.badge = "x64 Windows Ready";
  }
  return out;
}

#endif
