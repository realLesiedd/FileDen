// FileDen - archives for the background job: ZIP (miniz), 7z (7-Zip's C code) and, through
// libarchive, RAR, TAR and its compressed forms, gzip / bzip2 / xz / lzma / zstd / .Z files,
// CAB, LHA, CPIO, ISO, split .001 sets and multi-part RARs
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stddef.h>

enum { AR_NONE, AR_ZIP, AR_7Z, AR_MULTI, AR_SINGLE }; // AR_SINGLE: one compressed file (.gz, .xz, ...)
enum { TF_NONE, TF_GZ, TF_XZ, TF_BZ2, TF_ZST };      // TAR compression
int ar_kind(const char *path); // by the file's contents (and, for split sets, its name)
// new archive at out (which must not exist yet) with each source and, for folders, all inside;
// progress, cancel and errors go through the running job. 0 or -errno / -FS_E*
int ar_zip_create(const char *out, const char *const *srcs, int n);
int ar_7z_create(const char *out, const char *const *srcs, int n);
int ar_tar_create(const char *out, const char *const *srcs, int n, int filter);
// unpacks into the existing folder dest (every file and folder 0777); for AR_SINGLE dest is the
// file to create instead
int ar_extract(const char *archive, const char *dest);
// what an archive unpacks to: "game.part1.rar" -> "game", "notes.txt.gz" -> "notes.txt"
int ar_out_name(const char *archive, char *out, size_t n);
