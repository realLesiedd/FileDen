# Third-party notices

FileDen is licensed under GPL-3.0 (see [LICENSE](LICENSE)). It includes the components below, which keep
their own licenses. The license texts are next to the code in `third_party/`, or in the projects linked here
for the libraries linked in at build time.

| Component | License | Used for |
|---|---|---|
| [PS5 Payload SDK](https://github.com/ps5-payload-dev/sdk) © John Törnblom | GPL-3.0-or-later | Building the payload and the app |
| [ps5-native-app-boilerplate](https://github.com/blackbearreloaded/ps5-native-app-boilerplate) © BlackBearReloaded, through [ProsperoStore](https://github.com/darkxex/ProsperoStore) © darkxex | GPL-3.0-or-later | The app's build tooling, runtime (`libc.prx`) and memory heap |
| [libarchive](https://www.libarchive.org/) © Tim Kientzle and contributors | BSD-2-Clause | RAR, TAR, CAB, LHA, CPIO, ISO and the other archive formats; ZIP with a password (`third_party/libarchive`) |
| [OpenSSL](https://www.openssl.org/) © The OpenSSL Project Authors | Apache-2.0 | AES-256, SHA-256 and the key derivation for archives with a password |
| [zlib](https://zlib.net/) © Jean-loup Gailly and Mark Adler | zlib | gzip |
| [bzip2](https://sourceware.org/bzip2/) © Julian Seward | bzip2 (BSD-style) | bzip2 |
| [Zstandard](https://github.com/facebook/zstd) © Meta Platforms | BSD-3-Clause | zstd |
| [xz / liblzma](https://tukaani.org/xz/) | 0BSD | xz and lzma |
| [libwebp](https://chromium.googlesource.com/webm/libwebp) © Google | BSD-3-Clause | WebP pictures |
| [jxrlib](https://github.com/4creators/jxrlib) © Microsoft Corporation | BSD-2-Clause | PS5 HDR screenshots (`.jxr`) |
| [miniz](https://github.com/richgel999/miniz) © Rich Geldreich, RAD Game Tools, Valve Software | MIT | ZIP |
| [7-Zip C code / LZMA SDK](https://www.7-zip.org/sdk.html) by Igor Pavlov | Public domain | 7z, LZMA2, SHA-256 |
| [stb](https://github.com/nothings/stb) © Sean Barrett | MIT or public domain | Images and text drawing |
| [Figtree](https://github.com/erikdkennedy/figtree) © The Figtree Project Authors | SIL OFL 1.1 | The font |

The zlib, bzip2, Zstandard, xz, OpenSSL and libwebp libraries are the prebuilt
[ps5-payload-dev PacBrew ports](https://github.com/ps5-payload-dev/pacbrew-repo) (`tools/fetch-deps.sh`);
libarchive 3.7.4 is built from `third_party/libarchive`, with two small changes marked `FileDen:` (a 2 MB xz block
size, and a fix for a zstd memory leak in its 7z reader). RAR with a password is FileDen's own code (`rarcrypt.c`),
written from the RAR 5.0 format notes; it contains no unRAR code.
The sample archives in `tests/fixtures/` come from libarchive's tests (`LICENSE-libarchive`) and, for
`rar5-psw.rar`, `rar5-hpsw.rar` and `rar5-psw-blake.rar`, from [rarfile](https://github.com/markokr/rarfile) © Marko Kreen (ISC, `LICENSE-rarfile`).

## Apps in the screenshots

The screenshots show these PS5 homebrew apps from the [ProsperoStore catalog](https://github.com/darkxex/ps5-homebrew-catalog)
with their own names and icons. They belong to their authors and are not part of FileDen.

| App | Author | License |
|---|---|---|
| [ProsperoStore](https://github.com/darkxex/ProsperoStore) (`PPSA99000`) | AngelXex | GPL-3.0 |
| [ProsperoEden](https://github.com/Swordpdf/ProsperoEden) (`PPSA99008`) | BlackBearReloaded | GPL-3.0 |
| [PS5SX2](https://github.com/Swordpdf/PS5SX2) (`PPSA99203`) | Sword | GPL-3.0 |
| [PC5](https://github.com/bornaradusin/PC5) (`PPSA11018`) | bornaradusin | LGPL-2.1 |
| [Porpoise](https://github.com/elripalda/Porpoise-Dolphin-Emulator-for-PS5) (`PPSA99764`) | Ruben Ripalda | GPL-3.0 |
| [Breeze](https://github.com/MiladRTX/Breeze) (`PPSA92878`) | Milad | GPL-3.0 |
