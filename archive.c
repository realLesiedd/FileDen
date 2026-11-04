// FileDen - archives.
// Everything else (RAR, TAR and its compressed forms, gzip / bzip2 / xz / lzma / zstd / .Z files,
// CAB, LHA, CPIO, ISO, split .001 sets, multi-part RARs, ZIPX) goes through libarchive.
// ZIP: miniz (deflate, zip64 when needed). 7z: written here as one solid LZMA2 stream with
// 7-Zip's C encoder; read with 7-Zip's C header parser and a streaming decoder for the usual
// methods (LZMA, LZMA2, Copy, with BCJ x86 / ARM64 / Delta filters), so archives of any size
// unpack in fixed memory; other methods (PPMd, BCJ2, ...) are decoded in memory up to 512 MB.
// Everything extracted is written 0777 (the console only starts apps whose files are 0777).
// SPDX-License-Identifier: GPL-3.0-or-later
#include "archive.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include "fsops.h"
#include "third_party/miniz/miniz.h"
#include "7z.h"
#include "7zCrc.h"
#include "Alloc.h"
#include "Bra.h"
#include "CpuArch.h"
#include "Delta.h"
#include "Lzma2Dec.h"
#include "Lzma2Enc.h"
#include "LzmaDec.h"
#include <archive.h>
#include "rarcrypt.h"
#include "archive_blake2.h" // libarchive's BLAKE2sp, for RAR5 checksums
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <archive_entry.h>

#define PMAX 1024
#define MEM_DECODE_MAX (512u << 20) // other 7z methods are decoded in memory up to this size

static int la_probe(const char *path);
static int la_create(const char *out, const char *const *srcs, int n, int filter);
#define LA_ZIP_AES (-1)
static int is_split(const char *path);
int ar_kind(const char *path) {
  unsigned char s[6] = {0};
  int fd = open(path, O_RDONLY);
  if (fd < 0) return AR_NONE;
  ssize_t k = read(fd, s, sizeof s);
  close(fd);
  if (is_split(path)) return la_probe(path) ? AR_MULTI : AR_NONE; // .001 sets: libarchive reads the parts as one
  if (k >= 4 && s[0] == 'P' && s[1] == 'K' && ((s[2] == 3 && s[3] == 4) || (s[2] == 5 && s[3] == 6))) return AR_ZIP;
  if (k == 6 && !memcmp(s, k7zSignature, 6)) return AR_7Z;
  return la_probe(path);
}

static const char *base_of(const char *p) {
  const char *s = strrchr(p, '/');
  return s ? s + 1 : p;
}
static void set_mtime(const char *p, long long t) {
  if (t <= 0) return;
  struct timeval tv[2] = {{(time_t)t, 0}, {(time_t)t, 0}};
  utimes(p, tv);
}
static int write_all(int fd, const void *b, size_t n) {
  for (const char *p = b; n;) {
    ssize_t w = write(fd, p, n);
    if (w <= 0) return w < 0 ? -errno : -EIO;
    p += w, n -= w;
  }
  return 0;
}

// ---- what goes into a new archive ----
typedef struct {
  char *path, *name; // on disk, inside the archive ("folder/file.txt")
  int dir;
  long long size, mtime;
} aent_t;
typedef struct {
  aent_t *v;
  int n, cap;
  const char *skip; // the archive being written
} alist_t;
static void alist_free(alist_t *l) {
  for (int i = 0; i < l->n; i++) free(l->v[i].path), free(l->v[i].name);
  free(l->v);
}
static int alist_add(alist_t *l, const char *path, const char *name, int dir, const struct stat *st) {
  if (l->n == l->cap) {
    aent_t *q = realloc(l->v, (l->cap = l->cap ? l->cap * 2 : 256) * sizeof *q);
    if (!q) return -ENOMEM;
    l->v = q;
  }
  aent_t *e = &l->v[l->n];
  e->path = strdup(path), e->name = strdup(name);
  if (!e->path || !e->name) return free(e->path), free(e->name), -ENOMEM;
  e->dir = dir, e->size = dir ? 0 : (long long)st->st_size, e->mtime = (long long)st->st_mtime;
  l->n++;
  return 0;
}
static int collect(alist_t *l, const char *path, const char *name, int depth) {
  struct stat st;
  if (job_cancelled()) return -ECANCELED;
  if (lstat(path, &st)) return -errno;
  if (l->skip && !strcmp(path, l->skip)) return 0;
  if (S_ISREG(st.st_mode)) return alist_add(l, path, name, 0, &st);
  if (!S_ISDIR(st.st_mode) || depth > 64) return 0; // links, devices: left out
  int rc = alist_add(l, path, name, 1, &st);
  DIR *d = rc ? 0 : opendir(path);
  if (!d) return rc ? rc : -errno;
  struct dirent *de;
  char *cp = malloc(PMAX), *cn = malloc(PMAX);
  if (!cp || !cn) rc = -ENOMEM;
  while (!rc && (de = readdir(d))) {
    if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
    if (snprintf(cp, PMAX, "%s/%s", path, de->d_name) >= PMAX || snprintf(cn, PMAX, "%s/%s", name, de->d_name) >= PMAX)
      rc = -ENAMETOOLONG;
    else rc = collect(l, cp, cn, depth + 1);
  }
  free(cp), free(cn);
  closedir(d);
  return rc;
}
static int collect_all(alist_t *l, const char *out, const char *const *srcs, int n) {
  memset(l, 0, sizeof *l);
  l->skip = out;
  for (int i = 0; i < n; i++) {
    int rc = collect(l, srcs[i], base_of(srcs[i]), 0);
    if (rc) return rc;
  }
  return 0;
}

// ---- names inside archives: never outside the target folder ----
static int safe_rel(const char *in, char *out, size_t n) {
  size_t w = 0;
  char t[PMAX];
  snprintf(t, sizeof t, "%s", in);
  for (char *c = t; *c; c++)
    if (*c == '\\') *c = '/';
  char *s = t;
  if (s[0] && s[1] == ':') s += 2; // "C:\..."
  for (char *tok = s; *tok;) { // each part between slashes (no strtok: jobs and requests run at once)
    size_t l = strcspn(tok, "/");
    char *next = tok + l + (tok[l] == '/');
    if (!l || (l == 1 && tok[0] == '.')) {
      tok = next;
      continue;
    }
    if (l == 2 && tok[0] == '.' && tok[1] == '.') return -1;
    if (w + l + 2 >= n) return -1;
    if (w) out[w++] = '/';
    memcpy(out + w, tok, l);
    w += l;
    tok = next;
  }
  out[w] = 0;
  return w ? 0 : -1;
}
static int target_of(const char *dest, const char *rel, char *out) {
  return snprintf(out, PMAX, "%s/%s", dest, rel) >= PMAX ? -ENAMETOOLONG : 0;
}
static int mkparents(const char *p) {
  char d[PMAX];
  snprintf(d, sizeof d, "%s", p);
  char *s = strrchr(d, '/');
  if (!s || s == d) return 0;
  *s = 0;
  return fs_mkdirs(d);
}

// ======================= ZIP =======================
static int stored_ext(const char *n) { // already compressed: store, it's much faster
  static const char *const e[] = {"zip", "7z", "rar", "gz", "xz", "bz2", "zst", "pkg", "jpg", "jpeg", "png", "gif",
                                  "webp", "mp4", "m4v", "mkv", "webm", "mov", "mp3", "m4a", "aac", "ogg", "flac",
                                  "opus", "jxr", "avif", "heic", "ffpkg", 0};
  const char *d = strrchr(n, '.');
  if (!d) return 0;
  for (int i = 0; e[i]; i++)
    if (!strcasecmp(d + 1, e[i])) return 1;
  return 0;
}
typedef struct {
  int fd;
} zrd_t;
static size_t zip_read(void *o, mz_uint64 ofs, void *buf, size_t n) {
  zrd_t *r = o;
  size_t got = 0;
  while (got < n) {
    ssize_t k = pread(r->fd, (char *)buf + got, n - got, (off_t)(ofs + got));
    if (k <= 0) break;
    got += k;
  }
  if (job_tick((long long)got, 0, 0)) return 0; // cancelled: miniz stops
  return got;
}
int ar_zip_create(const char *out, const char *const *srcs, int n) {
  if (job_password()[0]) return la_create(out, srcs, n, LA_ZIP_AES);
  alist_t l;
  int rc = collect_all(&l, out, srcs, n);
  mz_zip_archive z;
  memset(&z, 0, sizeof z);
  if (!rc && !mz_zip_writer_init_file_v2(&z, out, 0, 0)) rc = -EIO;
  for (int i = 0; !rc && i < l.n; i++) {
    aent_t *e = &l.v[i];
    MZ_TIME_T t = (MZ_TIME_T)e->mtime;
    job_tick(0, 0, base_of(e->name));
    if (e->dir) {
      char dn[PMAX + 2];
      snprintf(dn, sizeof dn, "%s/", e->name);
      if (!mz_zip_writer_add_mem_ex_v2(&z, dn, "", 0, 0, 0, 0, 0, 0, &t, 0, 0, 0, 0)) rc = -EIO;
    } else {
      zrd_t r = {open(e->path, O_RDONLY)};
      if (r.fd < 0) rc = -errno;
      else {
        mz_uint lvl = stored_ext(e->name) || !e->size ? MZ_NO_COMPRESSION : 6;
        if (!mz_zip_writer_add_read_buf_callback(&z, e->name, zip_read, &r, (mz_uint64)e->size, &t, 0, 0, lvl, 0, 0, 0, 0))
          rc = job_cancelled() ? -ECANCELED : -EIO;
        close(r.fd);
      }
      if (rc && rc != -ECANCELED) job_fail("Couldn't add", e->path, rc);
    }
    if (!rc && job_tick(0, 1, 0)) rc = -ECANCELED;
  }
  if (!rc && !mz_zip_writer_finalize_archive(&z)) rc = -EIO;
  if (z.m_zip_mode != MZ_ZIP_MODE_INVALID) mz_zip_writer_end(&z);
  if (!rc) chmod(out, FS_MODE);
  alist_free(&l);
  return rc;
}

typedef struct {
  int fd;
} zwr_t;
static size_t zip_write(void *o, mz_uint64 ofs, const void *buf, size_t n) {
  (void)ofs;
  if (write_all(((zwr_t *)o)->fd, buf, n)) return 0;
  if (job_tick((long long)n, 0, 0)) return 0;
  return n;
}
static int zip_extract(const char *arc, const char *dest) {
  mz_zip_archive z;
  memset(&z, 0, sizeof z);
  if (!mz_zip_reader_init_file_v2(&z, arc, 0, 0, 0)) return -FS_EARCHIVE;
  mz_uint n = mz_zip_reader_get_num_files(&z);
  long long total = 0;
  int rc = 0;
  for (mz_uint i = 0; i < n; i++) {
    mz_zip_archive_file_stat s;
    if (!mz_zip_reader_file_stat(&z, i, &s)) {
      rc = -FS_EARCHIVE;
      break;
    }
    if (s.m_is_encrypted || (!s.m_is_directory && !s.m_is_supported)) {
      rc = -FS_EMETHOD;
      break;
    }
    total += (long long)s.m_uncomp_size;
  }
  if (!rc) job_add_total(total, n);
  for (mz_uint i = 0; !rc && i < n; i++) {
    mz_zip_archive_file_stat s;
    char rel[PMAX], full[PMAX];
    mz_zip_reader_file_stat(&z, i, &s);
    if (safe_rel(s.m_filename, rel, sizeof rel) || target_of(dest, rel, full)) { // "../x", "/etc/x": skipped
      job_tick((long long)s.m_uncomp_size, 1, 0);
      continue;
    }
    job_tick(0, 0, base_of(rel));
    if (s.m_is_directory) rc = fs_mkdirs(full);
    else if (!(rc = mkparents(full))) {
      zwr_t w = {fs_create(full, 0)};
      if (w.fd < 0) rc = w.fd;
      else {
        if (!mz_zip_reader_extract_to_callback(&z, i, zip_write, &w, 0))
          rc = job_cancelled() ? -ECANCELED : mz_zip_get_last_error(&z) == MZ_ZIP_UNSUPPORTED_METHOD ? -FS_EMETHOD : -EIO;
        close(w.fd);
      }
    }
    if (!rc) set_mtime(full, (long long)s.m_time);
    else if (rc != -ECANCELED) job_fail("Couldn't extract", rel, rc);
    if (!rc && job_tick(0, 1, 0)) rc = -ECANCELED;
  }
  mz_zip_reader_end(&z);
  return rc;
}

// ======================= 7z: writing =======================
#define FILETIME_UNIX 11644473600LL
typedef struct {
  Byte *p;
  size_t n, cap;
  int bad;
} bb_t;
static void bb_put(bb_t *b, const void *d, size_t n) {
  if (b->bad) return;
  if (b->n + n > b->cap) {
    size_t c = b->cap ? b->cap * 2 : 4096;
    while (c < b->n + n) c *= 2;
    Byte *q = realloc(b->p, c);
    if (!q) {
      b->bad = 1;
      return;
    }
    b->p = q, b->cap = c;
  }
  memcpy(b->p + b->n, d, n);
  b->n += n;
}
static void bb_byte(bb_t *b, unsigned v) {
  Byte c = (Byte)v;
  bb_put(b, &c, 1);
}
static void bb_u32(bb_t *b, UInt32 v) {
  Byte t[4];
  SetUi32(t, v);
  bb_put(b, t, 4);
}
static void bb_u64(bb_t *b, UInt64 v) {
  Byte t[8];
  SetUi64(t, v);
  bb_put(b, t, 8);
}
static void bb_num(bb_t *b, UInt64 v) { // 7z variable-length number
  Byte first = 0, mask = 0x80;
  int i;
  for (i = 0; i < 8; i++) {
    if (v < ((UInt64)1 << (7 * (i + 1)))) {
      first |= (Byte)(v >> (8 * i));
      break;
    }
    first |= mask;
    mask >>= 1;
  }
  bb_byte(b, first);
  for (; i > 0; i--) {
    bb_byte(b, (Byte)v);
    v >>= 8;
  }
}
static void bb_bits(bb_t *b, const Byte *bits, int n) { // MSB-first bit vector
  for (int i = 0; i < n; i += 8) {
    Byte c = 0;
    for (int k = 0; k < 8 && i + k < n; k++)
      if (bits[i + k]) c |= (Byte)(0x80 >> k);
    bb_byte(b, c);
  }
}
static void bb_utf16(bb_t *b, const char *s) { // UTF-8 name -> UTF-16LE with a terminator
  const unsigned char *p = (const unsigned char *)s;
  while (*p) {
    unsigned c = *p++, cp = c;
    int more = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;
    if (more) {
      cp = c & (0x3f >> more);
      for (int i = 0; i < more; i++) {
        if ((*p & 0xc0) != 0x80) {
          cp = c; // not UTF-8: keep the byte as Latin-1
          more = 0;
          break;
        }
        cp = (cp << 6) | (*p++ & 0x3f);
      }
    }
    if (cp >= 0x10000) {
      cp -= 0x10000;
      bb_byte(b, (0xd800 | (cp >> 10)) & 255), bb_byte(b, (0xd800 | (cp >> 10)) >> 8);
      bb_byte(b, (0xdc00 | (cp & 0x3ff)) & 255), bb_byte(b, (0xdc00 | (cp & 0x3ff)) >> 8);
    } else bb_byte(b, cp & 255), bb_byte(b, cp >> 8);
  }
  bb_byte(b, 0), bb_byte(b, 0);
}

// ======================= 7zAES: AES-256-CBC, key = SHA-256 over 2^cycles rounds =======================
#define AES_CYCLES 19 // 7-Zip's own setting
static int aes_seen; // this extract met encrypted data (a failure then means a wrong password)
static struct {
  int ok, cycles;
  Byte salt[16], key[32];
  unsigned saltn;
  char pw[256];
} kc; // the last key worked out: headers and every folder share one
static int aes_key(const Byte *salt, unsigned saltn, int cycles, Byte key[32]) {
  const char *pw = job_password();
  if (kc.ok && kc.cycles == cycles && kc.saltn == saltn && !memcmp(kc.salt, salt, saltn) && !strcmp(kc.pw, pw)) {
    memcpy(key, kc.key, 32);
    return 0;
  }
  bb_t u = {0};
  bb_utf16(&u, pw);
  size_t un = u.n >= 2 ? u.n - 2 : 0; // no terminator
  int rc = u.bad ? -ENOMEM : 0;
  if (!rc && cycles == 0x3f) { // no hashing: the salt and password, padded
    memset(key, 0, 32);
    size_t k = 0;
    for (unsigned i = 0; i < saltn && k < 32; i++) key[k++] = salt[i];
    for (size_t i = 0; i < un && k < 32; i++) key[k++] = u.p[i];
  } else if (!rc) {
    EVP_MD_CTX *c = EVP_MD_CTX_new();
    rc = c && EVP_DigestInit_ex(c, EVP_sha256(), 0) ? 0 : -ENOMEM;
    Byte ctr[8] = {0};
    for (UInt64 i = 0; !rc && i < ((UInt64)1 << cycles); i++) {
      EVP_DigestUpdate(c, salt, saltn);
      EVP_DigestUpdate(c, u.p, un);
      EVP_DigestUpdate(c, ctr, 8);
      for (int b = 0; b < 8 && !++ctr[b]; b++) // little-endian counter
        ;
    }
    unsigned kl = 32;
    if (!rc && !EVP_DigestFinal_ex(c, key, &kl)) rc = -EIO;
    EVP_MD_CTX_free(c);
  }
  free(u.p);
  if (!rc) {
    kc.ok = 1, kc.cycles = cycles, kc.saltn = saltn;
    memcpy(kc.salt, salt, saltn), memcpy(kc.key, key, 32);
    snprintf(kc.pw, sizeof kc.pw, "%s", pw);
  }
  return rc;
}
typedef struct {
  EVP_CIPHER_CTX *c;
} aes_t;
static int aes_begin(aes_t *a, const Byte key[32], const Byte iv[16], int enc) {
  a->c = EVP_CIPHER_CTX_new();
  if (!a->c || !EVP_CipherInit_ex(a->c, EVP_aes_256_cbc(), 0, key, iv, enc)) return -ENOMEM;
  EVP_CIPHER_CTX_set_padding(a->c, 0);
  return 0;
}
static int aes_run(aes_t *a, Byte *d, size_t n) { // in place, n a multiple of 16
  for (size_t off = 0; off < n;) {
    int k = n - off > (1 << 20) ? (1 << 20) : (int)(n - off), o = 0;
    if (!EVP_CipherUpdate(a->c, d + off, &o, d + off, k) || o != k) return -EIO;
    off += (size_t)k;
  }
  return 0;
}
static void aes_end(aes_t *a) {
  EVP_CIPHER_CTX_free(a->c);
  a->c = 0;
}
// the coder's properties: cycles, salt, IV (7-Zip's layout)
static int aes_props(const Byte *p, unsigned n, int *cycles, const Byte **salt, unsigned *saltn, Byte iv[16]) {
  if (n < 1) return -1;
  *cycles = p[0] & 0x3f, *saltn = 0, *salt = p + 2;
  memset(iv, 0, 16);
  if (!(p[0] & 0xc0)) return n == 1 ? 0 : -1;
  if (n < 2) return -1;
  unsigned sn = ((p[0] >> 7) & 1) + (p[1] >> 4), ivn = ((p[0] >> 6) & 1) + (p[1] & 15);
  if (2 + sn + ivn != n || ivn > 16 || sn > 16 || (*cycles > 24 && *cycles != 0x3f)) return -1;
  *saltn = sn;
  memcpy(iv, p + 2 + sn, ivn);
  return 0;
}
// the hook in 7zDec.c (encrypted headers, and folders decoded in memory)
int FileDen_StripAes(const CSzFolder *f, CSzFolder *g, unsigned *ai);
extern SRes (*Sz7zAesDecrypt)(const Byte *props, unsigned propsSize, Byte *data, size_t size);
static int aes_fail(SRes res) { // after 7zAES: no password, or the wrong one
  (void)res; // the data or header didn't decode after decrypting: the password is wrong
  return job_password()[0] ? -FS_EBADPW : -FS_EPASSWORD;
}
static SRes aes7z_decrypt(const Byte *props, unsigned n, Byte *data, size_t size) {
  aes_seen = 1;
  if (!job_password()[0]) return SZ_ERROR_PARAM; // FileDen asks for the password
  int cycles;
  const Byte *salt;
  unsigned sn;
  Byte iv[16], key[32];
  if (aes_props(props, n, &cycles, &salt, &sn, iv)) return SZ_ERROR_UNSUPPORTED;
  if (aes_key(salt, sn, cycles, key)) return SZ_ERROR_MEM;
  aes_t a;
  int rc = aes_begin(&a, key, iv, 0);
  if (!rc) rc = aes_run(&a, data, size);
  aes_end(&a);
  return rc ? SZ_ERROR_MEM : SZ_OK;
}

typedef struct {
  ISeqInStream vt; // first: the encoder passes &vt back to us
  aent_t *v;
  int n, i, fd, err;
  long long left;
  UInt32 crc, *crcs;
} cat_t;
static SRes cat_read(ISeqInStreamPtr pp, void *buf, size_t *size) { // all files, one after another
  cat_t *c = (cat_t *)(void *)pp;
  size_t want = *size;
  *size = 0;
  while (c->fd < 0) {
    while (c->i < c->n && (c->v[c->i].dir || !c->v[c->i].size)) c->i++;
    if (c->i >= c->n) return SZ_OK; // end of all data
    c->fd = open(c->v[c->i].path, O_RDONLY);
    if (c->fd < 0) {
      c->err = -errno;
      job_fail("Couldn't read", c->v[c->i].path, c->err);
      return SZ_ERROR_READ;
    }
    c->left = c->v[c->i].size, c->crc = CRC_INIT_VAL;
    job_tick(0, 0, base_of(c->v[c->i].name));
  }
  if ((long long)want > c->left) want = (size_t)c->left;
  ssize_t k = read(c->fd, buf, want);
  if (k <= 0) { // the file got shorter while we read it
    c->err = k < 0 ? -errno : -EIO;
    job_fail("Couldn't read", c->v[c->i].path, c->err);
    return SZ_ERROR_READ;
  }
  c->crc = CrcUpdate(c->crc, buf, (size_t)k);
  c->left -= k;
  *size = (size_t)k;
  if (!c->left) {
    c->crcs[c->i] = CRC_GET_DIGEST(c->crc);
    close(c->fd);
    c->fd = -1;
    c->i++;
  }
  if (job_tick(k, 0, 0)) {
    c->err = -ECANCELED;
    return SZ_ERROR_READ;
  }
  return SZ_OK;
}
typedef struct {
  ISeqOutStream vt;
  int fd, err;
  UInt64 written, plain; // bytes in the file; bytes before encryption
  aes_t *aes;            // with a password: AES-256-CBC on the way out
  Byte *tmp;
} out_t;
static size_t out_write(ISeqOutStreamPtr pp, const void *buf, size_t size) {
  out_t *o = (out_t *)(void *)pp;
  if (o->err) return 0;
  o->plain += size;
  if (!o->aes) {
    if ((o->err = write_all(o->fd, buf, size))) return 0;
    o->written += size;
    return size;
  }
  for (size_t off = 0; off < size;) { // the cipher keeps a partial block back until it's whole
    int k = size - off > (1 << 16) ? (1 << 16) : (int)(size - off), w = 0;
    if (!EVP_CipherUpdate(o->aes->c, o->tmp, &w, (const Byte *)buf + off, k)) return o->err = -EIO, 0;
    if (w && (o->err = write_all(o->fd, o->tmp, (size_t)w))) return 0;
    o->written += (UInt64)w, off += (size_t)k;
  }
  return size;
}
static int out_finish(out_t *o) { // pad the last block with zeros (the reader knows the real size)
  if (!o->aes || o->err) return o->err;
  Byte z[16] = {0};
  unsigned pad = (unsigned)((16 - o->plain % 16) % 16);
  UInt64 keep = o->plain;
  if (pad && !out_write(&o->vt, z, pad)) return o->err ? o->err : -EIO;
  o->plain = keep;
  return 0;
}
static void aes_coder(bb_t *h, const Byte props[18]) { // 7zAES with its properties
  static const Byte id[4] = {0x06, 0xf1, 0x07, 0x01};
  bb_byte(h, 0x24), bb_put(h, id, 4), bb_num(h, 18), bb_put(h, props, 18);
}
static int aes_new(Byte props[18], Byte key[32]) { // a fresh IV; no salt, like 7-Zip
  props[0] = AES_CYCLES | 0x40, props[1] = 15;
  if (RAND_bytes(props + 2, 16) != 1) return -EIO;
  return aes_key(props, 0, AES_CYCLES, key);
}

int ar_7z_create(const char *out, const char *const *srcs, int nsrc) {
  alist_t l;
  int rc = collect_all(&l, out, srcs, nsrc);
  int fd = rc ? -1 : fs_create(out, 1);
  if (!rc && fd < 0) rc = fd;
  CrcGenerateTable();
  Byte zero[k7zStartHeaderSize] = {0};
  if (!rc) rc = write_all(fd, zero, sizeof zero);
  UInt32 *crcs = rc ? 0 : calloc(l.n ? l.n : 1, sizeof *crcs);
  if (!rc && !crcs) rc = -ENOMEM;
  UInt64 total = 0, packed = 0, lzsize = 0;
  int nonempty = 0, enc_pw = job_password()[0] != 0;
  Byte aprops[18];
  for (int i = 0; i < l.n; i++)
    if (!l.v[i].dir && l.v[i].size) total += (UInt64)l.v[i].size, nonempty++;
  Byte prop = 0;
  if (!rc && nonempty) { // one solid LZMA2 stream with every file's data
    CLzma2EncHandle enc = Lzma2Enc_Create(&g_Alloc, &g_BigAlloc);
    if (!enc) rc = -ENOMEM;
    else {
      CLzma2EncProps pr;
      Lzma2EncProps_Init(&pr);
      pr.lzmaProps.level = 5;
      pr.lzmaProps.dictSize = 1 << 23; // 8 MB: about 100 MB of encoder memory
      pr.lzmaProps.reduceSize = total;
      pr.lzmaProps.numThreads = 1;
      pr.numTotalThreads = 1;
      pr.numBlockThreads_Max = 1;
      pr.blockSize = LZMA2_ENC_PROPS_BLOCK_SIZE_SOLID;
      Lzma2EncProps_Normalize(&pr);
      cat_t in = {{cat_read}, l.v, l.n, 0, -1, 0, 0, 0, crcs};
      out_t o = {{out_write}, fd, 0, 0, 0, 0, 0};
      aes_t aes = {0};
      if (enc_pw) { // a password: the LZMA2 stream goes through AES-256 too
        Byte key[32];
        o.tmp = malloc((1 << 16) + 32);
        if (!o.tmp || aes_new(aprops, key) || aes_begin(&aes, key, aprops + 2, 1)) o.err = -ENOMEM;
        o.aes = &aes;
      }
      SRes r = o.err ? SZ_ERROR_MEM : Lzma2Enc_SetProps(enc, &pr);
      if (r == SZ_OK) {
        Lzma2Enc_SetDataSize(enc, total);
        prop = Lzma2Enc_WriteProperties(enc);
        r = Lzma2Enc_Encode2(enc, &o.vt, 0, 0, &in.vt, 0, 0, 0);
      }
      if (r == SZ_OK && out_finish(&o)) r = SZ_ERROR_WRITE;
      if (in.fd >= 0) close(in.fd);
      Lzma2Enc_Destroy(enc);
      if (o.aes) aes_end(&aes);
      free(o.tmp);
      lzsize = o.plain;
      if (r != SZ_OK) rc = in.err ? in.err : o.err ? o.err : r == SZ_ERROR_MEM ? -ENOMEM : -EIO;
      else if (in.i < l.n) { // a file ended early
        for (; in.i < l.n && (l.v[in.i].dir || !l.v[in.i].size); in.i++)
          ;
        if (in.i < l.n) rc = -EIO;
      }
      packed = o.written;
    }
  }
  // the header: pack info, the one folder (LZMA2), substreams, then the file list
  bb_t h = {0};
  if (!rc) {
    int n = l.n, nempty = 0;
    bb_byte(&h, 0x01); // kHeader
    if (nonempty) {
      bb_byte(&h, 0x04);                                                  // kMainStreamsInfo
      bb_byte(&h, 0x06), bb_num(&h, 0), bb_num(&h, 1);                    // kPackInfo: at 0, 1 stream
      bb_byte(&h, 0x09), bb_num(&h, packed), bb_byte(&h, 0x00);           // kSize, kEnd
      bb_byte(&h, 0x07), bb_byte(&h, 0x0b), bb_num(&h, 1), bb_byte(&h, 0); // kUnPackInfo, kFolder, 1, not external
      bb_num(&h, enc_pw ? 2 : 1);                                         // coders: LZMA2 (then 7zAES)
      bb_byte(&h, 0x21), bb_byte(&h, 0x21);                               // id size 1 + has props, id LZMA2
      bb_num(&h, 1), bb_byte(&h, prop);                                   // props
      if (enc_pw) aes_coder(&h, aprops), bb_num(&h, 0), bb_num(&h, 1);    // its input is 7zAES's output
      bb_byte(&h, 0x0c), bb_num(&h, total);                               // kCodersUnPackSize
      if (enc_pw) bb_num(&h, lzsize);
      bb_byte(&h, 0x00);                                                  // kEnd
      bb_byte(&h, 0x08);                                                  // kSubStreamsInfo
      if (nonempty != 1) bb_byte(&h, 0x0d), bb_num(&h, (UInt64)nonempty); // kNumUnPackStream
      if (nonempty > 1) {
        bb_byte(&h, 0x09); // kSize: every substream but the last
        int seen = 0;
        for (int i = 0; i < n; i++)
          if (!l.v[i].dir && l.v[i].size && ++seen < nonempty) bb_num(&h, (UInt64)l.v[i].size);
      }
      bb_byte(&h, 0x0a), bb_byte(&h, 1); // kCRC, all defined
      for (int i = 0; i < n; i++)
        if (!l.v[i].dir && l.v[i].size) bb_u32(&h, crcs[i]);
      bb_byte(&h, 0x00), bb_byte(&h, 0x00); // kEnd (substreams), kEnd (streams)
    }
    if (n) {
      bb_byte(&h, 0x05), bb_num(&h, (UInt64)n); // kFilesInfo
      Byte *es = calloc(n, 1), *ef = calloc(n, 1);
      if (!es || !ef) rc = -ENOMEM;
      for (int i = 0; !rc && i < n; i++)
        if (l.v[i].dir || !l.v[i].size) es[i] = 1, ef[nempty++] = !l.v[i].dir;
      if (!rc && nempty) {
        bb_byte(&h, 0x0e), bb_num(&h, (UInt64)(n + 7) / 8), bb_bits(&h, es, n); // kEmptyStream
        int anyfile = 0;
        for (int i = 0; i < nempty; i++) anyfile |= ef[i];
        if (anyfile) bb_byte(&h, 0x0f), bb_num(&h, (UInt64)(nempty + 7) / 8), bb_bits(&h, ef, nempty); // kEmptyFile
      }
      free(es), free(ef);
      bb_t names = {0};
      for (int i = 0; i < n; i++) bb_utf16(&names, l.v[i].name);
      bb_byte(&h, 0x11), bb_num(&h, names.n + 1), bb_byte(&h, 0), bb_put(&h, names.p, names.n); // kName
      if (names.bad) rc = -ENOMEM;
      free(names.p);
      bb_byte(&h, 0x14), bb_num(&h, 8 * (UInt64)n + 2), bb_byte(&h, 1), bb_byte(&h, 0); // kMTime, all defined
      for (int i = 0; i < n; i++) bb_u64(&h, (UInt64)(l.v[i].mtime + FILETIME_UNIX) * 10000000ULL);
      bb_byte(&h, 0x15), bb_num(&h, 4 * (UInt64)n + 2), bb_byte(&h, 1), bb_byte(&h, 0); // kWinAttributes
      for (int i = 0; i < n; i++) // Windows bits + Unix mode 0777 (0x8000: high half is the Unix mode)
        bb_u32(&h, (l.v[i].dir ? 0x10u : 0x20u) | 0x8000u | ((UInt32)(l.v[i].dir ? 040777 : 0100777) << 16));
      bb_byte(&h, 0x00); // kEnd (files)
    }
    bb_byte(&h, 0x00); // kEnd (header)
    if (h.bad) rc = -ENOMEM;
  }
  UInt64 hoff = packed; // where the header starts, after the data
  if (!rc && enc_pw) { // the header (the file names) is encrypted too: an encoded header points at it
    Byte hp[18], key[32];
    size_t pn = (h.n + 15) & ~(size_t)15;
    Byte *e = calloc(pn ? pn : 16, 1);
    aes_t a = {0};
    rc = e ? 0 : -ENOMEM;
    if (!rc) memcpy(e, h.p, h.n), rc = aes_new(hp, key);
    if (!rc) rc = aes_begin(&a, key, hp + 2, 1);
    if (!rc) rc = aes_run(&a, e, pn);
    aes_end(&a);
    if (!rc) rc = write_all(fd, e, pn);
    free(e);
    bb_t eh = {0};
    bb_byte(&eh, 0x17);                                                  // kEncodedHeader
    bb_byte(&eh, 0x06), bb_num(&eh, packed), bb_num(&eh, 1);             // kPackInfo: after the data, 1 stream
    bb_byte(&eh, 0x09), bb_num(&eh, pn), bb_byte(&eh, 0x00);
    bb_byte(&eh, 0x07), bb_byte(&eh, 0x0b), bb_num(&eh, 1), bb_byte(&eh, 0); // one folder: 7zAES
    bb_num(&eh, 1), aes_coder(&eh, hp);
    bb_byte(&eh, 0x0c), bb_num(&eh, h.n);                                // kCodersUnPackSize
    bb_byte(&eh, 0x0a), bb_byte(&eh, 1), bb_u32(&eh, CrcCalc(h.p, h.n)); // kCRC
    bb_byte(&eh, 0x00), bb_byte(&eh, 0x00);                              // kEnd (unpack info), kEnd (streams)
    hoff = packed + pn;
    free(h.p);
    h = eh;
    if (h.bad) rc = -ENOMEM;
  }
  if (!rc) rc = write_all(fd, h.p, h.n);
  if (!rc) { // start header: where the header is, its size and CRC
    Byte sh[k7zStartHeaderSize];
    memcpy(sh, k7zSignature, k7zSignatureSize);
    sh[6] = 0, sh[7] = 4;
    SetUi64(sh + 12, hoff);
    SetUi64(sh + 20, (UInt64)h.n);
    SetUi32(sh + 28, CrcCalc(h.p, h.n));
    SetUi32(sh + 8, CrcCalc(sh + 12, 20));
    if (pwrite(fd, sh, sizeof sh, 0) != (ssize_t)sizeof sh) rc = -errno ? -errno : -EIO;
  }
  for (int i = 0; !rc && i < l.n; i++) // folders and empty files count as done here
    if (l.v[i].dir || !l.v[i].size) job_tick(0, 1, 0);
  if (!rc) job_tick(0, nonempty, 0);
  if (fd >= 0 && close(fd) && !rc) rc = -errno;
  free(h.p);
  free(crcs);
  alist_free(&l);
  return rc;
}

// ======================= 7z: reading =======================
typedef struct {
  ISeekInStream vt;
  int fd;
} fdin_t;
static SRes fdin_read(ISeekInStreamPtr pp, void *buf, size_t *size) {
  fdin_t *f = (fdin_t *)(void *)pp;
  ssize_t k = read(f->fd, buf, *size);
  if (k < 0) {
    *size = 0;
    return SZ_ERROR_READ;
  }
  *size = (size_t)k;
  return SZ_OK;
}
static SRes fdin_seek(ISeekInStreamPtr pp, Int64 *pos, ESzSeek origin) {
  fdin_t *f = (fdin_t *)(void *)pp;
  off_t r = lseek(f->fd, (off_t)*pos, origin == SZ_SEEK_SET ? SEEK_SET : origin == SZ_SEEK_CUR ? SEEK_CUR : SEEK_END);
  if (r < 0) return SZ_ERROR_READ;
  *pos = (Int64)r;
  return SZ_OK;
}

static int name7z(const CSzArEx *db, UInt32 i, char *out, size_t n) { // UTF-16 -> UTF-8
  size_t len = SzArEx_GetFileNameUtf16(db, i, 0);
  UInt16 *u = malloc(len * sizeof *u);
  if (!u) return -1;
  SzArEx_GetFileNameUtf16(db, i, u);
  size_t w = 0;
  for (size_t k = 0; k < len && u[k]; k++) {
    unsigned c = u[k];
    if (c >= 0xd800 && c < 0xdc00 && k + 1 < len && u[k + 1] >= 0xdc00 && u[k + 1] < 0xe000)
      c = 0x10000 + ((c - 0xd800) << 10) + (u[++k] - 0xdc00);
    if (w + 5 >= n) break;
    if (c < 0x80) out[w++] = (char)c;
    else if (c < 0x800) out[w++] = (char)(0xc0 | c >> 6), out[w++] = (char)(0x80 | (c & 63));
    else if (c < 0x10000)
      out[w++] = (char)(0xe0 | c >> 12), out[w++] = (char)(0x80 | ((c >> 6) & 63)), out[w++] = (char)(0x80 | (c & 63));
    else
      out[w++] = (char)(0xf0 | c >> 18), out[w++] = (char)(0x80 | ((c >> 12) & 63)), out[w++] = (char)(0x80 | ((c >> 6) & 63)),
      out[w++] = (char)(0x80 | (c & 63));
  }
  out[w] = 0;
  free(u);
  return 0;
}
static long long mtime7z(const CSzArEx *db, UInt32 i) {
  if (!SzBitWithVals_Check(&db->MTime, i)) return 0;
  const CNtfsFileTime *t = &db->MTime.Vals[i];
  UInt64 ft = ((UInt64)t->High << 32) | t->Low;
  return (long long)(ft / 10000000ULL) - FILETIME_UNIX;
}

// sends a folder's unpacked bytes into its files, one after another
typedef struct {
  const CSzArEx *db;
  const char *dest;
  UInt32 first, last, folder, cur; // file indexes of this folder
  int fd;
  UInt64 left;
  UInt32 crc;
  char path[PMAX];
} route_t;
static int route_open(route_t *r) { // the next file of the folder (skipping folders and empty ones)
  while (r->cur < r->last) {
    UInt32 i = r->cur;
    if (SzArEx_IsDir(r->db, i) || r->db->FileToFolder[i] != r->folder) {
      r->cur++;
      continue;
    }
    char nm[PMAX], rel[PMAX];
    r->left = SzArEx_GetFileSize(r->db, i);
    r->crc = CRC_INIT_VAL;
    if (name7z(r->db, i, nm, sizeof nm) || safe_rel(nm, rel, sizeof rel) || target_of(r->dest, rel, r->path)) {
      r->path[0] = 0; // unsafe name: its bytes are read and dropped
      r->fd = -2;
    } else {
      int rc = mkparents(r->path);
      if (rc) return rc;
      r->fd = fs_create(r->path, 0);
      if (r->fd < 0) return r->fd;
      job_tick(0, 0, base_of(rel));
    }
    return 1;
  }
  return 0; // no more files
}
static int route_close(route_t *r) {
  UInt32 i = r->cur;
  int rc = 0;
  if (r->fd >= 0) {
    if (close(r->fd)) rc = -errno;
    if (!rc && SzBitWithVals_Check(&r->db->CRCs, i) && r->db->CRCs.Vals[i] != CRC_GET_DIGEST(r->crc)) {
      job_fail("Damaged archive (CRC) at", r->path, -EIO);
      rc = -EIO;
    }
    if (!rc) set_mtime(r->path, mtime7z(r->db, i));
  }
  r->fd = -1;
  r->cur++;
  job_tick(0, 1, 0);
  return rc;
}
static int route_write(route_t *r, const Byte *d, size_t n) {
  while (n) {
    if (r->fd == -1) {
      int k = route_open(r);
      if (k < 0) return k;
      if (!k) return -EIO; // more data than files
      if (!r->left) {
        int rc = route_close(r);
        if (rc) return rc;
        continue;
      }
    }
    size_t k = n < r->left ? n : (size_t)r->left;
    if (r->fd >= 0) {
      int rc = write_all(r->fd, d, k);
      if (rc) {
        job_fail("Couldn't write", r->path, rc);
        return rc;
      }
      r->crc = CrcUpdate(r->crc, d, k);
    }
    d += k, n -= k, r->left -= k;
    if (job_tick((long long)k, 0, 0)) return -ECANCELED;
    if (!r->left) {
      int rc = route_close(r);
      if (rc) return rc;
    }
  }
  return 0;
}
static int route_finish(route_t *r) { // empty files at the end of the folder
  for (;;) {
    if (r->fd == -1) {
      int k = route_open(r);
      if (k <= 0) return k;
    }
    if (r->left) return -EIO; // data missing
    int rc = route_close(r);
    if (rc) return rc;
  }
}

#define K_COPY 0
#define K_LZMA2 0x21
#define K_LZMA 0x30101
#define K_BCJ 0x3030103
#define K_ARM64 0xa
#define K_DELTA 3
#define K_AES 0x6f10701
// LZMA / LZMA2 / Copy, optionally with one BCJ x86 / ARM64 / Delta filter after it: decoded
// as a stream. Returns 1 when the folder isn't one of those (the caller decodes it in memory).
static int decode_stream(CLookToRead2 *look, const CSzArEx *db, UInt32 f, route_t *r) {
  const CSzAr *a = &db->db;
  const Byte *cd = a->CodersData + a->FoCodersOffsets[f];
  CSzData sd = {cd, a->FoCodersOffsets[f + 1] - a->FoCodersOffsets[f]};
  CSzFolder fo;
  if (SzGetNextFolderItem(&fo, &sd) != SZ_OK) return -FS_EARCHIVE;
  // with a password the folder has a 7zAES coder; without it, it's an ordinary folder
  CSzFolder g;
  CSzCoderInfo fo_aes = {0};
  unsigned ai = 0;
  int enc = 0;
  for (UInt32 i = 0; i < fo.NumCoders; i++) enc |= fo.Coders[i].MethodID == K_AES;
  if (enc) {
    if (!FileDen_StripAes(&fo, &g, &ai) || !g.NumCoders) return 1;
    fo_aes = fo.Coders[ai], fo = g;
  }
  UInt32 m = fo.Coders[0].MethodID, flt = 0;
  if (m != K_COPY && m != K_LZMA && m != K_LZMA2) return 1;
  if (fo.NumPackStreams != 1 || fo.PackStreams[0] != 0 || fo.Coders[0].NumStreams != 1) return 1;
  if (fo.NumCoders == 2) {
    const CSzCoderInfo *c = &fo.Coders[1];
    if (c->NumStreams != 1 || fo.NumBonds != 1 || fo.Bonds[0].InIndex != 1 || fo.Bonds[0].OutIndex != 0) return 1;
    flt = c->MethodID;
    if (!((flt == K_BCJ || flt == K_ARM64) && c->PropsSize == 0) && !(flt == K_DELTA && c->PropsSize == 1)) return 1;
  } else if (fo.NumCoders != 1 || fo.NumBonds) return 1;
  // coder outputs in the archive's numbering (the AES coder still counts there)
  UInt32 cu = a->FoToCoderUnpackSizes[f], c0 = enc && ai == 0 ? 1 : 0;
  UInt64 size = a->CoderUnpackSizes[cu + c0]; // main coder output
  UInt32 ps = a->FoStartPackStreamIndex[f];
  UInt64 pack = a->PackPositions[ps + 1] - a->PackPositions[ps];
  if (LookInStream_SeekTo(&look->vt, db->dataPos + a->PackPositions[ps]) != SZ_OK) return -EIO;
  aes_t aes = {0};
  enum { IB = 1 << 18 };
  Byte *ib = 0;          // decrypted input, with a password
  size_t ib_at = 0, ib_n = 0;
  UInt64 plain = 0;      // what's left of the real (unpadded) encrypted stream
  if (enc) {
    const CSzCoderInfo *ac = &fo_aes;
    const Byte *ap = cd + ac->PropsOffset, *salt;
    unsigned saltn;
    int cycles;
    Byte iv[16], key[32];
    aes_seen = 1;
    if (!job_password()[0]) return -FS_EPASSWORD;
    if (pack & 15) return -FS_EARCHIVE;
    if (aes_props(ap, ac->PropsSize, &cycles, &salt, &saltn, iv) || aes_key(salt, saltn, cycles, key)) return -FS_EMETHOD;
    if (!(ib = malloc(IB))) return -ENOMEM;
    if (aes_begin(&aes, key, iv, 0)) return free(ib), -ENOMEM;
    plain = a->CoderUnpackSizes[cu + ai];
  }
  const Byte *props = cd + fo.Coders[0].PropsOffset;
  CLzmaDec d1;
  CLzma2Dec d2;
  LzmaDec_CONSTRUCT(&d1)
  Lzma2Dec_CONSTRUCT(&d2)
  if (m == K_LZMA && LzmaDec_Allocate(&d1, props, fo.Coders[0].PropsSize, &g_Alloc) != SZ_OK) return -ENOMEM;
  if (m == K_LZMA2 && (fo.Coders[0].PropsSize != 1 || Lzma2Dec_Allocate(&d2, props[0], &g_Alloc) != SZ_OK)) return -ENOMEM;
  if (m == K_LZMA) LzmaDec_Init(&d1);
  if (m == K_LZMA2) Lzma2Dec_Init(&d2);
  enum { OUT = 1 << 20 };
  Byte *ob = malloc(OUT + 16), dstate[DELTA_STATE_SIZE];
  UInt32 xstate = Z7_BRANCH_CONV_ST_X86_STATE_INIT_VAL, pc = 0;
  unsigned delta = flt == K_DELTA ? (unsigned)cd[fo.Coders[1].PropsOffset] + 1 : 0;
  Delta_Init(dstate);
  UInt64 made = 0;
  size_t keep = 0;
  int rc = ob ? 0 : -ENOMEM;
  while (!rc && made < size) {
    const void *in = 0;
    size_t look_n = 1 << 18;
    if (enc) { // top up the decrypted buffer, whole blocks at a time
      if (ib_at == ib_n) ib_at = ib_n = 0;
      if (ib_n < IB / 2 && pack) {
        if (ib_at) memmove(ib, ib + ib_at, ib_n - ib_at), ib_n -= ib_at, ib_at = 0;
        size_t k = (IB - ib_n) & ~(size_t)15;
        if (k > pack) k = (size_t)pack;
        if (LookInStream_Read(&look->vt, ib + ib_n, k) != SZ_OK || aes_run(&aes, ib + ib_n, k)) {
          rc = -EIO;
          break;
        }
        pack -= k;
        UInt64 real = plain < k ? plain : k; // the zero padding at the end isn't data
        plain -= real, ib_n += (size_t)real;
      }
      in = ib + ib_at, look_n = ib_n - ib_at;
    } else {
      if (look_n > pack) look_n = (size_t)pack;
      if (ILookInStream_Look(&look->vt, &in, &look_n) != SZ_OK) {
        rc = -EIO;
        break;
      }
    }
    SizeT inp = look_n, outn = OUT - keep;
    if ((UInt64)outn > size - made) outn = (SizeT)(size - made);
    ELzmaStatus st = LZMA_STATUS_NOT_SPECIFIED;
    SRes res = SZ_OK;
    if (m == K_COPY) {
      if (inp > outn) inp = outn;
      memcpy(ob + keep, in, inp);
      outn = inp;
    } else if (m == K_LZMA) res = LzmaDec_DecodeToBuf(&d1, ob + keep, &outn, in, &inp, LZMA_FINISH_ANY, &st);
    else res = Lzma2Dec_DecodeToBuf(&d2, ob + keep, &outn, in, &inp, LZMA_FINISH_ANY, &st);
    if (enc) ib_at += inp;
    else ILookInStream_Skip(&look->vt, inp), pack -= inp;
    if (res != SZ_OK) {
      rc = res == SZ_ERROR_MEM ? -ENOMEM : enc ? -FS_EBADPW : -EIO; // garbage after AES: the wrong password
      if (!enc) job_fail("Damaged archive at", r->path[0] ? r->path : "start", -EIO);
      break;
    }
    if (!inp && !outn) { // no progress: the data ended early
      rc = -EIO;
      break;
    }
    made += outn;
    size_t all = keep + outn, done = all;
    int last = made >= size;
    if (flt == K_BCJ) done = (size_t)(z7_BranchConvSt_X86_Dec(ob, all, pc, &xstate) - ob);
    else if (flt == K_ARM64) done = (size_t)(z7_BranchConv_ARM64_Dec(ob, all, pc) - ob);
    else if (flt == K_DELTA) Delta_Decode(dstate, delta, ob, all);
    if (last) done = all; // the tail is too short to hold an instruction
    rc = route_write(r, ob, done);
    pc += (UInt32)done;
    keep = all - done;
    memmove(ob, ob + done, keep);
  }
  free(ob);
  free(ib);
  if (enc) aes_end(&aes);
  LzmaDec_Free(&d1, &g_Alloc);
  Lzma2Dec_Free(&d2, &g_Alloc);
  return rc;
}
static int decode_memory(CLookToRead2 *look, const CSzArEx *db, UInt32 f, route_t *r) {
  UInt64 size = SzAr_GetFolderUnpackSize(&db->db, f);
  if (size > MEM_DECODE_MAX) return -FS_EMETHOD;
  Byte *buf = malloc(size ? (size_t)size : 1);
  if (!buf) return -ENOMEM;
  aes_seen = 0; // set when 7zAES runs on this folder
  SRes res = SzAr_DecodeFolder(&db->db, f, &look->vt, db->dataPos, buf, (size_t)size, &g_Alloc);
  int rc = res == SZ_OK ? 0 : res == SZ_ERROR_MEM ? -ENOMEM : aes_seen ? aes_fail(res) : res == SZ_ERROR_UNSUPPORTED ? -FS_EMETHOD : -EIO;
  for (size_t off = 0; !rc && off < size;) {
    size_t k = size - off > (1 << 20) ? (1 << 20) : (size_t)(size - off);
    rc = route_write(r, buf + off, k);
    off += k;
  }
  free(buf);
  return rc;
}
static int sevenz_extract(const char *arc, const char *dest) {
  fdin_t fin = {{fdin_read, fdin_seek}, open(arc, O_RDONLY)};
  if (fin.fd < 0) return -errno;
  CrcGenerateTable();
  CLookToRead2 look;
  LookToRead2_CreateVTable(&look, False);
  look.buf = malloc(1 << 18);
  look.bufSize = 1 << 18;
  look.realStream = &fin.vt;
  LookToRead2_INIT(&look)
  CSzArEx db;
  SzArEx_Init(&db);
  int rc = 0;
  Sz7zAesDecrypt = aes7z_decrypt;
  aes_seen = 0;
  if (!look.buf) rc = -ENOMEM;
  else {
    SRes res = SzArEx_Open(&db, &look.vt, &g_Alloc, &g_Alloc); // an encrypted header needs the password already
    if (res != SZ_OK) rc = res == SZ_ERROR_MEM ? -ENOMEM : aes_seen ? aes_fail(res) : res == SZ_ERROR_UNSUPPORTED ? -FS_EMETHOD : -FS_EARCHIVE;
  }
  if (!rc) {
    long long total = 0;
    for (UInt32 i = 0; i < db.NumFiles; i++) total += (long long)SzArEx_GetFileSize(&db, i);
    job_add_total(total, db.NumFiles);
  }
  // folders and files with no data first
  for (UInt32 i = 0; !rc && i < db.NumFiles; i++) {
    int dir = SzArEx_IsDir(&db, i);
    if (!dir && db.FileToFolder[i] != (UInt32)-1) continue;
    char nm[PMAX], rel[PMAX], full[PMAX];
    if (name7z(&db, i, nm, sizeof nm) || safe_rel(nm, rel, sizeof rel) || target_of(dest, rel, full)) {
      job_tick(0, 1, 0);
      continue;
    }
    if (dir) rc = fs_mkdirs(full);
    else if (!(rc = mkparents(full))) {
      int fd = fs_create(full, 0);
      rc = fd < 0 ? fd : close(fd) ? -errno : 0;
    }
    if (!rc) set_mtime(full, mtime7z(&db, i));
    if (!rc && job_tick(0, 1, base_of(rel))) rc = -ECANCELED;
  }
  for (UInt32 f = 0; !rc && f < db.db.NumFolders; f++) {
    route_t r = {&db, dest, db.FolderToFile[f], db.FolderToFile[f + 1], f, db.FolderToFile[f], -1, 0, 0, ""};
    rc = decode_stream(&look, &db, f, &r);
    if (rc == 1) rc = decode_memory(&look, &db, f, &r);
    if (!rc) rc = route_finish(&r);
    if (r.fd >= 0) close(r.fd);
  }
  for (UInt32 i = 0; !rc && i < db.NumFiles; i++) // folders get their times last (adding files changes them)
    if (SzArEx_IsDir(&db, i)) {
      char nm[PMAX], rel[PMAX], full[PMAX];
      if (!name7z(&db, i, nm, sizeof nm) && !safe_rel(nm, rel, sizeof rel) && !target_of(dest, rel, full))
        set_mtime(full, mtime7z(&db, i));
    }
  SzArEx_Free(&db, &g_Alloc);
  free(look.buf);
  close(fin.fd);
  return rc;
}

// ======================= everything else: libarchive =======================
#define LA_BLOCK (1 << 16)

static int ends_ci(const char *s, const char *e) {
  size_t a = strlen(s), b = strlen(e);
  return a >= b && !strcasecmp(s + a - b, e);
}
// digits in a ".001"-style ending (3 or more), 0 if there's none
static int split_digits(const char *path) {
  const char *d = strrchr(path, '.');
  if (!d || strchr(d, '/')) return 0;
  int n = 0;
  for (const char *c = d + 1; *c; c++, n++)
    if (*c < '0' || *c > '9') return 0;
  return n >= 3 ? n : 0;
}
static int is_split(const char *path) { return split_digits(path) > 0; }
// "name.part07.rar": the digits' offset and count, 0 if it isn't named that way
static int rar_part(const char *path, size_t *at) {
  if (!ends_ci(path, ".rar")) return 0;
  const char *e = path + strlen(path) - 4, *d = e;
  while (d > path && d[-1] >= '0' && d[-1] <= '9') d--;
  if (d == e || d - path < 5 || strncasecmp(d - 5, ".part", 5)) return 0;
  *at = (size_t)(d - path);
  return (int)(e - d);
}

// the files an archive is spread over, in order (one for most archives)
typedef struct {
  char **v; // 0-terminated, for archive_read_open_filenames
  int n;
  long long bytes;
} vols_t;
static void vols_free(vols_t *v) {
  for (int i = 0; i < v->n; i++) free(v->v[i]);
  free(v->v);
  memset(v, 0, sizeof *v);
}
static int vols_add(vols_t *v, const char *p) { // 1 if p isn't there
  struct stat st;
  if (stat(p, &st) || !S_ISREG(st.st_mode)) return 1;
  char **q = realloc(v->v, (size_t)(v->n + 2) * sizeof *q);
  if (!q) return 1;
  v->v = q;
  if (!(q[v->n] = strdup(p))) return 1;
  q[++v->n] = 0;
  v->bytes += (long long)st.st_size;
  return 0;
}
static int vols_find(const char *path, vols_t *v) {
  memset(v, 0, sizeof *v);
  char q[PMAX + 16];
  size_t len = strlen(path), at;
  int nd = split_digits(path);
  if (nd) // name.001, name.002, ... (whichever part was picked)
    for (int i = 1; i < 100000; i++) {
      snprintf(q, sizeof q, "%.*s%0*d", (int)(len - nd), path, nd, i);
      if (vols_add(v, q)) break;
    }
  else if ((nd = rar_part(path, &at))) // name.part1.rar, name.part2.rar, ...
    for (int i = 1; i < 100000; i++) {
      snprintf(q, sizeof q, "%.*s%0*d.rar", (int)at, path, nd, i);
      if (vols_add(v, q)) break;
    }
  if (!v->n) {
    vols_add(v, path);
    if (ends_ci(path, ".rar")) // old style: name.rar, name.r00, name.r01, ...
      for (int i = 0; i < 100; i++) {
        snprintf(q, sizeof q, "%.*s.r%02d", (int)(len - 4), path, i);
        if (vols_add(v, q)) break;
      }
  }
  return v->n ? 0 : -ENOENT;
}

// RAR with a password: each volume is read through rarcrypt, which decrypts it for libarchive
typedef struct {
  rx_shared_t *s;
  const char *path;
  rx_vol_t *v;
} rxcb_t;
static const char *rx_words(int e) { // in libarchive's error, for la_fail
  return e == -FS_EPASSWORD ? "passphrase required" : e == -FS_EBADPW ? "Incorrect passphrase" : e == -FS_EMETHOD ? "unsupported encryption" : "damaged";
}
static int rxcb_open(struct archive *a, void *d) {
  rxcb_t *c = d;
  if (!c->v && !(c->v = rx_open(c->s, c->path))) {
    archive_set_error(a, EIO, "%s", rx_words(rx_error(c->s)));
    return ARCHIVE_FATAL;
  }
  return ARCHIVE_OK;
}
static la_ssize_t rxcb_read(struct archive *a, void *d, const void **buf) {
  rxcb_t *c = d;
  long k = c->v ? rx_read(c->v, buf) : -1;
  if (k < 0) archive_set_error(a, EIO, "%s", rx_words(rx_error(c->s)));
  return k;
}
static int rxcb_close(struct archive *a, void *d) {
  (void)a;
  rxcb_t *c = d;
  rx_close(c->v);
  c->v = 0;
  return ARCHIVE_OK;
}
typedef struct {
  rx_shared_t *s;
  rxcb_t *cb;
} la_rx_t;
static la_rx_t la_rx; // the one in use by the job's extract (la_open with rx)
static void la_rx_free(void) {
  for (int i = 0; la_rx.cb && la_rx.cb[i].path; i++) rx_close(la_rx.cb[i].v);
  free(la_rx.cb);
  rx_shared_free(la_rx.s);
  memset(&la_rx, 0, sizeof la_rx);
}

static struct archive *la_open(const vols_t *v, int single) {
  struct archive *a = archive_read_new();
  if (!a) return 0;
  archive_read_support_filter_all(a);
  if (single) archive_read_support_format_raw(a); // one compressed file: .gz, .bz2, .xz, .lzma, .zst, .Z
  else {
    archive_read_support_format_7zip(a);
    archive_read_support_format_ar(a);
    archive_read_support_format_cab(a);
    archive_read_support_format_cpio(a);
    archive_read_support_format_iso9660(a);
    archive_read_support_format_lha(a);
    archive_read_support_format_rar(a);
    archive_read_support_format_rar5(a);
    archive_read_support_format_tar(a);
    archive_read_support_format_xar(a);
    archive_read_support_format_zip(a);
  }
  if (job_password()[0]) archive_read_add_passphrase(a, job_password());
  if (archive_read_open_filenames(a, (const char **)v->v, LA_BLOCK) != ARCHIVE_OK) {
    archive_read_free(a);
    return 0;
  }
  return a;
}
static struct archive *la_open_rx(const vols_t *v, int *err) {
  la_rx_free();
  struct archive *a = archive_read_new();
  if (!a || !(la_rx.s = rx_shared_new(job_password())) || !(la_rx.cb = calloc((size_t)v->n + 1, sizeof *la_rx.cb))) {
    if (a) archive_read_free(a);
    la_rx_free();
    return 0;
  }
  archive_read_support_format_rar(a);
  archive_read_support_format_rar5(a);
  archive_read_set_open_callback(a, rxcb_open);
  archive_read_set_read_callback(a, rxcb_read);
  archive_read_set_close_callback(a, rxcb_close);
  for (int i = 0; i < v->n; i++) {
    la_rx.cb[i] = (rxcb_t){la_rx.s, v->v[i], 0};
    if (i ? archive_read_append_callback_data(a, &la_rx.cb[i]) : archive_read_set_callback_data(a, &la_rx.cb[i])) {
      archive_read_free(a);
      la_rx_free();
      return 0;
    }
  }
  if (archive_read_open1(a) != ARCHIVE_OK) {
    if (err) *err = rx_error(la_rx.s);
    archive_read_free(a);
    return 0;
  }
  return a;
}
static int la_says(struct archive *a, const char *const *words) {
  const char *m = archive_error_string(a);
  for (int i = 0; m && words[i]; i++)
    if (strcasestr(m, words[i])) return 1;
  return 0;
}
static const char *const LA_METHOD[] = {"encrypt", "passphrase", "password", "unsupported", "not supported", 0};
static int la_probe(const char *path) {
  vols_t v;
  if (vols_find(path, &v)) return AR_NONE;
  if (rx_detect(v.v[0])) return vols_free(&v), AR_MULTI; // RAR with a password
  int k = AR_NONE;
  struct archive_entry *e;
  struct archive *a = la_open(&v, 0);
  if (a) {
    int r = archive_read_next_header(a, &e);
    // a RAR with encrypted names can't be listed, but it is an archive: extracting says why it can't
    if (r == ARCHIVE_OK || r == ARCHIVE_WARN || (r == ARCHIVE_FATAL && archive_format(a) && la_says(a, LA_METHOD))) k = AR_MULTI;
    archive_read_free(a);
  }
  if (k == AR_NONE && v.n == 1 && (a = la_open(&v, 1))) {
    if (archive_read_next_header(a, &e) == ARCHIVE_OK && archive_filter_count(a) > 1) k = AR_SINGLE;
    archive_read_free(a);
  }
  vols_free(&v);
  return k;
}

// a failure libarchive explains: its own words go to the job
static const char *const LA_PASS[] = {"passphrase", "password", 0};
static int la_fail(struct archive *a, const char *what, const char *name) {
  if (la_rx.s) { // RAR with a password: rarcrypt knows best
    int e = rx_error(la_rx.s);
    if (e) return e == -FS_EARCHIVE ? (job_fail_text(what, name, "The archive is damaged or incomplete"), e) : e;
    if (rx_decrypted(la_rx.s) && !job_password()[0]) return -FS_EPASSWORD;
    // decrypted data that doesn't unpack: with no check value to go by (RAR 3.x), the password was wrong
    if (rx_decrypted(la_rx.s) && !rx_verified(la_rx.s)) return -FS_EBADPW;
  }
  const char *m0 = archive_error_string(a);
  if (m0 && strcasestr(m0, "ncorrect passphrase")) return -FS_EBADPW;
  if (la_says(a, LA_PASS)) return job_password()[0] ? -FS_EBADPW : -FS_EPASSWORD;
  if (la_says(a, LA_METHOD)) return -FS_EMETHOD;
  int e = archive_errno(a);
  if (e == ENOMEM || e == ENOSPC || e == EIO) return -e;
  const char *m = archive_error_string(a);
  job_fail_text(what, name, m && m[0] ? m : "The archive is damaged or incomplete");
  return -FS_EARCHIVE;
}
static int la_tick(struct archive *a, long long *seen) { // progress by archive bytes read
  long long b = (long long)archive_filter_bytes(a, -1), d = b - *seen;
  if (d < 0) d = 0;
  *seen = b;
  return job_tick(d, 0, 0);
}
static int copy_into(const char *from, const char *to, char *buf) { // hard links become copies
  int in = open(from, O_RDONLY);
  if (in < 0) return errno == ENOENT ? 0 : -errno;
  int out = fs_create(to, 0), rc = out < 0 ? out : 0;
  for (ssize_t k; !rc && (k = read(in, buf, LA_BLOCK)) != 0;)
    rc = k < 0 ? -errno : write_all(out, buf, (size_t)k);
  if (out >= 0 && close(out) && !rc) rc = -errno;
  close(in);
  return rc;
}
static int la_data(struct archive *a, int fd, char *buf, long long *seen, uLong *crc, blake2sp_state *b2) {
  for (;;) {
    la_ssize_t k = archive_read_data(a, buf, LA_BLOCK);
    if (k == 0) return 0;
    if (k < 0) return la_fail(a, "Couldn't extract", "");
    if (crc) *crc = crc32(*crc, (const Bytef *)buf, (uInt)k);
    if (b2) blake2sp_update(b2, buf, (size_t)k);
    int rc = write_all(fd, buf, (size_t)k);
    if (rc) return rc;
    if (la_tick(a, seen)) return -ECANCELED;
  }
}
static int la_extract_multi(const vols_t *v, const char *dest) {
  int oerr = 0;
  struct archive *a = rx_detect(v->v[0]) ? la_open_rx(v, &oerr) : la_open(v, 0);
  if (!a) {
    la_rx_free();
    return oerr ? oerr : -FS_EARCHIVE;
  }
  char *buf = malloc(LA_BLOCK);
  int rc = buf ? 0 : -ENOMEM;
  long long seen = 0;
  job_add_total(v->bytes, 0);
  while (!rc) {
    struct archive_entry *e;
    int r = archive_read_next_header(a, &e);
    if (r == ARCHIVE_EOF && la_rx.s && rx_error(la_rx.s)) r = ARCHIVE_FATAL; // rarcrypt stopped it: not the end
    if (r == ARCHIVE_EOF) break;
    if (r < ARCHIVE_WARN) {
      rc = la_fail(a, "Couldn't read", base_of(v->v[0]));
      break;
    }
    job_add_total(0, 1);
    if (archive_entry_is_encrypted(e) && !job_password()[0]) {
      rc = -FS_EPASSWORD;
      break;
    }
    const char *nm = archive_entry_pathname_utf8(e), *hl = archive_entry_hardlink_utf8(e);
    if (!nm) nm = archive_entry_pathname(e);
    if (!hl) hl = archive_entry_hardlink(e);
    char rel[PMAX], full[PMAX];
    if (!nm || safe_rel(nm, rel, sizeof rel) || target_of(dest, rel, full)) { // "../x", "/etc/x": skipped
      job_tick(0, 1, 0);
      continue;
    }
    job_tick(0, 0, base_of(rel));
    mode_t t = archive_entry_filetype(e);
    int made = 1;
    if (hl) {
      char hr[PMAX], hf[PMAX];
      if (!safe_rel(hl, hr, sizeof hr) && !target_of(dest, hr, hf) && !(rc = mkparents(full))) rc = copy_into(hf, full, buf);
    } else if (t == AE_IFDIR) rc = fs_mkdirs(full);
    else if (t == AE_IFREG) {
      if (!(rc = mkparents(full))) {
        int fd = fs_create(full, 0);
        if (fd < 0) rc = fd;
        else {
          uLong crc = crc32(0, 0, 0);
          blake2sp_state b2;
          int want_b2 = la_rx.s && rx_wants_blake2(la_rx.s, nm) && !blake2sp_init(&b2, 32);
          rc = la_data(a, fd, buf, &seen, la_rx.s ? &crc : 0, want_b2 ? &b2 : 0);
          if (!rc && want_b2) {
            unsigned char dg[32];
            if (!blake2sp_final(&b2, dg, 32) && rx_check_blake2(la_rx.s, nm, dg) < 0) {
              if (!rx_verified(la_rx.s)) rc = -FS_EBADPW;
              else job_fail_text("Couldn't extract", rel, "The file inside is damaged (checksum error)"), rc = -FS_EARCHIVE;
            }
          }
          if (!rc && la_rx.s && rx_error(la_rx.s)) rc = la_fail(a, "Couldn't extract", rel);
          if (close(fd) && !rc) rc = -errno;
          if (!rc && la_rx.s && rx_check_crc(la_rx.s, nm, (uint32_t)crc) < 0) { // WinRAR's checksum, as a MAC
            if (!rx_verified(la_rx.s)) rc = -FS_EBADPW;
            else job_fail_text("Couldn't extract", rel, "The file inside is damaged (checksum error)"), rc = -FS_EARCHIVE;
          }
        }
      }
    } else made = 0; // symbolic links, devices: left out
    if (!rc && made && archive_entry_mtime_is_set(e)) set_mtime(full, (long long)archive_entry_mtime(e));
    if (rc && rc != -ECANCELED && rc != -FS_EMETHOD && rc != -FS_EARCHIVE && rc != -FS_EPASSWORD && rc != -FS_EBADPW)
      job_fail("Couldn't extract", rel, rc);
    if (!rc && (la_tick(a, &seen) || job_tick(0, 1, 0))) rc = -ECANCELED;
  }
  free(buf);
  archive_read_free(a);
  la_rx_free();
  return rc;
}
static int la_extract_single(const vols_t *v, const char *out) {
  struct archive *a = la_open(v, 1);
  if (!a) return -FS_EARCHIVE;
  char *buf = malloc(LA_BLOCK);
  int rc = buf ? 0 : -ENOMEM, fd = -1;
  long long seen = 0;
  struct archive_entry *e;
  job_add_total(v->bytes, 1);
  job_tick(0, 0, base_of(out));
  if (!rc && archive_read_next_header(a, &e) != ARCHIVE_OK) rc = la_fail(a, "Couldn't read", base_of(v->v[0]));
  if (!rc && (fd = fs_create(out, 1)) < 0) rc = fd;
  if (!rc) rc = la_data(a, fd, buf, &seen, 0, 0);
  if (fd >= 0 && close(fd) && !rc) rc = -errno;
  if (!rc && archive_entry_mtime_is_set(e)) set_mtime(out, (long long)archive_entry_mtime(e));
  if (!rc) job_tick(0, 1, 0);
  free(buf);
  archive_read_free(a);
  return rc;
}

int ar_extract(const char *archive, const char *dest) {
  int k = ar_kind(archive), rc;
  if (k == AR_NONE) return -FS_EARCHIVE;
  if (k == AR_ZIP && (rc = zip_extract(archive, dest)) != -FS_EMETHOD) return rc;
  if (k == AR_7Z && (rc = sevenz_extract(archive, dest)) != -FS_EMETHOD) return rc;
  // ZIP or 7z with a method FileDen's own readers don't know (ZIPX, ...): libarchive tries next
  vols_t v;
  if (vols_find(archive, &v)) return -FS_EARCHIVE;
  rc = k == AR_SINGLE ? la_extract_single(&v, dest) : la_extract_multi(&v, dest);
  vols_free(&v);
  return rc;
}

// the folder (or, for one compressed file, the file) an archive unpacks to
int ar_out_name(const char *archive, char *out, size_t n) {
  static const char *const two[] = {".tar.gz", ".tar.bz2", ".tar.xz", ".tar.zst", ".tar.lzma", ".tar.lz", ".tar.z", 0};
  const char *b = base_of(archive);
  size_t len = strlen(b), at, cut = len;
  int nd;
  for (int i = 0; two[i]; i++)
    if (ends_ci(b, two[i])) cut = len - strlen(two[i]);
  if (cut == len && (nd = rar_part(b, &at))) cut = at - 5; // ".part01.rar"
  if (cut == len && (nd = split_digits(b))) {            // ".7z.001", ".zip.001", ".001"
    cut = len - nd - 1;
    static const char *const in[] = {".7z", ".zip", ".rar", ".tar", 0};
    for (int i = 0; in[i]; i++)
      if (cut >= strlen(in[i]) && !strncasecmp(b + cut - strlen(in[i]), in[i], strlen(in[i]))) cut -= strlen(in[i]);
  }
  if (cut == len) {
    const char *d = strrchr(b, '.');
    if (d && d != b) cut = (size_t)(d - b);
  }
  if (!cut) return snprintf(out, n, "extracted") >= (int)n ? -ENAMETOOLONG : 0;
  return snprintf(out, n, "%.*s", (int)cut, b) >= (int)n ? -ENAMETOOLONG : 0;
}

// ======================= TAR (plain, .gz, .xz, .bz2, .zst) =======================
// TAR (with its compressions) or, with a password, ZIP encrypted with AES-256 (WinZip AE-2; 7-Zip,
// WinRAR, Keka and others open it)
static int la_create(const char *out, const char *const *srcs, int n, int filter) {
  alist_t l;
  int rc = collect_all(&l, out, srcs, n);
  struct archive *w = rc ? 0 : archive_write_new();
  char *buf = malloc(LA_BLOCK);
  if (!rc && (!w || !buf)) rc = -ENOMEM;
  if (!rc) {
    int f = ARCHIVE_OK;
    if (filter == LA_ZIP_AES) {
      archive_write_set_format_zip(w);
      if (archive_write_set_options(w, "zip:encryption=aes256") != ARCHIVE_OK ||
          archive_write_set_passphrase(w, job_password()) != ARCHIVE_OK)
        f = ARCHIVE_FATAL;
    } else archive_write_set_format_pax_restricted(w); // ustar, with pax headers only for long names, big files, UTF-8
    if (f != ARCHIVE_OK) {
    } else f = filter == TF_GZ ? archive_write_add_filter_gzip(w)
          : filter == TF_XZ ? archive_write_add_filter_xz(w)
          : filter == TF_BZ2 ? archive_write_add_filter_bzip2(w)
          : filter == TF_ZST ? archive_write_add_filter_zstd(w)
                             : ARCHIVE_OK;
    if (f != ARCHIVE_OK) rc = -FS_EMETHOD;
    else if (filter == TF_XZ || filter == TF_ZST) // several cores where the library can
      archive_write_set_filter_option(w, 0, "threads", "4");
    if (!rc && archive_write_open_filename(w, out) != ARCHIVE_OK) rc = archive_errno(w) > 0 ? -archive_errno(w) : -EIO;
  }
  for (int i = 0; !rc && i < l.n; i++) {
    aent_t *a = &l.v[i];
    struct archive_entry *e = archive_entry_new();
    if (!e) {
      rc = -ENOMEM;
      break;
    }
    job_tick(0, 0, base_of(a->name));
    if (filter == LA_ZIP_AES && a->dir) { // ZIP: folders end in "/"
      char dn[PMAX + 2];
      snprintf(dn, sizeof dn, "%s/", a->name);
      archive_entry_set_pathname_utf8(e, dn);
    } else archive_entry_set_pathname_utf8(e, a->name);
    archive_entry_set_filetype(e, a->dir ? AE_IFDIR : AE_IFREG);
    archive_entry_set_perm(e, FS_MODE);
    archive_entry_set_size(e, a->dir ? 0 : a->size);
    archive_entry_set_mtime(e, (time_t)a->mtime, 0);
    int fd = a->dir ? -2 : open(a->path, O_RDONLY);
    if (fd == -1) rc = -errno;
    else if (archive_write_header(w, e) != ARCHIVE_OK) rc = -EIO;
    long long left = a->dir ? 0 : a->size;
    while (!rc && left > 0) { // exactly the size in the header, even if the file changes meanwhile
      ssize_t k = read(fd, buf, left < LA_BLOCK ? (size_t)left : LA_BLOCK);
      if (k <= 0) {
        rc = k < 0 ? -errno : -EIO;
        break;
      }
      if (archive_write_data(w, buf, (size_t)k) != k) rc = archive_errno(w) > 0 ? -archive_errno(w) : -EIO;
      left -= k;
      if (!rc && job_tick(k, 0, 0)) rc = -ECANCELED;
    }
    if (fd >= 0) close(fd);
    archive_entry_free(e);
    if (rc && rc != -ECANCELED) job_fail("Couldn't add", a->path, rc);
    if (!rc && job_tick(0, 1, 0)) rc = -ECANCELED;
  }
  if (w) {
    if (!rc) job_finishing();
    if (!rc && archive_write_close(w) != ARCHIVE_OK) rc = archive_errno(w) > 0 ? -archive_errno(w) : -EIO;
    archive_write_free(w);
  }
  if (!rc) chmod(out, FS_MODE);
  free(buf);
  alist_free(&l);
  return rc;
}
int ar_tar_create(const char *out, const char *const *srcs, int n, int filter) { return la_create(out, srcs, n, filter); }
