#!/usr/bin/env bash
# Builds the fake-signed (fself) FileDen app folder. Uses the native build tooling from ProsperoStore (darkxex/ProsperoStore,
# GPL-3.0-or-later; itself the ps5-native-app-boilerplate by BlackBearReloaded)
# for the PS5 metadata converter, the fself writer and the libc.prx runtime shim.
# That tooling is fetched into $WORK at build time; none of it is in this repo.
#   app/fself/build.sh /path/to/fileden.elf
set -euo pipefail
here=$(cd -- "$(dirname -- "$0")" && pwd)
repo=$(cd -- "$here/../.." && pwd)
elf=${1:?fileden.elf}
elf=$(cd -- "$(dirname -- "$elf")" && pwd)/$(basename -- "$elf")
work=${WORK:-/tmp/fileden-fself}
pin=22d5a44a053b4dad4f3a5867fe39d262a80a1b8c
id=$(python3 -c 'import json,sys;print(json.load(open(sys.argv[1]))["titleId"])' "$repo/sce_sys/param.json")
[[ -d $work ]] || git clone https://github.com/darkxex/ProsperoStore "$work"
cd "$work" && git checkout -q -f "$pin" && git clean -qfdx -e .deps
rm -rf src/* sce_sys/*
mkdir -p third_party
cp -a "$repo/third_party/stb" "$repo/third_party/figtree" third_party/
cp "$repo/app/app.c" "$repo/app/ui_core.h" "$repo/app/logo.h" src/
cp "$repo/version.h" .  # app.c includes ../version.h
# process heap big enough for the 1920x1080 frames and decoded images
cp "$repo/third_party/blackbearreloaded/app_heap.c" src/
cp "$repo/sce_sys/param.json" sce_sys/param.json
# the icon as a plain PNG (header, one image chunk, end): the PS5's own decoder, which draws it on the
# home screen and in ShadowMountPlus' "installed" pop-up, skips icons with extra chunks
python3 - "$repo/assets/icon0.png" sce_sys/icon0.png <<'PY'
import struct, sys, zlib
d = open(sys.argv[1], "rb").read()
assert d[:8] == b"\x89PNG\r\n\x1a\n", "icon0.png isn't a PNG"
i, ihdr, idat = 8, None, b""
while i < len(d):
    n, t = struct.unpack(">I4s", d[i:i + 8])
    if t == b"IHDR": ihdr = d[i + 8:i + 8 + n]
    elif t == b"IDAT": idat += d[i + 8:i + 8 + n]
    i += 12 + n
w, h, depth, ctype, _, _, lace = struct.unpack(">IIBBBBB", ihdr)
assert (w, h, depth, ctype, lace) == (512, 512, 8, 2, 0), "icon0.png must be 512x512, 8-bit RGB, not interlaced"
chunk = lambda t, b: struct.pack(">I", len(b)) + t + b + struct.pack(">I", zlib.crc32(t + b) & 0xffffffff)
open(sys.argv[2], "wb").write(d[:8] + chunk(b"IHDR", ihdr) + chunk(b"IDAT", idat) + chunk(b"IEND", b""))
PY
# PS5 container magic (54 14 F5 EE)
sed -i 's/^fself_magic=.*/fself_magic=0xEEF51454/' tools/build.sh
USE_CCACHE=0 PS5_CLANG=${PS5_CLANG:-clang} APP_ASSETS= APP_HEAP_SIZE=0x10000000 \
    APP_WRAP_SYMBOLS="malloc calloc realloc free posix_memalign malloc_usable_size" \
    APP_DEFINITIONS="_DEFAULT_SOURCE FILEDEN_FSELF" \
    bash tools/build.sh Folder
rm -rf "dist/$id/assets"
cp "$elf" "dist/$id/sce_sys/fileden.elf"
# The console only starts an app whose files are open to everyone (0777); with 0644
# files from a zip it answers CE-107750-0. Set the folder to 0777 and rebuild the zip
# (the tooling zipped it before the payload was added), then store every entry as 0777.
chmod -R 0777 "dist/$id"
rm -f "dist/$id.zip"
(cd dist && python3 -m zipfile -c "$id.zip" "$id")
python3 tools/store-zip-modes.py "dist/$id.zip"
echo "app folder: $work/dist/$id  (copy it to /data/homebrew/$id)"
echo "release zip: $work/dist/$id.zip"
