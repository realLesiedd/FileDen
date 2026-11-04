// FileDen - file operations: listing, places, info, mkdir/new file/rename/save/chmod,
// the PS5 system path guard, and the background job (copy, move, delete, zip, 7z,
// extract, SHA-256, folder size, recursive permissions) with progress for both UIs.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "fsops.h"
#include "title.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/vfs.h>
#else
#include <sys/param.h>
#include <sys/mount.h>
#endif
#include "archive.h"
#include "third_party/lzma/Sha256.h"

#ifndef PATH_MAX
#define PATH_MAX 1024
#endif
#define PMAX 1024 // our own limit for paths we build

// ---- growable text buffer ----
typedef struct {
  char *p;
  size_t n, cap;
  int bad;
} sb_t;
static void sb_put(sb_t *b, const char *s, size_t n) {
  if (b->bad) return;
  if (b->n + n + 1 > b->cap) {
    size_t c = b->cap ? b->cap : 4096;
    while (c < b->n + n + 1) c *= 2;
    char *q = realloc(b->p, c);
    if (!q) {
      b->bad = 1;
      return;
    }
    b->p = q, b->cap = c;
  }
  memcpy(b->p + b->n, s, n);
  b->n += n;
  b->p[b->n] = 0;
}
static void sb_str(sb_t *b, const char *s) { sb_put(b, s, strlen(s)); }
static void sb_fmt(sb_t *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void sb_fmt(sb_t *b, const char *fmt, ...) {
  char t[512];
  va_list ap;
  va_start(ap, fmt);
  int k = vsnprintf(t, sizeof t, fmt, ap);
  va_end(ap);
  if (k > 0) sb_put(b, t, (size_t)k < sizeof t ? (size_t)k : sizeof t - 1);
}
static void sb_json(sb_t *b, const char *s) { // quoted, escaped JSON string
  sb_put(b, "\"", 1);
  for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
    if (*p == '"' || *p == '\\') {
      char e[2] = {'\\', (char)*p};
      sb_put(b, e, 2);
    } else if (*p < 0x20) {
      char e[8];
      snprintf(e, sizeof e, "\\u%04x", *p);
      sb_put(b, e, 6);
    } else sb_put(b, (const char *)p, 1);
  }
  sb_put(b, "\"", 1);
}
static void sb_tsv(sb_t *b, const char *s) { // tab, newline, CR and backslash escaped
  for (; *s; s++) {
    if (*s == '\t') sb_put(b, "\\t", 2);
    else if (*s == '\n') sb_put(b, "\\n", 2);
    else if (*s == '\r') sb_put(b, "\\r", 2);
    else if (*s == '\\') sb_put(b, "\\\\", 2);
    else sb_put(b, s, 1);
  }
}
void fs_json_str(char *o, size_t n, const char *s) {
  sb_t b = {0};
  sb_json(&b, s);
  snprintf(o, n, "%s", b.p && !b.bad && b.n < n ? b.p : "\"\""); // never half a string: that isn't JSON
  free(b.p);
}

// ---- paths ----
int fs_path_ok(const char *p) {
  if (!p || p[0] != '/' || strlen(p) >= PMAX - 64) return 0;
  if (!p[1]) return 1; // "/"
  for (const char *s = p + 1;;) {
    const char *e = strchr(s, '/');
    size_t l = e ? (size_t)(e - s) : strlen(s);
    if (!l || (l == 1 && s[0] == '.') || (l == 2 && s[0] == '.' && s[1] == '.')) return 0;
    if (!e) return 1;
    s = e + 1;
  }
}
int fs_name_ok(const char *n) {
  return n && n[0] && !strchr(n, '/') && strcmp(n, ".") && strcmp(n, "..") && strlen(n) < 256;
}
static void parent_of(const char *p, char *o, size_t n) {
  snprintf(o, n, "%s", p);
  char *s = strrchr(o, '/');
  if (s == o) o[1] = 0;
  else if (s) *s = 0;
}
static const char *base_of(const char *p) {
  const char *s = strrchr(p, '/');
  return s ? s + 1 : p;
}
static int join(char *o, size_t n, const char *dir, const char *name) {
  int k = snprintf(o, n, "%s%s%s", dir, strcmp(dir, "/") ? "/" : "", name);
  return k > 0 && (size_t)k < n ? 0 : -ENAMETOOLONG;
}
static int exists(const char *p) {
  struct stat st;
  return !lstat(p, &st);
}

// ---- PS5 system paths ----
// Trees where FileDen changes nothing at all: the firmware and its read-only copies, the
// system databases (/system_data/priv/mms: app.db, appinfo.db, addcontent.db,
// av_content_bg.db), the registry (/system_data/settings, /user/settings/system.eap),
// accounts, saves and licences (/user/home, /user/license), device nodes (/dev holds
// the flash), and the sandboxes the console mounts for running apps.
static const char *const LOCKED_TREES[] = {
    "/system", "/system_ex", "/system_data", "/system_tmp", "/preinst", "/preinst2", "/update", "/dev",
    "/adm", "/eap_user", "/eap_vsh", "/host", "/hostapp", "/user/home", "/user/settings", "/user/system",
    "/user/license", "/user/licence", "/user/priv", "/user/sshared", "/mnt/sandbox", "/mnt/disc", "/mnt/proc", 0};
// Folders that must stay where they are (their contents are yours to manage): roots, mount
// points, the folders the console keeps games, updates, add-ons, captures and trophies in,
// and FileDen's own settings folder.
static const char *const LOCKED_ENTRIES[] = {
    "/", "/data", "/user", "/mnt", "/mnt/ext0", "/mnt/ext1", "/user/app", "/user/patch", "/user/patch0",
    "/user/addcont", "/user/appmeta", "/user/av_contents", "/user/download", "/user/trophy", "/user/data",
    "/user/common", "/user/temp", "/user/bgft", "/data/fileden", "/data/sce_coredumps", "/data/sce_logs", 0};
// Folders where nothing new may be created directly (mount points, roots of the system tree)
static const char *const NO_NEW_IN[] = {"/", "/mnt", "/user", 0};

static int under(const char *p, const char *root) { // p == root or p inside root
  size_t l = strlen(root);
  return !strncmp(p, root, l) && (p[l] == 0 || p[l] == '/');
}
static int lex_system(const char *p) {
  for (int i = 0; LOCKED_TREES[i]; i++)
    if (under(p, LOCKED_TREES[i])) return 1;
  return 0;
}
static int lex_entry_locked(const char *p) {
  if (lex_system(p)) return 1;
  for (int i = 0; LOCKED_ENTRIES[i]; i++)
    if (!strcmp(p, LOCKED_ENTRIES[i])) return 1;
  return !strncmp(p, "/mnt/usb", 8) && p[8] >= '0' && p[8] <= '9' && !p[9];
}
// the path with symlinks resolved, so a link can't be used to reach a system folder
static int resolved(const char *p, char *o) {
  char *r = realpath(p, 0);
  if (!r) return 0;
  snprintf(o, PMAX, "%s", r);
  free(r);
  return 1;
}
int fs_is_system(const char *p) {
  char r[PMAX];
  return lex_system(p) || (resolved(p, r) && lex_system(r));
}
static char guard_hit[PMAX]; // the last path the guard refused (for the error message)
static int refuse(const char *p) {
  snprintf(guard_hit, sizeof guard_hit, "%s", p);
  return -FS_ESYSTEM;
}
int fs_guard_entry(const char *p) {
  char r[PMAX], par[PMAX];
  if (lex_entry_locked(p)) return refuse(p);
  if (resolved(p, r) && lex_entry_locked(r)) {
    // the entry is a link: only what it points to matters if the link itself sits in a locked place
    struct stat st;
    if (lstat(p, &st) || !S_ISLNK(st.st_mode)) return refuse(p);
  }
  parent_of(p, par, sizeof par); // a link inside a system folder is still system
  if (resolved(par, r) && lex_system(r)) return refuse(p);
  return 0;
}
int fs_guard_write(const char *dir) {
  char r[PMAX];
  for (int k = 0; k < 2; k++) {
    const char *d = k ? (resolved(dir, r) ? r : 0) : dir;
    if (!d) continue;
    if (lex_system(d)) return refuse(dir);
    for (int i = 0; NO_NEW_IN[i]; i++)
      if (!strcmp(d, NO_NEW_IN[i])) return refuse(dir);
  }
  return 0;
}
void fs_system_msg(char *o, size_t n) {
  struct stat st;
  const char *b = base_of(guard_hit);
  int dir = !lstat(guard_hit, &st) && S_ISDIR(st.st_mode);
  snprintf(o, n, "\"%s\" is a PS5 system %s. FileDen won't delete, move or change it", b[0] ? b : "/", dir ? "folder" : "file");
}
static int guard_parent(const char *p) {
  char d[PMAX];
  parent_of(p, d, sizeof d);
  return fs_guard_write(d);
}

int fs_unique(const char *dir, const char *name, char *out, size_t on) {
  if (join(out, on, dir, name)) return -ENAMETOOLONG;
  if (!exists(out)) return 0;
  char stem[256], ext[256] = "";
  snprintf(stem, sizeof stem, "%s", name);
  char *dot = strrchr(stem, '.');
  if (dot && dot != stem) { // "photo.png" -> "photo (2).png"; ".profile" keeps its name
    snprintf(ext, sizeof ext, "%s", dot);
    *dot = 0;
  }
  for (int i = 2; i < 10000; i++) {
    char nm[600];
    snprintf(nm, sizeof nm, "%s (%d)%s", stem, i, ext);
    if (join(out, on, dir, nm)) return -ENAMETOOLONG;
    if (!exists(out)) return 0;
  }
  return -EEXIST;
}

const char *fs_err(int rc) {
  switch (-rc) {
  case FS_ESYSTEM: return "That's a PS5 system file or folder, so FileDen won't change it";
  case FS_EARCHIVE: return "Not an archive FileDen can open";
  case FS_EMETHOD: return "This archive uses a compression method or encryption FileDen doesn't support";
  case FS_EPASSWORD: return "This archive needs a password";
  case FS_EBADPW: return "Wrong password";
  case EEXIST: return "Something with that name is already there";
  case ENOENT: return "It isn't there any more";
  case EACCES:
  case EPERM: return "Not allowed here";
  case EROFS: return "That drive is read-only";
  case ENOSPC: return "Not enough free space";
  case ENOTEMPTY: return "The folder isn't empty";
  case EXDEV: return "Can't move across drives";
  case ENAMETOOLONG: return "The name or path is too long";
  case EINVAL: return "That isn't allowed";
  case EBUSY: return "Another job (copy, move, delete, archive) is still running";
  case ECANCELED: return "Cancelled";
  case EISDIR: return "That's a folder";
  case ENOTDIR: return "That isn't a folder";
  case ELOOP: return "A folder can't go inside itself";
  case ENOMEM: return "Not enough memory";
  case EIO: return "Read or write error";
  case EFBIG: return "Too big";
  default: return strerror(-rc);
  }
}

// ---- creating things: always 0777 (the console only starts apps whose files are 0777) ----
int fs_create(const char *p, int excl) {
  int fd = open(p, O_WRONLY | O_CREAT | O_NOFOLLOW | (excl ? O_EXCL : O_TRUNC), FS_MODE); // never write through a link
  if (fd < 0) return -errno;
  fchmod(fd, FS_MODE); // whatever the umask took away
  return fd;
}
static int mkdir777(const char *p) {
  if (mkdir(p, FS_MODE)) return -errno;
  chmod(p, FS_MODE);
  return 0;
}
int fs_mkdirs(const char *p) {
  char t[PMAX];
  snprintf(t, sizeof t, "%s", p);
  for (char *s = t + 1;; s++) {
    if (*s == '/' || !*s) {
      char c = *s;
      *s = 0;
      struct stat st;
      if (stat(t, &st)) {
        int rc = mkdir777(t);
        if (rc && rc != -EEXIST) return rc;
      } else if (!S_ISDIR(st.st_mode)) return -ENOTDIR;
      *s = c;
      if (!c) return 0;
    }
  }
}

// ---- listing ----
typedef struct {
  char *name;
  int dir, link;
  long long size, mtime;
} ent_t;
static int ent_cmp(const void *a, const void *b) {
  const ent_t *x = a, *y = b;
  if (x->dir != y->dir) return y->dir - x->dir;
  int c = strcasecmp(x->name, y->name);
  return c ? c : strcmp(x->name, y->name);
}
char *fs_list(const char *path, int hidden, int tsv) {
  DIR *d = opendir(path);
  if (!d) return 0;
  ent_t *e = 0;
  size_t n = 0, cap = 0;
  struct dirent *de;
  char p[PMAX];
  while ((de = readdir(d))) {
    if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
    if (!hidden && de->d_name[0] == '.') continue;
    if (n == cap) {
      ent_t *q = realloc(e, (cap = cap ? cap * 2 : 128) * sizeof *e);
      if (!q) break;
      e = q;
    }
    ent_t *x = &e[n];
    memset(x, 0, sizeof *x);
    if (!(x->name = strdup(de->d_name))) break;
    struct stat st, lst;
    int haveslot = !join(p, sizeof p, path, de->d_name);
    if (haveslot && !lstat(p, &lst)) {
      x->link = S_ISLNK(lst.st_mode);
      if (x->link && !stat(p, &st)) lst = st; // a link to a folder opens like a folder
      x->dir = S_ISDIR(lst.st_mode);
      x->size = x->dir ? -1 : (long long)lst.st_size;
      x->mtime = (long long)lst.st_mtime;
    } else {
      x->dir = de->d_type == DT_DIR;
      x->size = -1;
    }
    n++;
  }
  closedir(d);
  if (n) qsort(e, n, sizeof *e, ent_cmp);
  sb_t b = {0};
  int sys = fs_is_system(path);
  if (tsv) {
    for (size_t i = 0; i < n; i++) { // type, size, modified, locked (PS5 system item), name
      int lk = sys;
      if (!sys && !join(p, sizeof p, path, e[i].name)) lk = lex_entry_locked(p);
      sb_fmt(&b, "%c\t%lld\t%lld\t%d\t", e[i].dir ? (e[i].link ? 'D' : 'd') : (e[i].link ? 'L' : 'f'), e[i].size, e[i].mtime, lk);
      sb_tsv(&b, e[i].name);
      title_t t; // a game, app or package: its ID, name and whether it has an icon
      if ((e[i].dir || !e[i].link) && !join(p, sizeof p, path, e[i].name) && !title_of(p, e[i].dir, &t)) {
        sb_put(&b, "\t", 1), sb_tsv(&b, t.id), sb_put(&b, "\t", 1), sb_tsv(&b, t.name);
        sb_fmt(&b, "\t%d", t.icon);
      }
      sb_put(&b, "\n", 1);
    }
    if (!b.p) sb_put(&b, "", 0);
  } else {
    sb_str(&b, "{\"path\":");
    sb_json(&b, path);
    sb_fmt(&b, ",\"system\":%d,\"items\":[", sys);
    for (size_t i = 0; i < n; i++) {
      sb_str(&b, i ? ",{\"n\":" : "{\"n\":");
      sb_json(&b, e[i].name);
      int lk = 0;
      if (!sys && !join(p, sizeof p, path, e[i].name)) lk = lex_entry_locked(p);
      sb_fmt(&b, ",\"d\":%d,\"l\":%d,\"s\":%lld,\"m\":%lld,\"k\":%d", e[i].dir, e[i].link, e[i].size, e[i].mtime, sys || lk);
      title_t t;
      if (!join(p, sizeof p, path, e[i].name) && !title_of(p, e[i].dir, &t)) {
        sb_str(&b, ",\"tid\":"), sb_json(&b, t.id), sb_str(&b, ",\"tn\":"), sb_json(&b, t.name);
        sb_fmt(&b, ",\"ic\":%d", t.icon);
      }
      sb_str(&b, "}");
    }
    sb_str(&b, "]}");
  }
  for (size_t i = 0; i < n; i++) free(e[i].name);
  free(e);
  if (b.bad) {
    free(b.p);
    errno = ENOMEM;
    return 0;
  }
  return b.p;
}

static int space_of(const char *p, long long *fr, long long *tot) {
  struct statfs s;
  if (statfs(p, &s)) return -1;
  *fr = (long long)s.f_bavail * (long long)s.f_bsize;
  *tot = (long long)s.f_blocks * (long long)s.f_bsize;
  return 0;
}
char *fs_places(int tsv) {
  sb_t b = {0};
  struct {
    char p[32], l[40];
  } c[16];
  int k = 0;
  snprintf(c[k].p, 32, "/data"), snprintf(c[k++].l, 40, "Internal");
  for (int i = 0; i < 8; i++) snprintf(c[k].p, 32, "/mnt/usb%d", i), snprintf(c[k++].l, 40, "USB %d", i + 1);
  snprintf(c[k].p, 32, "/mnt/ext0"), snprintf(c[k++].l, 40, "Extended");
  snprintf(c[k].p, 32, "/mnt/ext1"), snprintf(c[k++].l, 40, "M.2 SSD");
  snprintf(c[k].p, 32, "/user"), snprintf(c[k++].l, 40, "System");
  snprintf(c[k].p, 32, "/"), snprintf(c[k++].l, 40, "Root");
  if (!tsv) sb_str(&b, "[");
  int first = 1;
  long long devdata = -1, devmnt = -1;
  struct stat ms;
  if (!stat("/mnt", &ms)) devmnt = (long long)ms.st_dev;
  for (int i = 0; i < k; i++) {
    struct stat st;
    if (stat(c[i].p, &st) || !S_ISDIR(st.st_mode)) continue;
    long long fr = -1, tot = -1;
    space_of(c[i].p, &fr, &tot);
    if (!strcmp(c[i].p, "/data")) devdata = (long long)st.st_dev;
    // the PS5 keeps /mnt/usb0-7 and /mnt/ext0-1 as empty folders: only one with a drive mounted on it
    // (a device of its own, not /mnt's) is a tab
    if ((!strncmp(c[i].p, "/mnt/usb", 8) || !strncmp(c[i].p, "/mnt/ext", 8)) &&
        (tot <= 0 || (long long)st.st_dev == devmnt || (long long)st.st_dev == devdata))
      continue;
    if (tsv) {
      sb_fmt(&b, "%s\t%s\t%lld\t%lld\n", c[i].p, c[i].l, fr, tot);
    } else {
      sb_fmt(&b, "%s{\"p\":\"%s\",\"n\":\"%s\",\"free\":%lld,\"total\":%lld}", first ? "" : ",", c[i].p, c[i].l, fr, tot);
    }
    first = 0;
  }
  if (!tsv) sb_str(&b, "]");
  if (!b.p) sb_put(&b, "", 0);
  if (b.bad) {
    free(b.p);
    return 0;
  }
  return b.p;
}

char *fs_info(const char *p) {
  struct stat lst, st;
  if (lstat(p, &lst)) return 0;
  int link = S_ISLNK(lst.st_mode);
  st = lst;
  if (link) stat(p, &st); // a link that leads nowhere keeps its own details
  long long btime = 0;
#if defined(st_birthtime) || defined(__FreeBSD__) || defined(__PROSPERO__)
  btime = (long long)lst.st_birthtime;
#endif
  sb_t b = {0};
  sb_str(&b, "{\"path\":");
  sb_json(&b, p);
  sb_str(&b, ",\"name\":");
  sb_json(&b, base_of(p)[0] ? base_of(p) : "/");
  const char *type = S_ISDIR(st.st_mode) ? "folder" : S_ISREG(st.st_mode) ? "file" : link ? "link" : "other";
  sb_fmt(&b, ",\"type\":\"%s\",\"link\":%d,\"size\":%lld,\"mode\":\"%04o\",\"uid\":%d,\"gid\":%d", type, link,
         S_ISREG(st.st_mode) ? (long long)st.st_size : -1LL, (unsigned)(lst.st_mode & 07777), (int)lst.st_uid, (int)lst.st_gid);
  sb_fmt(&b, ",\"created\":%lld,\"modified\":%lld,\"changed\":%lld,\"accessed\":%lld", btime, (long long)lst.st_mtime,
         (long long)lst.st_ctime, (long long)lst.st_atime);
  if (link) {
    char t[PMAX];
    ssize_t k = readlink(p, t, sizeof t - 1);
    t[k > 0 ? k : 0] = 0;
    sb_str(&b, ",\"target\":");
    sb_json(&b, t);
  }
  int sys = fs_guard_entry(p) != 0;
  sb_fmt(&b, ",\"system\":%d,\"system_tree\":%d}", sys, fs_is_system(p));
  if (b.bad) {
    free(b.p);
    return 0;
  }
  return b.p;
}

// ---- small operations ----
int fs_mkdir(const char *p) {
  if (!fs_path_ok(p) || !strcmp(p, "/")) return -EINVAL;
  int g = guard_parent(p);
  return g ? g : mkdir777(p);
}
int fs_newfile(const char *p) {
  if (!fs_path_ok(p) || !strcmp(p, "/")) return -EINVAL;
  int g = guard_parent(p);
  if (g) return g;
  int fd = fs_create(p, 1);
  if (fd < 0) return fd;
  close(fd);
  return 0;
}
// ---- undo: the last rename or move, put back ----
static struct {
  int kind; // 0 nothing, 1 a rename, 2 a move
  int n;
  char **now, **was; // where each item is now, and where it was
} U;
static pthread_mutex_t U_mu = PTHREAD_MUTEX_INITIALIZER;
static void undo_clear_locked(void) {
  for (int i = 0; i < U.n; i++) free(U.now[i]), free(U.was[i]);
  free(U.now), free(U.was);
  memset(&U, 0, sizeof U);
}
static void undo_begin(int kind) {
  pthread_mutex_lock(&U_mu);
  undo_clear_locked();
  U.kind = kind;
  pthread_mutex_unlock(&U_mu);
}
static void undo_add(const char *now, const char *was) {
  pthread_mutex_lock(&U_mu);
  char **a = realloc(U.now, (U.n + 1) * sizeof *a), **b = a ? realloc(U.was, (U.n + 1) * sizeof *b) : 0;
  if (a) U.now = a;
  if (b) U.was = b;
  if (a && b && (U.now[U.n] = strdup(now)) && (U.was[U.n] = strdup(was))) U.n++;
  pthread_mutex_unlock(&U_mu);
}
int fs_undo_info(char *o, size_t n) {
  pthread_mutex_lock(&U_mu);
  int k = U.n ? U.kind : 0;
  if (k == 1) snprintf(o, n, "Undo rename: \"%s\" back to \"%s\"", base_of(U.now[0]), base_of(U.was[0]));
  else if (k == 2 && U.n == 1) snprintf(o, n, "Undo move: \"%s\" back", base_of(U.now[0]));
  else if (k == 2) snprintf(o, n, "Undo move: %d items back", U.n);
  else if (n) o[0] = 0;
  pthread_mutex_unlock(&U_mu);
  return k;
}
// 0 done, 1 a job is moving things back (another drive), <0 an error
int fs_undo(void) {
  pthread_mutex_lock(&U_mu);
  int kind = U.n ? U.kind : 0, rc = kind ? 0 : -ENOENT, nleft = 0;
  char *left = 0, dir0[PMAX] = "";
  size_t ll = 0;
  for (int i = 0; kind && i < U.n && !rc; i++) {
    if (!exists(U.now[i])) {
      rc = -ENOENT;
      break;
    }
    if (exists(U.was[i]) && strcasecmp(U.now[i], U.was[i])) {
      rc = -EEXIST; // something else took its old place
      break;
    }
  }
  for (int i = 0; !rc && kind && i < U.n; i++) {
    if (!rename(U.now[i], U.was[i])) continue;
    if (errno != EXDEV) {
      rc = -errno;
      break;
    }
    char dir[PMAX]; // another drive: copy it back with a move job (to the folder it came from)
    parent_of(U.was[i], dir, sizeof dir);
    if (dir0[0] && strcmp(dir0, dir)) {
      rc = -EXDEV;
      break;
    }
    snprintf(dir0, sizeof dir0, "%s", dir);
    size_t l = strlen(U.now[i]);
    char *q = realloc(left, ll + l + 2);
    if (!q) {
      rc = -ENOMEM;
      break;
    }
    left = q, memcpy(left + ll, U.now[i], l), ll += l, left[ll++] = '\n', left[ll] = 0, nleft++;
  }
  if (!rc) undo_clear_locked();
  pthread_mutex_unlock(&U_mu);
  if (!rc && nleft) {
    left[ll - 1] = 0;
    rc = fs_job_start(JOB_MOVE, left, dir0, 0);
    if (!rc) rc = 1;
  }
  free(left);
  return rc;
}
int fs_rename(const char *p, const char *name, char *out, size_t on) {
  if (!fs_path_ok(p) || !fs_name_ok(name)) return -EINVAL;
  int g = fs_guard_entry(p);
  if (g) return g;
  char dir[PMAX];
  parent_of(p, dir, sizeof dir);
  if (join(out, on, dir, name)) return -ENAMETOOLONG;
  if (!strcmp(out, p)) return 0;
  if (exists(out) && strcasecmp(out, p)) return -EEXIST; // a case-only change is allowed
  if (rename(p, out)) return -errno;
  undo_begin(1), undo_add(out, p);
  return 0;
}
int fs_chmod(const char *p, int mode) {
  if (!fs_path_ok(p) || mode < 0 || mode > 07777) return -EINVAL;
  int g = fs_guard_entry(p);
  char r[PMAX];
  if (!g && resolved(p, r) && lex_entry_locked(r)) g = refuse(p); // chmod follows links
  if (g) return g;
  return chmod(p, (mode_t)mode) ? -errno : 0;
}
int fs_save(const char *p, const void *data, size_t n) {
  if (!fs_path_ok(p) || !strcmp(p, "/")) return -EINVAL;
  int g = guard_parent(p);
  if (!g && exists(p)) g = fs_guard_entry(p);
  if (g) return g;
  char tmp[PMAX + 32];
  snprintf(tmp, sizeof tmp, "%s.fileden-tmp", p);
  int fd = fs_create(tmp, 0);
  if (fd < 0) return fd;
  const char *s = data;
  for (size_t left = n; left;) {
    ssize_t w = write(fd, s, left);
    if (w <= 0) {
      int e = w < 0 ? errno : EIO;
      close(fd);
      unlink(tmp);
      return -e;
    }
    s += w, left -= w;
  }
  if (close(fd)) {
    int e = errno;
    unlink(tmp);
    return -e;
  }
  if (rename(tmp, p)) {
    int e = errno;
    unlink(tmp);
    return -e;
  }
  return 0;
}

// ---- background job ----
static struct {
  pthread_mutex_t mu;
  int id, state, op, arg; // state: 0 idle, 1 counting, 2 running, 3 done, 4 failed, 5 cancelled
  volatile int cancel;
  long long total_b, done_b, total_n, done_n;
  long long t0;
  char cur[300], err[400], dest[PMAX], result[PMAX];
  char pw[256];                // the archive password of the job (zip, 7z, extract)
  int need_pw;                 // the job failed for want of a password: 1 none given, 2 a wrong one
  int finishing;               // everything is read; the compressor is writing its last part
  char *paths;
  char *buf;
} J = {.mu = PTHREAD_MUTEX_INITIALIZER};
const char *job_password(void) { return J.pw; }
#define CHUNK (4 << 20)

static long long now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}
static void set_cur(const char *p) {
  pthread_mutex_lock(&J.mu);
  snprintf(J.cur, sizeof J.cur, "%s", base_of(p));
  pthread_mutex_unlock(&J.mu);
}
static void add_done(long long b, long long n) {
  pthread_mutex_lock(&J.mu);
  J.done_b += b, J.done_n += n;
  pthread_mutex_unlock(&J.mu);
}
static void fail(const char *what, const char *p, int rc) {
  pthread_mutex_lock(&J.mu);
  if (!J.err[0]) snprintf(J.err, sizeof J.err, "%s %s: %s", what, base_of(p), fs_err(rc));
  pthread_mutex_unlock(&J.mu);
}
int job_tick(long long bytes, long long items, const char *cur) {
  pthread_mutex_lock(&J.mu);
  J.done_b += bytes, J.done_n += items;
  if (cur) snprintf(J.cur, sizeof J.cur, "%s", cur);
  pthread_mutex_unlock(&J.mu);
  return J.cancel ? -ECANCELED : 0;
}
void job_finishing(void) {
  pthread_mutex_lock(&J.mu);
  J.finishing = 1;
  pthread_mutex_unlock(&J.mu);
}
void job_add_total(long long bytes, long long items) {
  pthread_mutex_lock(&J.mu);
  J.total_b += bytes, J.total_n += items;
  pthread_mutex_unlock(&J.mu);
}
void job_fail(const char *what, const char *p, int rc) { fail(what, p, rc); }
void job_fail_text(const char *what, const char *p, const char *why) {
  pthread_mutex_lock(&J.mu);
  if (!J.err[0]) snprintf(J.err, sizeof J.err, "%s %s: %s", what, base_of(p), why);
  pthread_mutex_unlock(&J.mu);
}
int job_cancelled(void) { return J.cancel; }
static void set_result(const char *s) {
  pthread_mutex_lock(&J.mu);
  snprintf(J.result, sizeof J.result, "%s", s);
  pthread_mutex_unlock(&J.mu);
}

static int rm_tree(const char *p, int depth);
// size, entries, files and folders under p (p included), without touching the job
static void measure4(const char *p, int depth, long long *b, long long *n, long long *nf, long long *nd) {
  struct stat st;
  if (J.cancel || lstat(p, &st)) return;
  (*n)++;
  if (S_ISREG(st.st_mode)) *b += st.st_size, (*nf)++;
  if (!S_ISDIR(st.st_mode) || depth > 64) return;
  (*nd)++;
  DIR *d = opendir(p);
  if (!d) return;
  struct dirent *de;
  char *c = malloc(PMAX);
  while (c && (de = readdir(d)) && !J.cancel) {
    if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
    if (!join(c, PMAX, p, de->d_name)) measure4(c, depth + 1, b, n, nf, nd);
  }
  free(c);
  closedir(d);
}
static void measure(const char *p, int depth, long long *b, long long *n) {
  long long nf = 0, nd = 0;
  measure4(p, depth, b, n, &nf, &nd);
}
static void set_done(long long b, long long n) {
  pthread_mutex_lock(&J.mu);
  J.done_b = b, J.done_n = n;
  pthread_mutex_unlock(&J.mu);
}
// after a cancelled job: remove the partial result, keep the counters as they were
static void undo_partial(const char *dst) {
  pthread_mutex_lock(&J.mu);
  long long b = J.done_b, n = J.done_n;
  J.cancel = 0;
  pthread_mutex_unlock(&J.mu);
  rm_tree(dst, 0);
  set_done(b, n);
}

static int copy_file(const char *s, const char *t) {
  int in = open(s, O_RDONLY);
  if (in < 0) return -errno;
  int out = fs_create(t, 1);
  if (out < 0) {
    close(in);
    return out;
  }
  int rc = 0;
  for (;;) {
    if (J.cancel) {
      rc = -ECANCELED;
      break;
    }
    ssize_t r = read(in, J.buf, CHUNK);
    if (r < 0) {
      rc = -errno;
      break;
    }
    if (!r) break;
    for (ssize_t off = 0; off < r;) {
      ssize_t w = write(out, J.buf + off, r - off);
      if (w <= 0) {
        rc = w < 0 ? -errno : -EIO;
        break;
      }
      off += w;
    }
    if (rc) break;
    add_done(r, 0);
  }
  close(in);
  if (close(out) && !rc) rc = -errno;
  if (rc) unlink(t);
  return rc;
}
static int copy_tree(const char *s, const char *t, int depth) {
  struct stat st;
  if (J.cancel) return -ECANCELED;
  if (lstat(s, &st)) return -errno;
  set_cur(s);
  int rc = 0;
  if (S_ISDIR(st.st_mode)) {
    if (depth > 64) return -ELOOP;
    if ((rc = mkdir777(t)) && rc != -EEXIST) return rc;
    rc = 0;
    add_done(0, 1);
    DIR *d = opendir(s);
    if (!d) return -errno;
    struct dirent *de;
    char *cs = malloc(PMAX), *ct = malloc(PMAX);
    if (!cs || !ct) rc = -ENOMEM;
    while (!rc && (de = readdir(d))) {
      if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
      if (join(cs, PMAX, s, de->d_name) || join(ct, PMAX, t, de->d_name)) {
        rc = -ENAMETOOLONG;
        break;
      }
      rc = copy_tree(cs, ct, depth + 1);
    }
    free(cs), free(ct);
    closedir(d);
    if (rc && rc != -ECANCELED && !J.err[0]) fail("Couldn't copy", s, rc);
    return rc;
  }
  if (S_ISLNK(st.st_mode)) {
    char l[PMAX];
    ssize_t k = readlink(s, l, sizeof l - 1);
    if (k < 0) return -errno;
    l[k] = 0;
    rc = symlink(l, t) ? -errno : 0;
  } else if (S_ISREG(st.st_mode)) rc = copy_file(s, t);
  else rc = 0; // devices, sockets: skipped
  if (!rc) add_done(0, 1);
  else if (rc != -ECANCELED) fail("Couldn't copy", s, rc);
  return rc;
}
static int rm_tree(const char *p, int depth) {
  struct stat st;
  if (J.cancel) return -ECANCELED;
  if (lstat(p, &st)) return errno == ENOENT ? 0 : -errno;
  set_cur(p);
  int rc = 0;
  if (S_ISDIR(st.st_mode)) {
    if (depth > 64) return -ELOOP;
    DIR *d = opendir(p);
    if (!d) return -errno;
    struct dirent *de;
    char *c = malloc(PMAX);
    if (!c) rc = -ENOMEM;
    while (!rc && (de = readdir(d))) {
      if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
      rc = join(c, PMAX, p, de->d_name) ? -ENAMETOOLONG : rm_tree(c, depth + 1);
    }
    free(c);
    closedir(d);
    if (!rc && rmdir(p)) rc = -errno;
  } else {
    if (unlink(p)) rc = -errno;
    else add_done(st.st_size, 0);
  }
  if (!rc) add_done(0, 1);
  else if (rc != -ECANCELED) fail("Couldn't delete", p, rc);
  return rc;
}
static int chmod_tree(const char *p, int mode, int depth) {
  struct stat st;
  if (J.cancel) return -ECANCELED;
  if (lstat(p, &st)) return -errno;
  if (S_ISLNK(st.st_mode)) return 0; // links have no permissions of their own here
  set_cur(p);
  if (chmod(p, (mode_t)mode)) {
    int rc = -errno;
    fail("Couldn't change", p, rc);
    return rc;
  }
  add_done(S_ISREG(st.st_mode) ? st.st_size : 0, 1);
  if (!S_ISDIR(st.st_mode) || depth > 64) return 0;
  DIR *d = opendir(p);
  if (!d) return -errno;
  struct dirent *de;
  char *c = malloc(PMAX);
  int rc = c ? 0 : -ENOMEM;
  while (!rc && (de = readdir(d))) {
    if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
    rc = join(c, PMAX, p, de->d_name) ? -ENAMETOOLONG : chmod_tree(c, mode, depth + 1);
  }
  free(c);
  closedir(d);
  return rc;
}
static int hash_file(const char *p) {
  int fd = open(p, O_RDONLY);
  if (fd < 0) return -errno;
  CSha256 h;
  Sha256_Init(&h);
  int rc = 0;
  set_cur(p);
  for (;;) {
    ssize_t r = read(fd, J.buf, CHUNK);
    if (r < 0) {
      rc = -errno;
      break;
    }
    if (!r) break;
    Sha256_Update(&h, (const Byte *)J.buf, (size_t)r);
    if ((rc = job_tick(r, 0, 0))) break;
  }
  close(fd);
  if (rc) return rc;
  Byte dg[SHA256_DIGEST_SIZE];
  Sha256_Final(&h, dg);
  char hex[SHA256_DIGEST_SIZE * 2 + 1];
  for (int i = 0; i < SHA256_DIGEST_SIZE; i++) snprintf(hex + i * 2, 3, "%02x", dg[i]);
  set_result(hex);
  add_done(0, 1);
  return 0;
}

// splits the newline separated list into an array of paths (pointing into J.paths)
static int split_paths(char *s, char ***out) {
  int n = 1;
  for (char *c = s; *c; c++) n += *c == '\n';
  char **v = calloc(n, sizeof *v);
  if (!v) return -1;
  int k = 0;
  for (char *c = s; *c;) {
    char *e = c + strcspn(c, "\n");
    char save = *e;
    *e = 0;
    if (*c) v[k++] = c;
    c = save ? e + 1 : e;
  }
  *out = v;
  return k;
}

// jobs waiting their turn: started one after another, each checked again when its turn comes
typedef struct {
  int op, arg;
  char *paths, *dest, pw[256];
} qjob_t;
static qjob_t Q[32];
static int nq;
static void job_next(void) {
  usleep(1500000); // the finished job's result stays up long enough for the apps to show it
  for (;;) {
    pthread_mutex_lock(&J.mu);
    if (!nq || J.state == 1 || J.state == 2) { // nothing waiting, or something else started meanwhile
      pthread_mutex_unlock(&J.mu);
      return;
    }
    qjob_t q = Q[0];
    memmove(Q, Q + 1, --nq * sizeof *Q);
    memset(&Q[nq], 0, sizeof *Q); // no stale copy of a password left in the slot
    pthread_mutex_unlock(&J.mu);
    int rc = fs_job_start_pw(q.op, q.paths, q.dest, q.arg, q.pw);
    memset(q.pw, 0, sizeof q.pw);
    free(q.paths), free(q.dest);
    if (!rc) return;
    pthread_mutex_lock(&J.mu); // it can't start (the files went, say): say so and go on
    J.id++, J.op = q.op, J.state = 4, J.total_b = J.done_b = J.total_n = J.done_n = 0;
    J.cur[0] = J.result[0] = 0;
    snprintf(J.err, sizeof J.err, "A queued job couldn't start: %s", fs_err(rc));
    pthread_mutex_unlock(&J.mu);
    usleep(1500000);
  }
}
void fs_job_cancel_all(void) {
  pthread_mutex_lock(&J.mu);
  for (int i = 0; i < nq; i++) memset(Q[i].pw, 0, sizeof Q[i].pw), free(Q[i].paths), free(Q[i].dest);
  nq = 0;
  J.cancel = 1;
  pthread_mutex_unlock(&J.mu);
}
static void *job_main(void *u) {
  (void)u;
  if (J.op == JOB_MOVE) undo_begin(2); // this move is what Undo puts back
  char *paths = J.paths, src[PMAX], dst[PMAX], dir[PMAX];
  int rc = 0;
  // pass 1: total size, so the bar means something (an extract learns it from the archive)
  if (J.op != JOB_EXTRACT)
    for (char *s = paths; *s && !J.cancel;) {
      char *e = s + strcspn(s, "\n");
      snprintf(src, sizeof src, "%.*s", (int)(e - s), s);
      s = *e ? e + 1 : e;
      long long b = 0, n = 0, nf = 0, nd = 0;
      if (src[0]) measure4(src, 0, &b, &n, &nf, &nd);
      if (J.op == JOB_SIZE) {
        char r[200];
        snprintf(r, sizeof r, "%lld %lld %lld", b, nf, nd > 0 ? nd - 1 : 0); // bytes, files, folders inside
        set_result(r);
      }
      pthread_mutex_lock(&J.mu);
      J.total_b += b, J.total_n += n;
      if (J.op == JOB_SIZE) J.done_b = J.total_b, J.done_n = J.total_n;
      pthread_mutex_unlock(&J.mu);
    }
  pthread_mutex_lock(&J.mu);
  J.state = 2;
  J.t0 = now_ms();
  pthread_mutex_unlock(&J.mu);
  if (J.cancel) rc = -ECANCELED;
  else if (J.op == JOB_SIZE) rc = 0;
  else if (J.op == JOB_HASH) {
    snprintf(src, sizeof src, "%.*s", (int)strcspn(paths, "\n"), paths);
    rc = hash_file(src);
  } else if (J.op == JOB_CHMOD) {
    snprintf(src, sizeof src, "%.*s", (int)strcspn(paths, "\n"), paths);
    rc = chmod_tree(src, J.arg, 0);
  } else if (J.op == JOB_ZIP || J.op == JOB_7Z || J.op == JOB_TAR) {
    char **v = 0;
    int n = split_paths(paths, &v);
    const char *const *cv = (const char *const *)v;
    rc = n < 0 ? -ENOMEM
       : J.op == JOB_ZIP ? ar_zip_create(J.dest, cv, n)
       : J.op == JOB_7Z ? ar_7z_create(J.dest, cv, n)
                         : ar_tar_create(J.dest, cv, n, J.arg);
    free(v);
    if (rc) { // no half archive left behind
      if (rc != -ECANCELED && !J.err[0]) fail("Couldn't create", J.dest, rc);
      unlink(J.dest);
    } else set_result(J.dest);
  } else if (J.op == JOB_EXTRACT) {
    snprintf(src, sizeof src, "%.*s", (int)strcspn(paths, "\n"), paths);
    int single = ar_kind(src) == AR_SINGLE; // one compressed file: unpacks to a file, not a folder
    rc = single ? 0 : mkdir777(J.dest);
    if (!rc) rc = ar_extract(src, J.dest);
    if (rc == -ECANCELED) undo_partial(J.dest), rc = -ECANCELED;
    else if (rc) {
      if (rc == -FS_EPASSWORD || rc == -FS_EBADPW) J.need_pw = rc == -FS_EPASSWORD ? 1 : 2, J.err[0] = 0;
      if (!J.err[0]) fail("Couldn't extract", src, rc);
      pthread_mutex_lock(&J.mu);
      J.cancel = 0;
      pthread_mutex_unlock(&J.mu);
      rm_tree(J.dest, 0);
    } else set_result(J.dest);
  } else
    for (char *s = paths; *s && !rc;) {
      char *e = s + strcspn(s, "\n");
      snprintf(src, sizeof src, "%.*s", (int)(e - s), s);
      s = *e ? e + 1 : e;
      if (!src[0]) continue;
      if (J.cancel) {
        rc = -ECANCELED;
        break;
      }
      if (J.op == JOB_DELETE) {
        rc = rm_tree(src, 0);
        continue;
      }
      parent_of(src, dir, sizeof dir);
      if (J.op == JOB_MOVE && !strcmp(dir, J.dest)) { // already here
        long long b = 0, n = 0;
        measure(src, 0, &b, &n);
        add_done(b, n);
        continue;
      }
      if ((rc = fs_unique(J.dest, base_of(src), dst, sizeof dst))) {
        fail("Couldn't place", src, rc);
        break;
      }
      if (J.op == JOB_MOVE) {
        set_cur(src);
        long long b = 0, n = 0;
        measure(src, 0, &b, &n);
        if (!rename(src, dst)) { // same drive: one step
          add_done(b, n);
          undo_add(dst, src);
          continue;
        }
        if (errno != EXDEV) {
          rc = -errno;
          fail("Couldn't move", src, rc);
          break;
        }
        // another drive: copy, then delete the original
        if ((rc = copy_tree(src, dst, 0))) {
          if (rc == -ECANCELED) undo_partial(dst);
          break;
        }
        pthread_mutex_lock(&J.mu);
        long long cb = J.done_b, cn = J.done_n;
        pthread_mutex_unlock(&J.mu);
        int r2 = rm_tree(src, 0);
        set_done(cb, cn); // the copy already counted these
        if (!r2) undo_add(dst, src);
        if (r2 == -ECANCELED) { // cancelled while removing the original: both copies stay
          rc = r2;
          break;
        }
        continue;
      }
      rc = copy_tree(src, dst, 0);
      if (rc == -ECANCELED) undo_partial(dst); // don't leave half a file or folder behind
    }
  pthread_mutex_lock(&J.mu);
  J.state = rc == -ECANCELED ? 5 : rc ? 4 : 3;
  if (rc && rc != -ECANCELED && !J.err[0]) snprintf(J.err, sizeof J.err, "%s", fs_err(rc));
  J.cur[0] = 0;
  memset(J.pw, 0, sizeof J.pw); // done with the password
  free(J.paths);
  J.paths = 0;
  pthread_mutex_unlock(&J.mu);
  job_next();
  return 0;
}

int fs_job_running(void) {
  pthread_mutex_lock(&J.mu);
  int r = J.state == 1 || J.state == 2;
  pthread_mutex_unlock(&J.mu);
  return r;
}
int fs_job_start(int op, const char *paths, const char *dest, int arg) { return fs_job_start_pw(op, paths, dest, arg, ""); }
int fs_job_start_pw(int op, const char *paths, const char *dest, int arg, const char *pw) {
  struct stat st;
  if (!pw) pw = "";
  char target[PMAX] = "", dir[PMAX];
  if (op < JOB_COPY || op > JOB_TAR) return -EINVAL;
  // where the result goes
  if (op == JOB_COPY || op == JOB_MOVE) {
    if (!fs_path_ok(dest)) return -EINVAL;
    if (stat(dest, &st) || !S_ISDIR(st.st_mode)) return -ENOTDIR;
    int g = fs_guard_write(dest);
    if (g) return g;
    snprintf(target, sizeof target, "%s", dest);
  } else if (op == JOB_ZIP || op == JOB_7Z || op == JOB_TAR || op == JOB_EXTRACT) { // dest: archive / folder to create
    if (!fs_path_ok(dest) || !strcmp(dest, "/")) return -EINVAL;
    parent_of(dest, dir, sizeof dir);
    if (stat(dir, &st) || !S_ISDIR(st.st_mode)) return -ENOTDIR;
    int g = fs_guard_write(dir);
    if (g) return g;
    int rc = fs_unique(dir, base_of(dest), target, sizeof target);
    if (rc) return rc;
  }
  // every source
  int n = 0;
  for (const char *s = paths; *s;) {
    const char *e = s + strcspn(s, "\n");
    char p[PMAX];
    snprintf(p, sizeof p, "%.*s", (int)(e - s), s);
    s = *e ? e + 1 : e;
    if (!p[0]) continue;
    if (!fs_path_ok(p)) return -EINVAL;
    if (lstat(p, &st)) return -ENOENT;
    if (op == JOB_MOVE || op == JOB_DELETE || op == JOB_CHMOD) {
      int g = fs_guard_entry(p);
      if (g) return g;
    }
    if (op == JOB_COPY || op == JOB_MOVE) { // not into itself
      size_t l = strlen(p);
      if (!strcmp(target, p) || (!strncmp(target, p, l) && target[l] == '/')) return -ELOOP;
    }
    if ((op == JOB_HASH || op == JOB_EXTRACT) && !S_ISREG(st.st_mode)) return -EISDIR;
    n++;
  }
  if (!n || ((op == JOB_HASH || op == JOB_EXTRACT || op == JOB_CHMOD || op == JOB_SIZE) && n != 1)) return -EINVAL;
  if (op == JOB_CHMOD && (arg < 0 || arg > 07777)) return -EINVAL;
  if (op == JOB_TAR && (arg < TF_NONE || arg > TF_ZST)) return -EINVAL;
  if (op == JOB_EXTRACT) {
    char a[PMAX];
    snprintf(a, sizeof a, "%.*s", (int)strcspn(paths, "\n"), paths);
    if (!ar_kind(a)) return -FS_EARCHIVE;
  }
  pthread_mutex_lock(&J.mu);
  if (J.state == 1 || J.state == 2) { // busy: copies, moves, archives and the like wait their turn
    if (op == JOB_HASH || op == JOB_SIZE || nq == (int)(sizeof Q / sizeof *Q)) {
      pthread_mutex_unlock(&J.mu);
      return -EBUSY;
    }
    qjob_t *q = &Q[nq];
    q->op = op, q->arg = arg, q->paths = strdup(paths), q->dest = strdup(dest ? dest : "");
    snprintf(q->pw, sizeof q->pw, "%s", pw);
    if (!q->paths || !q->dest) {
      free(q->paths), free(q->dest);
      pthread_mutex_unlock(&J.mu);
      return -ENOMEM;
    }
    nq++;
    pthread_mutex_unlock(&J.mu);
    return 0;
  }
  if (!J.buf && !(J.buf = malloc(CHUNK))) {
    pthread_mutex_unlock(&J.mu);
    return -ENOMEM;
  }
  free(J.paths);
  if (!(J.paths = strdup(paths))) {
    pthread_mutex_unlock(&J.mu);
    return -ENOMEM;
  }
  J.id++, J.state = 1, J.op = op, J.arg = arg, J.cancel = 0;
  snprintf(J.pw, sizeof J.pw, "%s", pw), J.need_pw = 0, J.finishing = 0; // the password goes with this job only
  J.total_b = J.done_b = J.total_n = J.done_n = 0;
  J.cur[0] = J.err[0] = J.result[0] = 0;
  snprintf(J.dest, sizeof J.dest, "%s", target);
  pthread_mutex_unlock(&J.mu);
  pthread_t t;
  pthread_attr_t at;
  pthread_attr_init(&at);
  pthread_attr_setstacksize(&at, 8 << 20);
  pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
  int e = pthread_create(&t, &at, job_main, 0);
  pthread_attr_destroy(&at);
  if (e) {
    pthread_mutex_lock(&J.mu);
    J.state = 4;
    snprintf(J.err, sizeof J.err, "Couldn't start the job");
    pthread_mutex_unlock(&J.mu);
    return -e;
  }
  return 0;
}
void fs_job_cancel(void) { J.cancel = 1; }
void fs_job_json(char *o, size_t n) {
  static const char *const st[] = {"idle", "counting", "running", "done", "failed", "cancelled"};
  static const char *const ops[] = {"copy", "move", "delete", "zip", "7z", "extract", "hash", "size", "chmod", "tar"};
  pthread_mutex_lock(&J.mu);
  long long el = J.state == 2 ? now_ms() - J.t0 : 0, sp = el > 500 ? J.done_b * 1000 / el : 0;
  char cur[700], err[900], dest[PMAX * 2], res[PMAX * 2];
  fs_json_str(cur, sizeof cur, J.cur);
  fs_json_str(err, sizeof err, J.err);
  fs_json_str(dest, sizeof dest, J.dest);
  fs_json_str(res, sizeof res, J.result);
  snprintf(o, n,
           "{\"id\":%d,\"state\":\"%s\",\"op\":\"%s\",\"total_b\":%lld,\"done_b\":%lld,\"total_n\":%lld,\"done_n\":%lld,"
           "\"speed\":%lld,\"cur\":%s,\"err\":%s,\"dest\":%s,\"result\":%s,\"need_pw\":%d,\"finishing\":%d,\"queued\":%d,"
           "\"elapsed\":%lld}",
           J.id, st[J.state], ops[J.op], J.total_b, J.done_b, J.total_n, J.done_n, sp, cur, err, dest, res, J.need_pw,
           J.finishing, nq, el);
  pthread_mutex_unlock(&J.mu);
}
