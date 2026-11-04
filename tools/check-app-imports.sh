#!/usr/bin/env bash
# Lists functions the full screen app calls that the PS5 app runtime doesn't provide.
# Those resolve to address 0, so the app crashes (SIGSEGV, RIP 0) the moment one is
# called; FileDen 0.0.1 crashed at launch on strcasestr this way. Run after
# app/fself/build.sh; exits 1 if anything is missing.
#   tools/check-app-imports.sh            (uses $WORK, default /tmp/fileden-fself)
set -euo pipefail
work=${WORK:-/tmp/fileden-fself}
sdk=$work/.deps/native/ps5-payload-sdk
rt=$work/tooling/native/runtime
elf=$work/build/llvm-pie.elf
[[ -f $elf ]] || { echo "no $elf: run app/fself/build.sh first" >&2; exit 2; }
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
# Sony libraries the app loads (libc comes from the runtime's libc.prx instead, and the
# WebKit POSIX library isn't there for an app like this)
ls "$sdk"/target/lib/libSce*.so "$sdk"/target/lib/libkernel*.so | grep -v -e LibcInternal -e PosixForWebKit |
  xargs "$sdk/bin/llvm-nm" -D --defined-only 2>/dev/null | awk 'NF>=2{print $NF}' | sort -u >"$tmp/sce"
"$sdk/bin/llvm-nm" -u "$elf" | awk '{print $NF}' | sort -u >"$tmp/undef"
missing=0
while read -r s; do
  [[ $s == __wrap_* || $s == __real_* ]] && continue
  grep -qx -- "$s" "$tmp/sce" && continue
  grep -q -- "^$s|" "$rt/imports.txt" && continue
  nid=$("$sdk/bin/prospero-nid" "$s" 2>/dev/null | awk '{print $NF}')
  grep -q -- "^$nid|" "$rt/api-surface.txt" && continue
  echo "missing in the app runtime: $s"
  missing=1
done <"$tmp/undef"
[[ $missing == 0 ]] && echo "app imports: all provided"
exit $missing
