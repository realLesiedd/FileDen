#!/usr/bin/env bash
# Builds the vendored libarchive for this PC (host tests and the PC preview), with OpenSSL for ZIP
# passwords, into build-host/. Prints the libraries to link.
#   LA=$(tools/host-libarchive.sh)
set -euo pipefail
repo=$(cd -- "$(dirname -- "$0")/.." && pwd)
b=$repo/build-host/libarchive
mkdir -p "$repo/build-host"
if [[ ! -f $b/libarchive/libarchive.a ]]; then
  opts=()
  for o in TEST TAR CPIO CAT UNZIP ACL XATTR ICONV LIBXML2 EXPAT LZ4 LZO LIBB2 PCREPOSIX PCRE2POSIX CNG MBEDTLS NETTLE INSTALL WERROR; do
    opts+=("-DENABLE_$o=OFF")
  done
  cmake -S "$repo/third_party/libarchive" -B "$b" -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF -DENABLE_OPENSSL=ON \
    "${opts[@]}" "$@" >"$b.log" 2>&1 || { cat "$b.log" >&2; exit 1; }
  make -C "$b" -j"$(nproc)" archive_static >>"$b.log" 2>&1 || { tail -30 "$b.log" >&2; exit 1; }
fi
echo "-I$repo/third_party/libarchive/libarchive $b/libarchive/libarchive.a -lcrypto -lzstd -llzma -lbz2 -lz"
