#!/usr/bin/env bash
# Fetches the prebuilt ps5-payload-dev PacBrew ports FileDen's payload links (zlib, bzip2, xz, zstd,
# OpenSSL's libcrypto and libwebp; libarchive itself is vendored in third_party/) into .deps/pacbrew. Same pinned release and checksum as the
# ProsperoStore app tooling uses. Only the needed headers and static libraries are unpacked.
set -euo pipefail
repo=$(cd -- "$(dirname -- "$0")/.." && pwd)
dir=$repo/.deps/pacbrew
version=v0.40.2
sha=a85f65de418a8e6a898c6c3e3c870d50fff7618a200e4dd59ea9692af6ecec4d
url=https://github.com/ps5-payload-dev/pacbrew-repo/releases/download/$version/ps5-payload-dev.tar.gz
pfx=opt/ps5-payload-sdk/target/user/homebrew
[[ -f $dir/$pfx/lib/libcrypto.a ]] && { echo "PacBrew ports: $dir/$pfx"; exit 0; }
mkdir -p "$dir"
tgz=$dir/ps5-payload-dev-$version.tar.gz
[[ -f $tgz ]] || curl -fL --retry 3 -o "$tgz" "$url"
echo "$sha  $tgz" | sha256sum -c -
tar -xzf "$tgz" -C "$dir" --wildcards \
  "$pfx/include/webp/*" "$pfx/include/zlib.h" "$pfx/include/zconf.h" "$pfx/include/bzlib.h" "$pfx/include/lzma.h" \
  "$pfx/include/lzma/*" "$pfx/include/zstd.h" "$pfx/include/zstd_errors.h" "$pfx/include/openssl/*" \
  "$pfx/lib/liblzma.a" "$pfx/lib/libzstd.a" "$pfx/lib/libbz2.a" "$pfx/lib/libz.a" "$pfx/lib/libcrypto.a" "$pfx/lib/libssl.a" \
  "$pfx/lib/libwebp.a" "$pfx/lib/libsharpyuv.a"
echo "PacBrew ports: $dir/$pfx"
