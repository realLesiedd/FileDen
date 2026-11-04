// FileDen - file manager payload for jailbroken PS5 consoles.
// Serves the web UI and the JSON API on :9923. The full screen app talks to the
// same API on 127.0.0.1 (the app's sandbox can't be assumed to see the whole
// file system, the payload can). With the web server setting off it listens on
// 127.0.0.1 only and quits when the app closes; with it on it listens on the
// network too and keeps running, so a phone or PC can manage files on its own.
// SPDX-License-Identifier: GPL-3.0-or-later
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include "fsops.h"
#include "archive.h"
#include "image.h"
#include "index_html.h"
#include "app/logo.h"
#include "web_fonts.h"
#include "version.h"

#define PORT 9923
#define MAX_FORM (1 << 20)   // form posts (path lists can be long)
#define MAX_SAVE (16 << 20)  // text editor saves
static char DATA[256] = "/data/fileden";

// ---- settings: /data/fileden/settings.ini ----
static struct {
  int web;     // 1: listen on the network and keep running after the app closes
  int hidden;  // show names starting with "."
  int confirm; // ask before deleting
  char theme[24]; // colour palette, shared by the TV app and the web UI
  int grid;       // the TV app shows folders as a grid
  int sound;      // the TV app's menu sounds
  int intro;      // the TV app shows its controls card at start (until X is pressed on it)
  int sort;       // the TV app's order: 0 name, 1 size, 2 date, 3 type
} S = {1, 0, 1, "montane-mint", 0, 1, 1, 0};
static pthread_mutex_t set_mu = PTHREAD_MUTEX_INITIALIZER;
static void settings_load(void) {
  char p[300], l[200];
  snprintf(p, sizeof p, "%s/settings.ini", DATA);
  FILE *f = fopen(p, "r");
  if (!f) return;
  while (fgets(l, sizeof l, f)) {
    int v;
    if (sscanf(l, "web=%d", &v) == 1) S.web = !!v;
    else if (sscanf(l, "hidden=%d", &v) == 1) S.hidden = !!v;
    else if (sscanf(l, "confirm=%d", &v) == 1) S.confirm = !!v;
    else if (sscanf(l, "grid=%d", &v) == 1) S.grid = !!v;
    else if (sscanf(l, "sound=%d", &v) == 1) S.sound = !!v;
    else if (sscanf(l, "intro=%d", &v) == 1) S.intro = !!v;
    else if (sscanf(l, "sort=%d", &v) == 1) S.sort = v & 3;
    else if (!strncmp(l, "theme=", 6)) snprintf(S.theme, sizeof S.theme, "%.*s", (int)strcspn(l + 6, "\r\n"), l + 6);
  }
  fclose(f);
}
static void settings_save(void) {
  char p[300], t[300];
  snprintf(p, sizeof p, "%s/settings.ini", DATA);
  snprintf(t, sizeof t, "%s/settings.tmp", DATA);
  FILE *f = fopen(t, "w");
  if (!f) return;
  fprintf(f, "web=%d\nhidden=%d\nconfirm=%d\ntheme=%s\ngrid=%d\nsound=%d\nintro=%d\nsort=%d\n", S.web, S.hidden, S.confirm, S.theme, S.grid,
          S.sound, S.intro, S.sort);
  if (!fclose(f)) rename(t, p);
}

// ---- favourites: /data/fileden/favourites.txt, one path per line, shared by the app and the web UI ----
#define MAX_FAVS 64
static char *favs[MAX_FAVS];
static int nfavs;
static void favs_load(void) {
  char p[300], l[1200];
  snprintf(p, sizeof p, "%s/favourites.txt", DATA);
  FILE *f = fopen(p, "r");
  if (!f) return;
  while (nfavs < MAX_FAVS && fgets(l, sizeof l, f)) {
    l[strcspn(l, "\r\n")] = 0;
    if (fs_path_ok(l) && (favs[nfavs] = strdup(l))) nfavs++;
  }
  fclose(f);
}
static int favs_save(void) {
  char p[300], t[300];
  snprintf(p, sizeof p, "%s/favourites.txt", DATA);
  snprintf(t, sizeof t, "%s/favourites.tmp", DATA);
  FILE *f = fopen(t, "w");
  if (!f) return -errno;
  for (int i = 0; i < nfavs; i++) fprintf(f, "%s\n", favs[i]);
  if (fclose(f)) return -EIO;
  chmod(t, 0777);
  return rename(t, p) ? -errno : 0;
}
static int fav_find(const char *p) {
  for (int i = 0; i < nfavs; i++)
    if (!strcmp(favs[i], p)) return i;
  return -1;
}
static int fav_set(const char *p, int on) { // under set_mu
  for (const char *c = p; *c; c++)
    if ((unsigned char)*c < 0x20) return -EINVAL; // one path per line in the file
  int i = fav_find(p);
  if (on && i < 0) {
    if (nfavs == MAX_FAVS) return -ENOSPC;
    if (!(favs[nfavs] = strdup(p))) return -ENOMEM;
    nfavs++;
  } else if (!on && i >= 0) {
    free(favs[i]);
    memmove(favs + i, favs + i + 1, (size_t)(nfavs - i - 1) * sizeof *favs);
    nfavs--;
  } else return 0;
  return favs_save();
}
// JSON [{"path":..,"dir":0|1,"gone":0|1}] or TSV "dir\tgone\tpath"
static char *favs_list(int tsv) {
  size_t cap = 256, n = 0;
  char *o = malloc(cap), e[8000];
  if (!o) return 0;
  if (!tsv) o[n++] = '[';
  for (int i = 0; i < nfavs; i++) {
    struct stat st;
    int gone = stat(favs[i], &st) != 0, dir = !gone && S_ISDIR(st.st_mode);
    if (tsv) snprintf(e, sizeof e, "%d\t%d\t%s\n", dir, gone, favs[i]);
    else {
      char j[7300];
      fs_json_str(j, sizeof j, favs[i]);
      snprintf(e, sizeof e, "%s{\"path\":%s,\"dir\":%d,\"gone\":%d}", i ? "," : "", j, dir, gone);
    }
    size_t l = strlen(e);
    if (n + l + 2 > cap) { // grow: each entry is sized as it comes
      char *g = realloc(o, cap = (n + l + 2) * 2);
      if (!g) return free(o), (char *)0;
      o = g;
    }
    memcpy(o + n, e, l), n += l;
  }
  if (!tsv) o[n++] = ']';
  o[n] = 0;
  return o;
}

// ---- notifications ----
#ifdef HOST
static void notify(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vprintf(fmt, ap);
  va_end(ap);
  putchar('\n');
  fflush(stdout);
}
#else
typedef struct {
  char useless1[45];
  char message[3075];
} notify_request_t;
int sceKernelSendNotificationRequest(int, notify_request_t *, size_t, int);
static void notify(const char *fmt, ...) {
  notify_request_t req;
  memset(&req, 0, sizeof req);
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(req.message, sizeof req.message - 1, fmt, ap);
  va_end(ap);
  sceKernelSendNotificationRequest(0, &req, sizeof req, 0);
  printf("%s\n", req.message);
}
#endif

static void local_ip(char *out, size_t sz) {
  struct ifaddrs *ifa, *i;
  snprintf(out, sz, "<PS5-IP>");
  if (getifaddrs(&ifa)) return;
  for (i = ifa; i; i = i->ifa_next)
    if (i->ifa_addr && i->ifa_addr->sa_family == AF_INET && !(i->ifa_flags & IFF_LOOPBACK)) {
      inet_ntop(AF_INET, &((struct sockaddr_in *)i->ifa_addr)->sin_addr, out, sz);
      break;
    }
  freeifaddrs(ifa);
}

// ---- HTTP replies ----
static int send_all(int c, const void *b, size_t n) {
  for (const char *p = b; n;) {
    ssize_t k = send(c, p, n, 0);
    if (k <= 0) return -1;
    p += k;
    n -= k;
  }
  return 0;
}
static const char *reason(int code) {
  return code == 200 ? "OK" : code == 206 ? "Partial Content" : code == 400 ? "Bad Request" : code == 403 ? "Forbidden"
         : code == 404 ? "Not Found" : code == 409 ? "Conflict" : code == 413 ? "Payload Too Large"
         : code == 416 ? "Range Not Satisfiable" : "Error";
}
static void reply(int c, int code, const char *type, const char *extra, const void *b, size_t n) {
  char h[768];
  int hl = snprintf(h, sizeof h,
                    "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nCache-Control: no-store\r\n%sConnection: close\r\n\r\n",
                    code, reason(code), type, n, extra ? extra : "");
  if (!send_all(c, h, hl) && n) send_all(c, b, n);
}
static void reply_json(int c, int code, const char *j) { reply(c, code, "application/json", 0, j, strlen(j)); }
static void reply_ok(int c) { reply_json(c, 200, "{\"ok\":true}"); }
static void reply_err(int c, int code, const char *msg) {
  char e[600], o[700];
  fs_json_str(e, sizeof e, msg);
  snprintf(o, sizeof o, "{\"ok\":false,\"error\":%s}", e);
  reply_json(c, code, o);
}
static void reply_rc(int c, int rc) {
  if (rc == -FS_ESYSTEM) { // name the file or folder
    char m[600];
    fs_system_msg(m, sizeof m);
    reply_err(c, 403, m);
  } else if (rc) reply_err(c, 400, fs_err(rc));
  else reply_ok(c);
}

// form / query field: name=value&... (URL decoded into o)
static int field(const char *q, const char *k, char *o, size_t n) {
  size_t kl = strlen(k);
  for (const char *p = q; p && *p;) {
    if (!strncmp(p, k, kl) && p[kl] == '=') {
      p += kl + 1;
      size_t w = 0;
      for (; *p && *p != '&' && w + 1 < n; p++) {
        int h;
        if (*p == '+') o[w++] = ' ';
        else if (*p == '%' && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2]) && sscanf(p + 1, "%2x", &h) == 1) {
          o[w++] = (char)h;
          p += 2;
        } else o[w++] = *p;
      }
      o[w] = 0;
      return 1;
    }
    p = strchr(p, '&');
    if (p) p++;
  }
  if (n) o[0] = 0;
  return 0;
}
static int field_int(const char *q, const char *k, int def) {
  char v[32];
  return field(q, k, v, sizeof v) && v[0] ? atoi(v) : def;
}

static const char *mime(const char *p) {
  static const char *const m[][2] = {
      {"jpg", "image/jpeg"}, {"jpeg", "image/jpeg"}, {"png", "image/png"}, {"gif", "image/gif"},
      {"webp", "image/webp"}, {"bmp", "image/bmp"}, {"svg", "image/svg+xml"}, {"ico", "image/x-icon"},
      {"avif", "image/avif"}, {"mp4", "video/mp4"}, {"m4v", "video/mp4"}, {"webm", "video/webm"},
      {"mov", "video/quicktime"}, {"mkv", "video/x-matroska"}, {"mp3", "audio/mpeg"}, {"m4a", "audio/mp4"},
      {"aac", "audio/aac"}, {"wav", "audio/wav"}, {"ogg", "audio/ogg"}, {"flac", "audio/flac"},
      {"opus", "audio/ogg"}, {"pdf", "application/pdf"}, {0, 0}};
  static const char *const text[] = {"txt", "md", "json", "xml", "ini", "cfg", "conf", "log", "csv", "tsv", "sh",
                                     "c", "h", "cpp", "hpp", "js", "mjs", "css", "html", "htm", "lua", "py", "yml",
                                     "yaml", "toml", "nfo", "srt", "vtt", "ass", "bat", "properties", "lst", 0};
  const char *d = strrchr(p, '.');
  if (!d || strchr(d, '/')) return "application/octet-stream";
  d++;
  for (int i = 0; m[i][0]; i++)
    if (!strcasecmp(d, m[i][0])) return m[i][1];
  for (int i = 0; text[i]; i++)
    if (!strcasecmp(d, text[i])) return "text/plain; charset=utf-8"; // never run as a page
  return "application/octet-stream";
}

// a file with Range support (video seeking, resumable downloads)
static void serve_file(int c, const char *path, const char *range, int dl) {
  int f = open(path, O_RDONLY);
  struct stat st;
  if (f < 0 || fstat(f, &st) || !S_ISREG(st.st_mode)) {
    if (f >= 0) close(f);
    reply_err(c, 404, f < 0 ? fs_err(-errno) : "That isn't a file");
    return;
  }
  off_t size = st.st_size, a = 0, z = size - 1;
  int part = 0;
  if (range && !strncmp(range, "bytes=", 6) && size > 0) {
    const char *r = range + 6;
    if (*r == '-') {
      off_t n = strtoll(r + 1, 0, 10);
      a = n > size ? 0 : size - n;
    } else {
      char *e;
      a = strtoll(r, &e, 10);
      if (*e == '-' && e[1] >= '0' && e[1] <= '9') z = strtoll(e + 1, 0, 10);
    }
    if (z >= size) z = size - 1;
    if (a < 0 || a > z || a >= size) {
      char h[200];
      int l = snprintf(h, sizeof h, "HTTP/1.1 416 Range Not Satisfiable\r\nContent-Range: bytes */%lld\r\nContent-Length: 0\r\nConnection: close\r\n\r\n",
                       (long long)size);
      send_all(c, h, l);
      close(f);
      return;
    }
    part = 1;
  }
  char h[2048], fn[900];
  { // RFC 5987 file name, so any name survives the download
    const char *b = strrchr(path, '/') + 1;
    size_t w = 0;
    for (; *b && w + 4 < sizeof fn; b++) {
      unsigned char ch = *b;
      if (isalnum(ch) || strchr("-._~", ch)) fn[w++] = ch;
      else w += snprintf(fn + w, sizeof fn - w, "%%%02X", ch);
    }
    fn[w] = 0;
  }
  int l = snprintf(h, sizeof h,
                   "HTTP/1.1 %s\r\nContent-Type: %s\r\nAccept-Ranges: bytes\r\nContent-Length: %lld\r\n"
                   "Content-Security-Policy: sandbox\r\nX-Content-Type-Options: nosniff\r\n",
                   part ? "206 Partial Content" : "200 OK", dl ? "application/octet-stream" : mime(path),
                   size ? (long long)(z - a + 1) : 0LL);
  if (part) l += snprintf(h + l, sizeof h - l, "Content-Range: bytes %lld-%lld/%lld\r\n", (long long)a, (long long)z, (long long)size);
  l += snprintf(h + l, sizeof h - l, "Content-Disposition: %s; filename*=UTF-8''%s\r\n", dl ? "attachment" : "inline", fn);
  l += snprintf(h + l, sizeof h - l, "Cache-Control: no-cache\r\nConnection: close\r\n\r\n");
  if (send_all(c, h, l) || !size) {
    close(f);
    return;
  }
  char *io = malloc(1 << 16);
  if (io) {
    lseek(f, a, SEEK_SET);
    for (off_t left = z - a + 1; left > 0;) {
      ssize_t n = read(f, io, left > (1 << 16) ? (1 << 16) : (size_t)left);
      if (n <= 0 || send_all(c, io, n)) break;
      left -= n;
    }
    free(io);
  }
  close(f);
}

// ---- request handling ----
static volatile int quit_pending, rebind;

typedef struct {
  int c;
  char *buf;          // headers + whatever part of the body came with them
  size_t got;         // bytes in buf
  char *body;         // start of the body inside buf
  long long clen;     // Content-Length
} req_t;

static int read_more(req_t *r, size_t cap) { // read the rest of a small body into buf
  size_t have = r->got - (r->body - r->buf), need = (size_t)r->clen;
  size_t off = r->body - r->buf;
  if (off + need + 1 > cap) {
    char *nb = realloc(r->buf, off + need + 1);
    if (!nb) return -1;
    r->buf = nb, r->body = nb + off;
  }
  while (have < need) {
    ssize_t k = recv(r->c, r->body + have, need - have, 0);
    if (k <= 0) return -1;
    have += k;
  }
  r->body[need] = 0;
  r->got = off + need;
  return 0;
}

// streams the request body to a file next to `final`, then renames it into place
static int receive_to(req_t *r, const char *final) {
  char tmp[1300];
  snprintf(tmp, sizeof tmp, "%s.fileden-part", final);
  int fd = fs_create(tmp, 0); // 0777, like everything FileDen writes
  if (fd < 0) return fd;
  size_t inbuf = r->got - (r->body - r->buf);
  long long left = r->clen;
  int rc = 0;
  char *io = malloc(1 << 20);
  if (!io) rc = -ENOMEM;
  if (!rc && inbuf) {
    size_t n = inbuf > (size_t)left ? (size_t)left : inbuf;
    if (write(fd, r->body, n) != (ssize_t)n) rc = -errno;
    left -= n;
  }
  while (!rc && left > 0) {
    ssize_t k = recv(r->c, io, left > (1 << 20) ? (1 << 20) : (size_t)left, 0);
    if (k <= 0) {
      rc = -EIO;
      break;
    }
    for (ssize_t off = 0; off < k;) {
      ssize_t w = write(fd, io + off, k - off);
      if (w <= 0) {
        rc = w < 0 ? -errno : -EIO;
        break;
      }
      off += w;
    }
    left -= k;
  }
  free(io);
  if (close(fd) && !rc) rc = -errno;
  if (!rc && rename(tmp, final)) rc = -errno;
  if (rc) unlink(tmp);
  return rc;
}

// DNS rebinding: a web page can point its own domain name at the console and then talk to
// FileDen as if it were that site. Browsers send the name they used, so only answer to an IP
// address, localhost, or a local name (one word, or .local / .lan / .home / .home.arpa / .internal).
static int host_ok(const req_t *r) {
  const char *h = strcasestr(r->buf, "\r\nHost:");
  if (!h || h > r->body) return 1; // HTTP/1.0 tools and the app may leave it out
  h += 7;
  while (*h == ' ' || *h == '\t') h++;
  if (*h == '[') return 1; // an IPv6 address
  char n[256];
  size_t l = strcspn(h, ":\r\n");
  if (!l || l >= sizeof n) return 0;
  snprintf(n, sizeof n, "%.*s", (int)l, h);
  for (char *c = n; *c; c++) *c = (char)tolower((unsigned char)*c);
  if (n[l - 1] == '.') n[--l] = 0; // "name." is the same name
  struct in_addr ip;
  if (inet_pton(AF_INET, n, &ip) == 1 || !strcmp(n, "localhost") || !strchr(n, '.')) return 1;
  static const char *const local[] = {".local", ".lan", ".home", ".home.arpa", ".internal", ".localhost", 0};
  for (int i = 0; local[i]; i++) {
    size_t k = strlen(local[i]);
    if (l > k && !strcmp(n + l - k, local[i])) return 1;
  }
  return 0;
}
static void handle(req_t *r) {
  int c = r->c;
  char method[8] = "", path[3072] = "";
  sscanf(r->buf, "%7s %3071s", method, path);
  char *q = strchr(path, '?');
  if (q) *q++ = 0;
  else q = "";
  int post = !strcmp(method, "POST") || !strcmp(method, "PUT");
  if (!host_ok(r)) {
    reply_err(c, 403, "FileDen only answers on the console's IP address");
    return;
  }
  // state-changing calls need this header: a page on another site can't add it
  // without a CORS preflight, which this server never approves
  char *xh = strcasestr(r->buf, "\r\nX-FileDen:");
  if (post && (!xh || xh > r->body)) {
    reply_err(c, 403, "Missing X-FileDen header");
    return;
  }
  char *range = 0, *rh = strcasestr(r->buf, "\r\nRange:");
  if (rh && rh < r->body) {
    range = rh + 8;
    while (*range == ' ') range++;
    char *e = strstr(range, "\r\n");
    if (e) *e = 0;
  }
  char a[1200], b[1200], o[3000];
#define FAIL(code, msg) do { reply_err(c, code, msg); return; } while (0)

  if (!strcmp(path, "/") || !strcmp(path, "/index.html")) {
    reply(c, 200, "text/html; charset=utf-8",
          "X-Frame-Options: DENY\r\nContent-Security-Policy: frame-ancestors 'none'\r\nReferrer-Policy: no-referrer\r\nX-Content-Type-Options: nosniff\r\n",
          index_html, index_html_len); // not inside another site's frame (clickjacking)
    return;
  }
  if (!strcmp(path, "/icon.png")) {
    reply(c, 200, "image/png", "Cache-Control: max-age=86400\r\n", logo192_png, logo192_png_len);
    return;
  }
  if (!strcmp(path, "/f400.woff2")) {
    reply(c, 200, "font/woff2", "Cache-Control: max-age=86400\r\n", figtree_400_woff2, figtree_400_woff2_len);
    return;
  }
  if (!strcmp(path, "/f600.woff2")) {
    reply(c, 200, "font/woff2", "Cache-Control: max-age=86400\r\n", figtree_600_woff2, figtree_600_woff2_len);
    return;
  }
  if (!strcmp(path, "/api/ping")) {
    reply_json(c, 200, "{\"app\":\"FileDen\",\"version\":\"" VERSION "\"}");
    return;
  }
  if (!strcmp(path, "/api/settings") && !post) {
    char ip[64];
    local_ip(ip, sizeof ip);
    pthread_mutex_lock(&set_mu);
    snprintf(o, sizeof o, "{\"web\":%d,\"hidden\":%d,\"confirm\":%d,\"grid\":%d,\"sound\":%d,\"intro\":%d,\"sort\":%d,\"theme\":\"%s\",\"ip\":\"%s\",\"port\":%d,\"version\":\"" VERSION "\"}",
             S.web, S.hidden, S.confirm, S.grid, S.sound, S.intro, S.sort, S.theme, ip, PORT);
    pthread_mutex_unlock(&set_mu);
    reply_json(c, 200, o);
    return;
  }
  if (!strcmp(path, "/api/ls")) {
    field(q, "path", a, sizeof a);
    if (!fs_path_ok(a)) FAIL(400, "Bad path");
    int tsv = field_int(q, "tsv", 0), hid = field_int(q, "hidden", S.hidden);
    char *j = fs_list(a, hid, tsv);
    if (!j) FAIL(errno == ENOENT ? 404 : 400, fs_err(-errno));
    reply(c, 200, tsv ? "text/tab-separated-values; charset=utf-8" : "application/json", 0, j, strlen(j));
    free(j);
    return;
  }
  if (!strcmp(path, "/api/places")) {
    char *j = fs_places(field_int(q, "tsv", 0));
    if (!j) FAIL(500, "Out of memory");
    reply(c, 200, "application/json", 0, j, strlen(j));
    free(j);
    return;
  }
  if (!strcmp(path, "/api/file")) {
    field(q, "path", a, sizeof a);
    if (!fs_path_ok(a)) FAIL(400, "Bad path");
    serve_file(c, a, range, field_int(q, "dl", 0));
    return;
  }
  if (!strcmp(path, "/api/favs") && !post) {
    pthread_mutex_lock(&set_mu);
    char *j = favs_list(field_int(q, "tsv", 0));
    pthread_mutex_unlock(&set_mu);
    if (!j) FAIL(500, "Out of memory");
    reply(c, 200, field_int(q, "tsv", 0) ? "text/tab-separated-values; charset=utf-8" : "application/json", 0, j, strlen(j));
    free(j);
    return;
  }
  if (!strcmp(path, "/api/image")) { // ?path=&w=&h=: a JPEG that fits w x h; X-Width / X-Height: its own size
    field(q, "path", a, sizeof a);
    if (!fs_path_ok(a)) FAIL(400, "Bad path");
    unsigned char *jpg;
    size_t jl;
    int iw, ih, rc = img_to_jpeg(a, field_int(q, "w", 1920), field_int(q, "h", 1080), &jpg, &jl, &iw, &ih);
    if (rc) FAIL(rc == -ENOENT ? 404 : rc == -EFBIG ? 413 : 415,
                 rc == -ENOTSUP ? "FileDen can't show this kind of image" : rc == -EFBIG ? "Too big to show" : rc == -ENOENT ? "It isn't there any more" : "Can't decode this image");
    char ex[160];
    snprintf(ex, sizeof ex, "X-Width: %d\r\nX-Height: %d\r\nAccess-Control-Expose-Headers: X-Width, X-Height\r\n", iw, ih);
    reply(c, 200, "image/jpeg", ex, jpg, jl);
    free(jpg);
    return;
  }
  if (!strcmp(path, "/api/info")) {
    field(q, "path", a, sizeof a);
    if (!fs_path_ok(a)) FAIL(400, "Bad path");
    char *j = fs_info(a);
    if (!j) FAIL(404, fs_err(-errno));
    reply_json(c, 200, j);
    free(j);
    return;
  }
  if (!strcmp(path, "/api/job")) {
    fs_job_json(o, sizeof o);
    reply_json(c, 200, o);
    return;
  }
  if (!strcmp(path, "/api/undo") && !post) { // what Undo would do
    char w[900], e[1900];
    int k = fs_undo_info(w, sizeof w);
    fs_json_str(e, sizeof e, w);
    snprintf(o, sizeof o, "{\"can\":%d,\"what\":%s}", k ? 1 : 0, e);
    reply_json(c, 200, o);
    return;
  }
  if (!post) {
    reply_err(c, 404, "Not found");
    return;
  }

  // ---- POST ----
  if (!strcmp(path, "/api/upload")) { // raw body; ?path=/folder/name&overwrite=0|1
    field(q, "path", a, sizeof a);
    if (!fs_path_ok(a) || !strcmp(a, "/")) FAIL(400, "Bad path");
    char dir[1200], *s = strrchr(a, '/');
    snprintf(dir, sizeof dir, "%.*s", s == a ? 1 : (int)(s - a), a);
    if (!fs_name_ok(s + 1)) FAIL(400, "Bad file name");
    int rc = fs_guard_write(dir);
    if (rc) {
      reply_rc(c, rc);
      return;
    }
    if (field_int(q, "overwrite", 0)) {
      snprintf(b, sizeof b, "%s", a);
      struct stat st;
      if (!lstat(a, &st)) rc = fs_guard_entry(a);
    } else rc = fs_unique(dir, s + 1, b, sizeof b);
    if (!rc) rc = receive_to(r, b);
    if (rc) {
      reply_rc(c, rc);
      return;
    }
    char e[2500];
    fs_json_str(e, sizeof e, b);
    snprintf(o, sizeof o, "{\"ok\":true,\"path\":%s}", e);
    reply_json(c, 200, o);
    return;
  }
  if (!strcmp(path, "/api/save")) { // raw body = the new file contents
    field(q, "path", a, sizeof a);
    if (!fs_path_ok(a)) FAIL(400, "Bad path");
    if (r->clen > MAX_SAVE) FAIL(413, "Too big for the editor (16 MB)");
    if (read_more(r, r->got)) FAIL(400, "Upload interrupted");
    reply_rc(c, fs_save(a, r->body, (size_t)r->clen));
    return;
  }
  if (r->clen > MAX_FORM) FAIL(413, "Request too big");
  if (read_more(r, r->got)) return;
  const char *in = r->body;

  if (!strcmp(path, "/api/mkdir")) {
    field(in, "path", a, sizeof a);
    reply_rc(c, fs_mkdir(a));
  } else if (!strcmp(path, "/api/newfile")) {
    field(in, "path", a, sizeof a);
    reply_rc(c, fs_newfile(a));
  } else if (!strcmp(path, "/api/undo")) { // put back the last rename or move
    int rc = fs_undo();
    if (rc < 0) {
      reply_rc(c, rc);
      return;
    }
    reply_json(c, 200, rc == 1 ? "{\"ok\":true,\"job\":true}" : "{\"ok\":true,\"job\":false}");
  } else if (!strcmp(path, "/api/rename")) {
    field(in, "path", a, sizeof a);
    field(in, "name", b, sizeof b);
    char out[1300], e[2700];
    int rc = fs_rename(a, b, out, sizeof out);
    if (rc) {
      reply_rc(c, rc);
      return;
    }
    fs_json_str(e, sizeof e, out);
    snprintf(o, sizeof o, "{\"ok\":true,\"path\":%s}", e);
    reply_json(c, 200, o);
  } else if (!strcmp(path, "/api/copy") || !strcmp(path, "/api/move") || !strcmp(path, "/api/delete")) {
    int op = path[5] == 'c' ? JOB_COPY : path[5] == 'm' ? JOB_MOVE : JOB_DELETE;
    size_t pl = strlen(in) + 1;
    char *paths = malloc(pl);
    if (!paths) FAIL(500, "Out of memory");
    field(in, "paths", paths, pl);
    field(in, "dest", a, sizeof a);
    int rc = fs_job_start(op, paths, a, 0);
    free(paths);
    reply_rc(c, rc);
  } else if (!strcmp(path, "/api/zip") || !strcmp(path, "/api/7z") || !strcmp(path, "/api/tar")) {
    // paths, dest folder, name; for tar: z = gz, xz, bz2, zst or empty
    size_t pl = strlen(in) + 1;
    char *paths = malloc(pl), nm[600], full[1900], z[8];
    if (!paths) FAIL(500, "Out of memory");
    field(in, "paths", paths, pl);
    field(in, "dest", a, sizeof a);
    field(in, "name", nm, sizeof nm);
    field(in, "z", z, sizeof z);
    int tf = !z[0] ? TF_NONE : !strcmp(z, "gz") ? TF_GZ : !strcmp(z, "xz") ? TF_XZ : !strcmp(z, "bz2") ? TF_BZ2 : !strcmp(z, "zst") ? TF_ZST : -1;
    int op = path[5] == 'z' ? JOB_ZIP : path[5] == '7' ? JOB_7Z : JOB_TAR;
    char pw[256];
    field(in, "pw", pw, sizeof pw); // a password: ZIP (AES-256) and 7z only
    int rc = fs_name_ok(nm) && (op != JOB_TAR || tf >= 0) && (!pw[0] || op != JOB_TAR) ? 0 : -EINVAL;
    if (!rc) snprintf(full, sizeof full, "%s%s%s", a, strcmp(a, "/") ? "/" : "", nm);
    if (!rc) rc = fs_job_start_pw(op, paths, full, op == JOB_TAR ? tf : 0, pw);
    free(paths);
    reply_rc(c, rc);
  } else if (!strcmp(path, "/api/extract")) { // path (archive), dest folder: unpacks into dest/<archive name>
    char nm[600], full[1900];
    field(in, "path", a, sizeof a);
    field(in, "dest", b, sizeof b);
    if (ar_out_name(a, nm, sizeof nm)) nm[0] = 0;
    if (!b[0]) { // default: next to the archive
      snprintf(b, sizeof b, "%s", a);
      char *sl = strrchr(b, '/');
      if (sl == b) b[1] = 0;
      else if (sl) *sl = 0;
    }
    snprintf(full, sizeof full, "%s%s%s", b, strcmp(b, "/") ? "/" : "", nm[0] ? nm : "extracted");
    char pw[256];
    field(in, "pw", pw, sizeof pw);
    reply_rc(c, fs_job_start_pw(JOB_EXTRACT, a, full, 0, pw));
  } else if (!strcmp(path, "/api/hash") || !strcmp(path, "/api/size")) {
    field(in, "path", a, sizeof a);
    reply_rc(c, fs_job_start(path[5] == 'h' ? JOB_HASH : JOB_SIZE, a, "", 0));
  } else if (!strcmp(path, "/api/chmod")) { // path, mode (octal), recursive=1 for a folder and all inside
    char m[16];
    field(in, "path", a, sizeof a);
    field(in, "mode", m, sizeof m);
    char *end;
    long mode = strtol(m, &end, 8);
    if (!m[0] || *end || mode < 0 || mode > 07777) FAIL(400, "Bad mode");
    struct stat st;
    int rec = field_int(in, "recursive", 0) && !lstat(a, &st) && S_ISDIR(st.st_mode);
    int rc = rec ? fs_job_start(JOB_CHMOD, a, "", (int)mode) : fs_chmod(a, (int)mode);
    if (rc) {
      reply_rc(c, rc);
      return;
    }
    reply_json(c, 200, rec ? "{\"ok\":true,\"job\":true}" : "{\"ok\":true,\"job\":false}");
  } else if (!strcmp(path, "/api/fav")) { // path, on=1 add / 0 remove
    field(in, "path", a, sizeof a);
    if (!fs_path_ok(a)) FAIL(400, "Bad path");
    pthread_mutex_lock(&set_mu);
    int rc = fav_set(a, field_int(in, "on", 1));
    pthread_mutex_unlock(&set_mu);
    if (rc == -ENOSPC) FAIL(400, "Favourites are full (64)");
    reply_rc(c, rc);
  } else if (!strcmp(path, "/api/cancel")) { // all=1: the queue too
    if (field_int(in, "all", 0)) fs_job_cancel_all();
    else fs_job_cancel();
    reply_ok(c);
  } else if (!strcmp(path, "/api/settings")) {
    pthread_mutex_lock(&set_mu);
    int was = S.web;
    S.web = field_int(in, "web", S.web) ? 1 : 0;
    S.hidden = field_int(in, "hidden", S.hidden) ? 1 : 0;
    S.confirm = field_int(in, "confirm", S.confirm) ? 1 : 0;
    S.grid = field_int(in, "grid", S.grid) ? 1 : 0;
    S.sound = field_int(in, "sound", S.sound) ? 1 : 0;
    S.intro = field_int(in, "intro", S.intro) ? 1 : 0;
    S.sort = field_int(in, "sort", S.sort) & 3;
    char th[24] = "";
    field(in, "theme", th, sizeof th);
    if (th[0] && strspn(th, "abcdefghijklmnopqrstuvwxyz-") == strlen(th)) snprintf(S.theme, sizeof S.theme, "%s", th);
    settings_save();
    if (was != S.web) rebind = 1;
    pthread_mutex_unlock(&set_mu);
    reply_ok(c);
  } else if (!strcmp(path, "/api/quit")) { // the app closed: stop unless the web server should stay
    int force = field_int(in, "force", 0);
    if (force || !S.web) quit_pending = 1;
    reply_json(c, 200, quit_pending ? "{\"ok\":true,\"quitting\":true}" : "{\"ok\":true,\"quitting\":false}");
  } else
    reply_err(c, 404, "Not found");
}

static int nclients; // connections being served (each has a thread)
#define MAX_CLIENTS 48
static void *client(void *arg) {
  req_t r = {(int)(long)arg};
  struct timeval tv = {60, 0}; // a client that stops sending (or reading) for a minute is dropped
  setsockopt(r.c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  setsockopt(r.c, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
  size_t cap = 65536;
  r.buf = malloc(cap + 1);
  while (r.buf) { // headers (and maybe the start of the body)
    ssize_t k = recv(r.c, r.buf + r.got, cap - r.got, 0);
    if (k <= 0) break;
    r.got += k;
    r.buf[r.got] = 0;
    char *e = strstr(r.buf, "\r\n\r\n");
    if (!e) {
      if (r.got == cap) break;
      continue;
    }
    char *cl = strcasestr(r.buf, "\r\nContent-Length:");
    r.clen = cl && cl < e ? atoll(cl + 17) : 0;
    if (r.clen < 0) r.clen = 0;
    r.body = e + 4;
    break;
  }
  if (r.buf && r.body) handle(&r);
  free(r.buf);
  close(r.c);
  __atomic_sub_fetch(&nclients, 1, __ATOMIC_RELAXED);
  return 0;
}

static int open_listener(int lan) {
  int s = socket(AF_INET, SOCK_STREAM, 0);
  if (s < 0) return -1;
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &(int){1}, sizeof(int));
  struct sockaddr_in a;
  memset(&a, 0, sizeof a);
  a.sin_family = AF_INET;
  a.sin_port = htons(PORT);
  a.sin_addr.s_addr = htonl(lan ? INADDR_ANY : INADDR_LOOPBACK);
  if (bind(s, (struct sockaddr *)&a, sizeof a) || listen(s, 32)) {
    close(s);
    return -1;
  }
  return s;
}
static int already_running(void) {
  int s = socket(AF_INET, SOCK_STREAM, 0);
  if (s < 0) return 0;
  struct sockaddr_in a = {0};
  a.sin_family = AF_INET;
  a.sin_port = htons(PORT);
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  int up = !connect(s, (struct sockaddr *)&a, sizeof a);
  close(s);
  return up;
}
static int started_by_app(void) {
  char p[300];
  struct stat st;
  snprintf(p, sizeof p, "%s/from_app", DATA);
  if (stat(p, &st)) return 0;
  unlink(p);
  return time(0) - st.st_mtime < 120; // ignore one left over from a failed start
}

int main(void) {
#ifdef SYS_thr_set_name
  syscall(SYS_thr_set_name, -1, "FileDen"); // shows up by name in process lists
#endif
  signal(SIGPIPE, SIG_IGN);
  if (getenv("FILEDEN_DATA")) snprintf(DATA, sizeof DATA, "%s", getenv("FILEDEN_DATA"));
  mkdir(DATA, 0777);
  settings_load();
  favs_load();
  if (already_running()) {
    notify("FileDen: port %d busy (already running?)", PORT);
    return 1;
  }
  int lan = S.web, s = open_listener(lan);
  if (s < 0) {
    notify("FileDen: port %d busy (already running?)", PORT);
    return 1;
  }
  char ip[64];
  local_ip(ip, sizeof ip);
  if (started_by_app()) {
    if (lan) notify("FileDen v" VERSION "\nAlso on http://%s:%d/", ip, PORT);
  } else if (lan)
    notify("FileDen v" VERSION " running\nOpen http://%s:%d/ on your phone or PC", ip, PORT);
  else
    notify("FileDen v" VERSION " running\nThe web server is off: turn it on in the FileDen app");

  int fails = 0;
  for (;;) {
    if (rebind) { // web server setting changed: listen on the network or on 127.0.0.1 only
      rebind = 0;
      int want = S.web, ns;
      if (want != lan) {
        close(s);
        for (int i = 0; (ns = open_listener(want)) < 0 && i < 50; i++) usleep(100000);
        if (ns < 0) ns = open_listener(lan); // put the old one back
        else lan = want;
        s = ns;
        if (lan) notify("FileDen web server on\nhttp://%s:%d/", ip, PORT);
      }
    }
    if (quit_pending && !fs_job_running()) {
      usleep(300000); // let the last reply go out
      close(s);
      return 0;
    }
    struct pollfd pf = {s, POLLIN, 0};
    int pr = s >= 0 ? poll(&pf, 1, 400) : (usleep(400000), 0);
    if (pr <= 0) {
      if (s < 0) s = open_listener(lan);
      continue;
    }
    struct sockaddr_in peer;
    socklen_t pl = sizeof peer;
    int c = accept(s, (struct sockaddr *)&peer, &pl);
    if (c < 0) { // the socket can die across rest mode: rebuild it
      if (++fails >= 5) {
        close(s);
        while ((s = open_listener(lan)) < 0) sleep(2);
        fails = 0;
      } else usleep(200000);
      continue;
    }
    fails = 0;
#ifdef SO_NOSIGPIPE
    setsockopt(c, SOL_SOCKET, SO_NOSIGPIPE, &(int){1}, sizeof(int));
#endif
    int local = pl >= sizeof peer && peer.sin_family == AF_INET && peer.sin_addr.s_addr == htonl(INADDR_LOOPBACK);
    if (__atomic_add_fetch(&nclients, 1, __ATOMIC_RELAXED) > MAX_CLIENTS && !local) { // too many at once (the app is always let in)
      __atomic_sub_fetch(&nclients, 1, __ATOMIC_RELAXED);
      static const char busy[] = "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\nRetry-After: 1\r\nConnection: close\r\n\r\n";
      send(c, busy, sizeof busy - 1, 0);
      close(c);
      continue;
    }
    pthread_t t;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 1 << 20);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &at, client, (void *)(long)c)) close(c), __atomic_sub_fetch(&nclients, 1, __ATOMIC_RELAXED);
    pthread_attr_destroy(&at);
  }
}
