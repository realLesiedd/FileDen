// FileDen: what a game or homebrew folder, or a .pkg, is called and where its icon is.
#include "title.h"
#include <ctype.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *base_of(const char *p) {
  const char *s = strrchr(p, '/');
  return s ? s + 1 : p;
}
// CUSA12345, PPSA01234, NPXS20001 ... (four capitals, five digits), possibly with something after
int title_id_like(const char *n) {
  for (int i = 0; i < 4; i++)
    if (n[i] < 'A' || n[i] > 'Z') return 0;
  for (int i = 4; i < 9; i++)
    if (!isdigit((unsigned char)n[i])) return 0;
  return !n[9] || !isalnum((unsigned char)n[9]);
}
static int read_at(int fd, long long off, void *b, size_t n) { return pread(fd, b, n, (off_t)off) == (ssize_t)n ? 0 : -1; }
static unsigned be32(const unsigned char *p) { return (unsigned)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static unsigned le32(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (unsigned)p[3] << 24; }
static unsigned le16(const unsigned char *p) { return p[0] | p[1] << 8; }

static void clean(char *s) { // one line, no control characters
  for (char *c = s; *c; c++)
    if ((unsigned char)*c < 32) *c = ' ';
}
// param.sfo (PS4): TITLE and TITLE_ID
static void from_sfo(const unsigned char *d, size_t n, title_t *t) {
  if (n < 20 || memcmp(d, "\0PSF", 4)) return;
  unsigned kt = le32(d + 8), dt = le32(d + 12), cnt = le32(d + 16);
  for (unsigned i = 0; i < cnt && 20 + i * 16 + 16 <= n; i++) {
    const unsigned char *e = d + 20 + i * 16;
    size_t ko = (size_t)kt + le16(e), len = le32(e + 4), vo = (size_t)dt + le32(e + 12); // 64-bit: no wrapping
    if (ko >= n || vo > n || len > n - vo) continue;
    const char *k = (const char *)d + ko, *v = (const char *)d + vo;
    size_t kl = strnlen(k, n - ko);
    if (kl == 5 && !memcmp(k, "TITLE", 5) && !t->name[0]) snprintf(t->name, sizeof t->name, "%.*s", (int)strnlen(v, len), v);
    if (kl == 8 && !memcmp(k, "TITLE_ID", 8) && !t->id[0]) snprintf(t->id, sizeof t->id, "%.*s", (int)strnlen(v, len), v);
  }
  clean(t->name);
}
// a JSON string value starting at the opening quote
static void json_str(const char *s, const char *end, char *o, size_t on) {
  size_t k = 0;
  if (s >= end || *s != '"') return;
  for (s++; s < end && *s != '"' && k + 5 < on; s++) {
    if (*s == '\\' && s + 1 < end) {
      s++;
      if (*s == 'u' && s + 4 < end) {
        char h[5] = {s[1], s[2], s[3], s[4], 0};
        unsigned c = (unsigned)strtoul(h, 0, 16);
        s += 4;
        if (c < 0x80) o[k++] = (char)c;
        else if (c < 0x800) o[k++] = (char)(0xc0 | c >> 6), o[k++] = (char)(0x80 | (c & 63));
        else o[k++] = (char)(0xe0 | c >> 12), o[k++] = (char)(0x80 | ((c >> 6) & 63)), o[k++] = (char)(0x80 | (c & 63));
        continue;
      }
      o[k++] = *s == 'n' ? ' ' : *s == 't' ? ' ' : *s;
    } else o[k++] = *s;
  }
  o[k] = 0;
}
static const char *json_key(const char *s, const char *end, const char *key) { // the value after "key":
  size_t kl = strlen(key);
  for (const char *p = s; p + kl + 2 < end; p++)
    if (*p == '"' && !strncmp(p + 1, key, kl) && p[kl + 1] == '"') {
      const char *v = p + kl + 2;
      while (v < end && (*v == ' ' || *v == ':' || *v == '\n' || *v == '\r' || *v == '\t')) v++;
      return v;
    }
  return 0;
}
// param.json (PS5): titleId, and the titleName of the default language
static void from_json(const char *d, size_t n, title_t *t) {
  const char *end = d + n, *v;
  if ((v = json_key(d, end, "titleId")) && !t->id[0]) json_str(v, end, t->id, sizeof t->id);
  char lang[16] = "";
  if ((v = json_key(d, end, "defaultLanguage"))) json_str(v, end, lang, sizeof lang);
  const char *from = d;
  if (lang[0] && (v = json_key(d, end, lang))) from = v;
  if (!t->name[0] && ((v = json_key(from, end, "titleName")) || (v = json_key(d, end, "titleName"))))
    json_str(v, end, t->name, sizeof t->name);
  clean(t->name);
}
static unsigned char *slurp(const char *p, size_t max, size_t *n) {
  int fd = open(p, O_RDONLY);
  if (fd < 0) return 0;
  struct stat st;
  unsigned char *b = 0;
  if (!fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_size > 0 && (size_t)st.st_size <= max && (b = malloc((size_t)st.st_size + 1))) {
    if (read_at(fd, 0, b, (size_t)st.st_size)) free(b), b = 0;
    else *n = (size_t)st.st_size, b[*n] = 0;
  }
  close(fd);
  return b;
}

// a PS4-style package ("\x7FCNT"): the entry table has param.sfo (0x1000) and icon0.png (0x1200);
// PS5 packages keep a param.json among the small entries
#define PKG_SFO 0x1000
#define PKG_ICON 0x1200
static int pkg_entries(int fd, unsigned *cnt, unsigned *table) {
  unsigned char h[0x64];
  if (read_at(fd, 0, h, sizeof h) || be32(h) != 0x7F434E54) return -1;
  *cnt = be32(h + 0x10), *table = be32(h + 0x18);
  return *cnt && *cnt < 4096 ? 0 : -1;
}
static int pkg_info(const char *p, title_t *t) {
  int fd = open(p, O_RDONLY);
  if (fd < 0) return -1;
  unsigned cnt, table;
  unsigned char h[0x64];
  if (pkg_entries(fd, &cnt, &table) || read_at(fd, 0, h, sizeof h)) return close(fd), -1;
  char cid[37];
  memcpy(cid, h + 0x40, 36), cid[36] = 0; // UP0000-CUSA12345_00-...
  if (strlen(cid) > 16 && title_id_like(cid + 7)) snprintf(t->id, sizeof t->id, "%.9s", cid + 7);
  for (unsigned i = 0; i < cnt && i < 256; i++) {
    unsigned char e[32];
    if (read_at(fd, (long long)table + i * 32, e, 32)) break;
    unsigned id = be32(e), off = be32(e + 16), sz = be32(e + 20);
    if (id == PKG_ICON && sz) t->icon = 1;
    if ((id == PKG_SFO || (sz > 16 && sz < (64 << 10))) && !t->name[0] && sz < (1 << 20)) {
      unsigned char *b = malloc(sz + 1);
      if (b && !read_at(fd, off, b, sz)) {
        b[sz] = 0;
        if (id == PKG_SFO) from_sfo(b, sz, t);
        else if (b[0] == '{' && strstr((char *)b, "titleName")) from_json((char *)b, sz, t);
      }
      free(b);
    }
  }
  close(fd);
  return t->name[0] || t->icon ? 0 : -1;
}
unsigned char *title_pkg_icon(const char *p, size_t *n) {
  int fd = open(p, O_RDONLY);
  if (fd < 0) return 0;
  unsigned cnt, table;
  unsigned char *b = 0;
  if (!pkg_entries(fd, &cnt, &table))
    for (unsigned i = 0; i < cnt && i < 256 && !b; i++) {
      unsigned char e[32];
      if (read_at(fd, (long long)table + i * 32, e, 32)) break;
      unsigned off = be32(e + 16), sz = be32(e + 20);
      if (be32(e) != PKG_ICON || !sz || sz > (8 << 20)) continue;
      if ((b = malloc(sz)) && read_at(fd, off, b, sz)) free(b), b = 0;
      else *n = sz;
    }
  close(fd);
  return b;
}

// where a title folder keeps its icon and parameters: sce_sys/ (a game dump, homebrew, an app folder),
// the folder itself (/user/appmeta/<ID>), or, for an installed app (/user/app/<ID>, only app.pkg inside),
// its /user/appmeta/<ID>
static int meta_dir(const char *path, char *o, size_t n) {
  struct stat st;
  snprintf(o, n, "%s/sce_sys", path);
  if (!stat(o, &st) && S_ISDIR(st.st_mode)) return 0;
  char p[1200];
  snprintf(p, sizeof p, "%s/param.json", path);
  int here = !stat(p, &st);
  if (!here) snprintf(p, sizeof p, "%s/param.sfo", path), here = !stat(p, &st);
  if (!here) snprintf(p, sizeof p, "%s/icon0.png", path), here = !stat(p, &st);
  if (here) return snprintf(o, n, "%s", path), 0;
  const char *b = base_of(path);
  if (title_id_like(b)) {
    snprintf(o, n, "/user/appmeta/%.9s", b);
    if (strcmp(o, path) && !stat(o, &st) && S_ISDIR(st.st_mode)) return 0;
  }
  return -1;
}
int title_icon_path(const char *dir, char *o, size_t n) {
  char m[1100];
  struct stat st;
  if (meta_dir(dir, m, sizeof m)) return -1;
  snprintf(o, n, "%s/icon0.png", m);
  return !stat(o, &st) && S_ISREG(st.st_mode) ? 0 : -1;
}

// a folder is checked only when it's worth it: one named like a title ID, or one inside a
// homebrew / games / apps folder
static int title_dir(const char *path, title_t *t) {
  const char *b = base_of(path);
  if (!title_id_like(b)) {
    char par[64] = "";
    const char *s = path + strlen(path) - strlen(b);
    if (s > path) {
      const char *q = s - 1;
      while (q > path && q[-1] != '/') q--;
      snprintf(par, sizeof par, "%.*s", (int)(s - 1 - q), q);
    }
    if (strcasecmp(par, "homebrew") && strcasecmp(par, "games") && strcasecmp(par, "apps")) return -1;
  }
  char m[1100], p[1200];
  if (meta_dir(path, m, sizeof m)) return -1;
  struct stat st;
  snprintf(p, sizeof p, "%s/icon0.png", m);
  t->icon = !stat(p, &st) && S_ISREG(st.st_mode);
  size_t n = 0;
  unsigned char *d;
  snprintf(p, sizeof p, "%s/param.json", m);
  if ((d = slurp(p, 1 << 20, &n))) from_json((char *)d, n, t), free(d);
  snprintf(p, sizeof p, "%s/param.sfo", m);
  if (!t->name[0] && (d = slurp(p, 1 << 20, &n))) from_sfo(d, n, t), free(d);
  return t->name[0] || t->icon ? 0 : -1;
}

// listings ask again every time a folder opens: answers are kept, and reused while the folder (and
// its sce_sys) or the package hasn't changed
#define TC_N 512
static struct {
  char *path;
  long long k1, k2;
  int rc;
  title_t t;
} tc[TC_N];
static pthread_mutex_t tc_mu = PTHREAD_MUTEX_INITIALIZER;
static void keys_of(const char *path, int isdir, long long *k1, long long *k2) {
  struct stat st;
  *k1 = *k2 = -1;
  if (stat(path, &st)) return;
  *k1 = (long long)st.st_mtime * 1000003 + (long long)st.st_size;
  if (!isdir) return;
  // the parameter file and icon themselves: an editor's save (a rename) gives them a new inode
  static const char *const f[] = {"sce_sys/param.json", "sce_sys/param.sfo", "param.json", "param.sfo", 0};
  char p[1200];
  for (int i = 0; f[i]; i++) {
    snprintf(p, sizeof p, "%s/%s", path, f[i]);
    if (!stat(p, &st)) {
      *k2 = ((long long)st.st_ino * 31 + (long long)st.st_size) * 1000003 + (long long)st.st_mtime;
      break;
    }
  }
  snprintf(p, sizeof p, "%s/sce_sys/icon0.png", path);
  if (!stat(p, &st)) *k2 = *k2 * 31 + (long long)st.st_ino;
}
int title_of(const char *path, int isdir, title_t *t) {
  memset(t, 0, sizeof *t);
  const char *b = base_of(path);
  size_t l = strlen(b);
  if (!isdir && !(l > 4 && !strcasecmp(b + l - 4, ".pkg"))) return -1;
  long long k1, k2;
  keys_of(path, isdir, &k1, &k2);
  unsigned h = 2166136261u;
  for (const char *c = path; *c; c++) h = (h ^ (unsigned char)*c) * 16777619u;
  int slot = (int)(h % TC_N);
  pthread_mutex_lock(&tc_mu);
  if (tc[slot].path && !strcmp(tc[slot].path, path) && tc[slot].k1 == k1 && tc[slot].k2 == k2) {
    *t = tc[slot].t;
    int rc = tc[slot].rc;
    pthread_mutex_unlock(&tc_mu);
    return rc;
  }
  pthread_mutex_unlock(&tc_mu);
  int rc = isdir ? title_dir(path, t) : pkg_info(path, t);
  if (rc) memset(t, 0, sizeof *t);
  char *cp = strdup(path);
  pthread_mutex_lock(&tc_mu);
  free(tc[slot].path);
  tc[slot].path = cp, tc[slot].k1 = k1, tc[slot].k2 = k2, tc[slot].rc = rc, tc[slot].t = *t;
  pthread_mutex_unlock(&tc_mu);
  return rc;
}
