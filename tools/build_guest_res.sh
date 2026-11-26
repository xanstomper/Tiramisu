#!/usr/bin/env bash
# tools/build_guest_res.sh — build the resource-test guest EXE:
# generate the hand-built .rsrc section -> assemble -> link -> patch the
# three absolute DataRva fields with the final .rsrc RVA.
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
cd "$here"
out=tests/out
mkdir -p "$out"
CLANG="${CLANG:-/usr/lib/llvm-19/bin/clang}"
[[ -x "$CLANG" ]] || CLANG="$HOME/.local/bin/clang-19"
LLD_LINK="$HOME/.local/bin/lld-link"

python3 tools/gen_guest_res.py "$out/guest_res_rsrc.s" "$out/guest_res_rsrc.o"
"$CLANG" --target=x86_64-pc-windows-msvc -O2 -c "$out/guest_res_rsrc.s" \
  -o "$out/guest_res_rsrc.obj"
"$CLANG" --target=x86_64-pc-windows-msvc -O2 -ffreestanding -fno-builtin \
  -c src/guest/guest_res.c -o "$out/guest_res.obj"
"$LLD_LINK" "$out/guest_res.obj" "$out/guest_res_rsrc.obj" \
  "$out/kernel32.lib" \
  /NODEFAULTLIB /ENTRY:guest_res_boot /SUBSYSTEM:WINDOWS /MACHINE:X64 \
  /DYNAMICBASE /BASE:0x1A00000 /OUT:"$out/guest_res.exe"

python3 - "$out/guest_res.exe" <<'EOF'
import struct, sys
p = sys.argv[1]
data = bytearray(open(p, 'rb').read())
e = struct.unpack_from('<I', data, 0x3C)[0]
nsec = struct.unpack_from('<H', data, e+6)[0]
optsz = struct.unpack_from('<H', data, e+20)[0]
sec0 = e + 24 + optsz
rsrc_rva = rsrc_raw = None
for i in range(nsec):
    off = sec0 + i*40
    name = data[off:off+8].rstrip(b'\0').decode()
    vsize, rva, rawsz, rawptr = struct.unpack_from('<IIII', data, off+8)
    if name == '.rsrc':
        rsrc_rva, rsrc_raw = rva, rawptr
assert rsrc_rva is not None, 'no .rsrc section in linked exe'
for de, blob in ((0x1C0, 0x2C0), (0x1D0, 0x2D0), (0x1E0, 0x2E0)):
    fo = rsrc_raw + de
    old = struct.unpack_from('<I', data, fo)[0]
    if old == 0x11223344:  # placeholder (fresh build)
        struct.pack_into('<I', data, fo, rsrc_rva + blob)
    else:
        assert old == rsrc_rva + blob, f'DataRva stale at {de:#x}'
open(p, 'wb').write(bytes(data))
print(f'guest_res.exe patched: .rsrc rva={rsrc_rva:#x}, 3 DataRva fields set')
EOF
echo "guest_res.exe ready"
