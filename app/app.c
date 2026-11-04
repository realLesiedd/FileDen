// FileDen - full screen app (fake-signed): a controller file explorer for the TV.
// Starts the payload if needed and does every file operation through its API on
// 127.0.0.1:9923 (the app's sandbox can't be assumed to see the whole file
// system). Browse, select, copy / cut / paste, rename, delete, new folder and
// file, an image viewer, a line-based text editor, and Settings with the web
// server switch. Drawing, glyphs, splash, hold-to-close and the VideoOut / pad
// front end are in ui_core.h.
// SPDX-License-Identifier: GPL-3.0-or-later
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdarg.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "ui_core.h"
#include "logo.h"
#include "../version.h"

#define PORT 9923
#define DATA "/data/fileden"
#define LOG_PATH DATA "/app.log"
#define MAX_IMAGE_BYTES (40u << 20)
#define MAX_IMAGE_PIXELS (24u << 20)
#define MAX_TEXT_BYTES (4u << 20)
#define MAX_LINE 4000 // the line editor's keyboard holds 4095 bytes

static unsigned *bgbuf, *logo;
static int logo_n;
static char toast[300];
static int toast_err;
static unsigned long long now_ms, toast_until;
static void say(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(toast, sizeof toast, fmt, ap);
  va_end(ap);
  toast_err = 0;
  toast_until = now_ms + 3500;
}
// ---- sound: soft menu effects, mixed on their own thread into sceAudioOut ----
enum { SFX_MOVE, SFX_SELECT, SFX_BACK, SFX_MENU, SFX_ERROR, NSFX };
static volatile int snd_mode = 1; // Settings > Sounds: 1 on, 0 off
#include <pthread.h>
#ifdef HOST_PREVIEW
static void sfx(int id) { (void)id; }
static void snd_start(void) {}
#else
int sceAudioOutInit(void);
int sceAudioOutOpen(int, int, int, unsigned, unsigned, unsigned);
int sceAudioOutOutput(int, const void *);
#define SR 48000
#define BLK 256
static short *sfx_pcm[NSFX];
static int sfx_len[NSFX];
static volatile unsigned sfx_seq; // bumped for each effect; the mixer picks it up
static volatile int sfx_id;
static unsigned long long sfx_last[NSFX];
static void sfx(int id) {
  if (snd_mode < 1 || !sfx_pcm[id]) return;
  if (id == SFX_MOVE && now_ms - sfx_last[id] < 45) return; // held d-pad: not a buzz
  sfx_last[id] = now_ms;
  sfx_id = id;
  __atomic_add_fetch(&sfx_seq, 1, __ATOMIC_RELEASE);
}
static void sfx_make(void) { // soft synthesised blips, made once at start
  static const struct { float f0, f1, dur, decay, gain, noise; } D[NSFX] = {
      {1480, 1480, .030f, 150, .22f, 0},  // move: a light tick
      {880, 1320, .110f, 30, .26f, 0},    // select: rising
      {1320, 784, .110f, 30, .22f, 0},    // back: falling
      {520, 1040, .090f, 35, .16f, .25f}, // menu: a soft swish up
      {220, 196, .200f, 14, .26f, 0},     // error: low
  };
  unsigned rnd = 1;
  for (int i = 0; i < NSFX; i++) {
    int n = (int)(D[i].dur * SR);
    short *p = malloc((size_t)n * sizeof *p);
    if (!p) continue;
    double ph = 0;
    for (int k = 0; k < n; k++) {
      float t = (float)k / SR, u = (float)k / n, f = D[i].f0 + (D[i].f1 - D[i].f0) * (u < .5f ? 0 : 1);
      if (i == SFX_MENU) f = D[i].f0 + (D[i].f1 - D[i].f0) * u;
      ph += 6.283185307 * f / SR;
      float e = expf(-t * D[i].decay) * fminf(1, t * 900); // quick attack, no click
      if (i == SFX_ERROR) e *= (u < .45f || u > .55f) ? 1 : 0; // two short buzzes
      rnd = rnd * 1103515245u + 12345u;
      float v = (sinf((float)ph) + .25f * sinf(2 * (float)ph)) * (1 - D[i].noise) + D[i].noise * ((rnd >> 16) / 32768.f - 1);
      p[k] = (short)(v * e * D[i].gain * 32767);
    }
    sfx_pcm[i] = p, sfx_len[i] = n;
  }
}
static void *snd_thread(void *u) {
  (void)u;
  sceAudioOutInit();
  int port = sceAudioOutOpen(0xff, 0, 0, BLK, SR, 1); // system user, main output, 16-bit stereo
  if (port < 0) return 0;
  static short out[BLK * 2];
  struct { int id, pos; } v[6] = {{-1, 0}, {-1, 0}, {-1, 0}, {-1, 0}, {-1, 0}, {-1, 0}};
  unsigned seen = sfx_seq;
  for (;;) {
    unsigned q = __atomic_load_n(&sfx_seq, __ATOMIC_ACQUIRE);
    if (q != seen) {
      seen = q;
      int id = sfx_id, k = 0;
      while (k < 5 && v[k].id >= 0) k++;
      v[k].id = id, v[k].pos = 0;
    }
    for (int i = 0; i < BLK; i++) {
      float l = 0, r = 0;
      for (int k = 0; k < 6; k++)
        if (v[k].id >= 0) {
          float x = sfx_pcm[v[k].id][v[k].pos];
          l += x, r += x;
          if (++v[k].pos >= sfx_len[v[k].id]) v[k].id = -1;
        }
      out[2 * i] = (short)(l > 32767 ? 32767 : l < -32768 ? -32768 : l);
      out[2 * i + 1] = (short)(r > 32767 ? 32767 : r < -32768 ? -32768 : r);
    }
    sceAudioOutOutput(port, out); // blocks for one block: paces this loop
  }
  return 0;
}
static void snd_start(void) {
  sfx_make();
  pthread_t t;
  pthread_attr_t a;
  pthread_attr_init(&a);
  pthread_attr_setstacksize(&a, 256 << 10);
  pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
  pthread_create(&t, &a, snd_thread, 0);
  pthread_attr_destroy(&a);
}
#endif
static void say_err(const char *m) {
  say("%s", m);
  toast_err = 1;
  sfx(SFX_ERROR);
}

// ---- HTTP client to the payload on 127.0.0.1 ----
typedef struct {
  int status;
  char *body; // NUL-terminated, may contain NULs for binary files
  size_t len;
  int xw, xh; // X-Width / X-Height (an /api/image picture's own size)
} resp_t;
static void resp_free(resp_t *r) {
  free(r->body);
  r->body = 0, r->len = 0;
}
static int connect_local(void) {
  int s = socket(AF_INET, SOCK_STREAM, 0);
  if (s < 0) return -1;
  struct sockaddr_in a = {0};
  a.sin_family = AF_INET;
  a.sin_port = htons(PORT);
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (connect(s, (struct sockaddr *)&a, sizeof a)) {
    close(s);
    return -1;
  }
  return s;
}
// case-insensitive strstr. The app runtime has no strcasestr (it resolves to address 0
// and crashes the app), so it lives here.
static char *ci_find(const char *h, const char *n) {
  size_t nl = strlen(n);
  for (; *h; h++)
    if (!strncasecmp(h, n, nl)) return (char *)h;
  return 0;
}
// returns 0 with r filled (any HTTP status), -1 no connection, -2 bigger than maxlen
static int http_now(const char *method, const char *path, const void *body, size_t blen, int raw, size_t maxlen, resp_t *r) {
  memset(r, 0, sizeof *r);
  int s = connect_local();
  if (s < 0) return -1;
  char req[3600];
  int n = snprintf(req, sizeof req, "%s %s HTTP/1.0\r\nHost: 127.0.0.1\r\nX-FileDen: 1\r\nContent-Type: %s\r\nContent-Length: %zu\r\n\r\n",
                   method, path, raw ? "application/octet-stream" : "application/x-www-form-urlencoded", blen);
  if (n >= (int)sizeof req || write(s, req, n) != n) {
    close(s);
    return -1;
  }
  for (const char *p = body; blen;) {
    ssize_t w = write(s, p, blen);
    if (w <= 0) {
      close(s);
      return -1;
    }
    p += w, blen -= w;
  }
  char hb[8192];
  size_t got = 0;
  char *he = 0;
  while (!he && got < sizeof hb - 1) {
    ssize_t k = read(s, hb + got, sizeof hb - 1 - got);
    if (k <= 0) break;
    got += k;
    hb[got] = 0;
    he = strstr(hb, "\r\n\r\n");
  }
  if (!he) {
    close(s);
    return -1;
  }
  sscanf(hb, "HTTP/%*s %d", &r->status);
  long long cl = -1;
  char *c = ci_find(hb, "\r\nContent-Length:");
  if (c && c < he) cl = atoll(c + 17);
  if ((c = ci_find(hb, "\r\nX-Width:")) && c < he) r->xw = atoi(c + 10);
  if ((c = ci_find(hb, "\r\nX-Height:")) && c < he) r->xh = atoi(c + 11);
  if (cl >= 0 && (size_t)cl > maxlen) {
    close(s);
    return -2;
  }
  size_t cap = cl >= 0 ? (size_t)cl + 1 : 65536, have = got - (he + 4 - hb);
  if (have + 1 > cap) cap = have + 1;
  r->body = malloc(cap);
  if (!r->body) {
    close(s);
    return -1;
  }
  memcpy(r->body, he + 4, have);
  for (;;) {
    if (cl >= 0 && have >= (size_t)cl) break;
    if (have + 1 >= cap) {
      if (cap > maxlen) {
        resp_free(r);
        close(s);
        return -2;
      }
      char *nb = realloc(r->body, cap *= 2);
      if (!nb) {
        resp_free(r);
        close(s);
        return -1;
      }
      r->body = nb;
    }
    ssize_t k = read(s, r->body + have, cap - 1 - have);
    if (k <= 0) break;
    have += k;
  }
  close(s);
  r->body[have] = 0;
  r->len = have;
  return 0;
}
// The same request, from the screen's thread: it runs on a thread of its own while frames keep being
// drawn (a "Working" chip after a moment), so a busy payload never freezes the picture. Buttons pressed
// meanwhile are kept for afterwards. Other threads, and requests made while one is waiting, go straight.
static pthread_t ui_thread;
static int ui_ready, http_waiting;
static unsigned long long busy_since;
static unsigned pending_down; // presses made while a request was waiting
typedef struct {
  const char *method, *path;
  const void *body;
  size_t blen, maxlen;
  int raw, rc;
  resp_t r;
  int done;
} hreq_t;
static void *hreq_main(void *u) {
  hreq_t *q = u;
#ifdef HOST_PREVIEW
  if (getenv("FD_SLOW")) usleep(atoi(getenv("FD_SLOW")) * 1000); // PC preview: a slow payload
#endif
  q->rc = http_now(q->method, q->path, q->body, q->blen, q->raw, q->maxlen, &q->r);
  __atomic_store_n(&q->done, 1, __ATOMIC_RELEASE); // q lives on the waiting caller's stack: nothing after this
  return 0;
}
static void frame(void);
static unsigned pad_buttons(void);
static unsigned long long clock_ms(void) { return sceKernelGetProcessTime() / 1000; }
static int http(const char *method, const char *path, const void *body, size_t blen, int raw, size_t maxlen, resp_t *r) {
  if (!ui_ready || http_waiting || !pthread_equal(pthread_self(), ui_thread)) return http_now(method, path, body, blen, raw, maxlen, r);
  hreq_t q = {method, path, body, blen, maxlen, raw, 0, {0}, 0};
  pthread_t t;
  pthread_attr_t a;
  pthread_attr_init(&a);
  pthread_attr_setstacksize(&a, 256 << 10);
  pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
  int e = pthread_create(&t, &a, hreq_main, &q);
  pthread_attr_destroy(&a);
  if (e) return http_now(method, path, body, blen, raw, maxlen, r);
  http_waiting = 1;
  unsigned long long t0 = clock_ms();
  unsigned held = pad_buttons();
  while (!__atomic_load_n(&q.done, __ATOMIC_ACQUIRE)) {
    if (clock_ms() - t0 < 40) { // most answers take a few milliseconds: no extra frame for those
      usleep(1000);
      continue;
    }
    now_ms = clock_ms(), busy_since = t0;
    unsigned b = pad_buttons();
    pending_down |= b & ~held & ~(unsigned)PAD_CIRCLE; // O stays with the hold-to-close logic
    held = b;
    frame();
  }
  http_waiting = 0, busy_since = 0;
  *r = q.r;
  return q.rc;
}
static void urlenc(const char *s, char *o, size_t n) {
  size_t k = 0;
  for (; *s && k + 4 < n; s++) {
    unsigned char c = *s;
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || strchr("-_.~/", c)) o[k++] = c;
    else k += snprintf(o + k, n - k, "%%%02X", c);
  }
  o[k] = 0;
}
// tiny JSON readers for the flat objects the payload sends
static const char *jget(const char *o, const char *k) {
  char pat[48];
  snprintf(pat, sizeof pat, "\"%s\":", k);
  const char *p = strstr(o, pat);
  return p ? p + strlen(pat) : 0;
}
static void jstr(const char *o, const char *k, char *out, size_t n) {
  const char *p = jget(o, k);
  size_t w = 0;
  if (p && *p == '"')
    for (p++; *p && *p != '"' && w + 1 < n; p++) {
      if (*p == '\\' && p[1]) {
        p++;
        if (*p == 'u') { // \u00XX control characters only
          unsigned v = 0;
          sscanf(p + 1, "%4x", &v);
          p += 4;
          out[w++] = v < 128 ? (char)v : '?';
          continue;
        }
        out[w++] = *p == 'n' ? '\n' : *p == 't' ? '\t' : *p;
        continue;
      }
      out[w++] = *p;
    }
  out[w] = 0;
}
static long long jnum(const char *o, const char *k) {
  const char *p = jget(o, k);
  return p ? (*p == 't' ? 1 : atoll(p)) : 0;
}
// POST a form; on failure shows the payload's error and returns -1
static int api_post(const char *path, const char *form, char *out, size_t on) {
  resp_t r;
  int rc = http("POST", path, form, strlen(form), 0, 1 << 20, &r);
  if (rc) {
    say_err("The background service isn't answering");
    return -1;
  }
  int ok = r.status == 200 && r.body && strstr(r.body, "\"ok\":true");
  if (!ok) {
    char e[300] = "";
    if (r.body) jstr(r.body, "error", e, sizeof e);
    say_err(e[0] ? e : "That didn't work");
  } else if (out)
    snprintf(out, on, "%s", r.body);
  resp_free(&r);
  return ok ? 0 : -1;
}

// ---- state ----
typedef struct {
  char *name;
  char type; // d folder, D link to folder, f file, L link
  long long size, mtime;
  int mark, lock; // lock: a PS5 system item FileDen won't change
  char *tid, *tname; // a game, app or package: its title ID and name (or 0)
  int ic;            // ... and it has an icon to show instead of the folder / file one
} item_t;
typedef struct {
  unsigned *px; // TW x TH, cropped to fill (or IC x IC for a game / app icon)
  int state;    // 0 not yet, 1 ready, 2 none, 3 on its way
  int sq;       // a square icon
} thumb_t;
#define IC 120 // game / app icons, kept square
static thumb_t *thumbs;
static int nthumbs;
static item_t *items;
static int nitems, icap, sel, list_err;
static char cwd[1024] = "/data", list_msg[300];
typedef struct {
  char path[64], label[40];
  long long fr, tot;
} place_t;
static place_t places[16];
static int nplaces;
static struct {
  int web, hidden, confirm, grid, intro, sort;
  char ip[64];
  int ok, fails; // fails: settings reads in a row that got no answer
} set = {1, 0, 1, 0, 0, 0, "", 0, 0};
// favourites (kept by the payload in /data/fileden/favourites.txt, shared with the web UI)
typedef struct {
  char path[1024];
  int dir, gone;
} fav_t;
static fav_t favs[64];
static int nfavs, fsel;
static char **clip;
static int nclip, clip_move;
static char clip_dir[1024];
static int nmarked;

static int isdir(const item_t *it) { return it->type == 'd' || it->type == 'D'; }
static const char *ext_of(const char *n) {
  const char *d = strrchr(n, '.');
  return d && d != n ? d + 1 : "";
}
static int ext_in(const char *n, const char *const *l) {
  const char *e = ext_of(n);
  for (int i = 0; l[i]; i++)
    if (!strcasecmp(e, l[i])) return 1;
  return 0;
}
// the payload turns these into JPEGs for the viewer and the grid (/api/image)
static const char *const IMG_EXT[] = {"jpg", "jpeg", "jpe", "jfif", "png", "bmp", "gif", "tga", "psd", "hdr", "pnm", "ppm",
                                      "pgm", "webp", "jxr", "wdp", "hdp", 0};
static const char *const IMG_WEB[] = {"svg", "avif", "heic", "ico", 0}; // shown only in a browser
static const char *const TXT_EXT[] = {"txt", "md", "json", "xml", "ini", "cfg", "conf", "log", "csv", "tsv", "sh", "c", "h",
                                      "cpp", "hpp", "js", "mjs", "css", "html", "htm", "lua", "py", "yml", "yaml", "toml",
                                      "nfo", "srt", "vtt", "ass", "bat", "properties", "lst", "env", 0};
static const char *const VID_EXT[] = {"mp4", "m4v", "webm", "mov", "mkv", "mp3", "m4a", "aac", "wav", "ogg", "flac", 0};
static const char *const ARC_EXT[] = {"zip", "zipx", "7z", "rar", "tar", "gz", "tgz", "gzip", "bz2", "bzip2", "tbz", "tbz2",
                                      "xz", "txz", "zst", "tzst", "lzma", "tlz", "z", "taz", "cab", "lzh", "lha", "cpio",
                                      "iso", "ar", "deb", "xar", 0}; // FileDen unpacks these
static const char *const PKG_EXT[] = {"pkg", "elf", "bin", "self", "sprx", "prx", "exfat", "ffpkg", "ffpfs", 0};
static int split_part(const char *n) { // name.001, name.r00: parts of a split archive
  const char *d = strrchr(n, '.');
  if (!d || strlen(d) != 4) return 0;
  int digits = (d[1] >= '0' && d[1] <= '9') + (d[2] >= '0' && d[2] <= '9') + (d[3] >= '0' && d[3] <= '9');
  return digits == 3 || ((d[1] == 'r' || d[1] == 'R') && digits == 2);
}
enum { K_DIR, K_FILE, K_IMG, K_TXT, K_VID, K_PKG, K_ARC };
static int kind_of(const item_t *it) {
  if (isdir(it)) return K_DIR;
  if (ext_in(it->name, IMG_EXT) || ext_in(it->name, IMG_WEB)) return K_IMG;
  if (ext_in(it->name, TXT_EXT) || (!strchr(it->name, '.') && it->size >= 0 && it->size < 65536)) return K_TXT;
  if (ext_in(it->name, VID_EXT)) return K_VID;
  if (ext_in(it->name, ARC_EXT) || split_part(it->name)) return K_ARC;
  if (ext_in(it->name, PKG_EXT)) return K_PKG;
  return K_FILE;
}
static void join(char *o, size_t n, const char *dir, const char *name) {
  snprintf(o, n, "%s%s%s", dir, strcmp(dir, "/") ? "/" : "", name);
}
static void parent_of(const char *p, char *o, size_t n) {
  snprintf(o, n, "%s", p);
  char *s = strrchr(o, '/');
  if (s == o) o[1] = 0;
  else if (s) *s = 0;
}
static const char *base_of(const char *p) {
  const char *s = strrchr(p, '/');
  return s && s[1] ? s + 1 : p;
}
static const char *human(long long b, char *o, size_t n) {
  const char *u[] = {"B", "KB", "MB", "GB", "TB"};
  double v = b;
  int i = 0;
  if (b < 0) return snprintf(o, n, "\x81"), o;
  while (v >= 1024 && i < 4) v /= 1024, i++;
  snprintf(o, n, i ? (v < 10 ? "%.2f %s" : "%.1f %s") : "%.0f %s", v, u[i]);
  return o;
}
static const char *when(long long t, char *o, size_t n) {
  if (t <= 0) return snprintf(o, n, " "), o;
  time_t tt = (time_t)t;
  struct tm *tp = localtime(&tt), tm; // the app runtime's libc has no localtime_r (one thread draws)
  if (!tp) return snprintf(o, n, " "), o;
  tm = *tp;
  static const char *const M[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  snprintf(o, n, "%d %s %d, %02d:%02d", tm.tm_mday, M[tm.tm_mon], tm.tm_year + 1900, tm.tm_hour, tm.tm_min);
  return o;
}

// ---- loading ----
static void free_items(void) {
  for (int i = 0; i < nitems; i++) free(items[i].name), free(items[i].tid), free(items[i].tname);
  nitems = 0, nmarked = 0;
}
static void tsv_unesc(char *s) {
  char *w = s;
  for (; *s; s++) {
    if (*s == '\\' && s[1]) {
      s++;
      *w++ = *s == 't' ? '\t' : *s == 'n' ? '\n' : *s == 'r' ? '\r' : *s;
    } else *w++ = *s;
  }
  *w = 0;
}
static int load_places(void) {
  resp_t r;
  if (http("GET", "/api/places?tsv=1", 0, 0, 0, 1 << 16, &r) || r.status != 200) {
    resp_free(&r);
    return -1;
  }
  nplaces = 0;
  for (char *l = r.body; l && *l && nplaces < 16;) {
    char *e = strchr(l, '\n');
    if (e) *e = 0;
    place_t *p = &places[nplaces];
    char *f[4] = {l, 0, 0, 0};
    for (int i = 1; i < 4 && f[i - 1]; i++) {
      char *t = strchr(f[i - 1], '\t');
      if (t) *t = 0, f[i] = t + 1;
    }
    if (f[3]) {
      snprintf(p->path, sizeof p->path, "%s", f[0]);
      snprintf(p->label, sizeof p->label, "%s", f[1]);
      p->fr = atoll(f[2]), p->tot = atoll(f[3]);
      nplaces++;
    }
    l = e ? e + 1 : 0;
  }
  resp_free(&r);
  return 0;
}
static int cur_place(void) { // the longest place path that contains cwd
  int best = -1;
  size_t bl = 0;
  for (int i = 0; i < nplaces; i++) {
    size_t l = strlen(places[i].path);
    int in = !strcmp(places[i].path, "/") || (!strncmp(cwd, places[i].path, l) && (cwd[l] == '/' || !cwd[l]));
    if (in && (best < 0 || l > bl)) best = i, bl = l;
  }
  return best;
}
static void set_theme(const char *id);
static void load_settings(void) {
  resp_t r;
  if (http("GET", "/api/settings", 0, 0, 0, 1 << 16, &r) || r.status != 200) {
    resp_free(&r);
    set.ok = 0, set.fails++;
    return;
  }
  set.fails = 0;
  set.web = (int)jnum(r.body, "web"), set.hidden = (int)jnum(r.body, "hidden"), set.confirm = (int)jnum(r.body, "confirm");
  set.grid = (int)jnum(r.body, "grid"), set.intro = (int)jnum(r.body, "intro"), set.sort = (int)jnum(r.body, "sort") & 3;
  if (jget(r.body, "sound")) snd_mode = jnum(r.body, "sound") ? 1 : 0;
  jstr(r.body, "ip", set.ip, sizeof set.ip);
  char th[32] = "";
  jstr(r.body, "theme", th, sizeof th);
  if (th[0]) set_theme(th);
  set.ok = 1;
  resp_free(&r);
}
static void load_favs(void) {
  resp_t r;
  if (http("GET", "/api/favs?tsv=1", 0, 0, 0, 1 << 18, &r) || r.status != 200) {
    resp_free(&r);
    return;
  }
  nfavs = 0;
  for (char *l = r.body; l && *l && nfavs < 64;) {
    char *e = strchr(l, '\n');
    if (e) *e = 0;
    char *t1 = strchr(l, '\t'), *t2 = t1 ? strchr(t1 + 1, '\t') : 0;
    if (t2 && t2[1] == '/') {
      favs[nfavs].dir = atoi(l), favs[nfavs].gone = atoi(t1 + 1);
      snprintf(favs[nfavs].path, sizeof favs[nfavs].path, "%s", t2 + 1);
      nfavs++;
    }
    l = e ? e + 1 : 0;
  }
  if (fsel >= nfavs) fsel = nfavs ? nfavs - 1 : 0;
  resp_free(&r);
}
static int is_fav(const char *p) {
  for (int i = 0; i < nfavs; i++)
    if (!strcmp(favs[i].path, p)) return 1;
  return 0;
}
static void fav_set(const char *p, int on) {
  char e[3000], f[3100];
  urlenc(p, e, sizeof e);
  snprintf(f, sizeof f, "path=%s&on=%d", e, on);
  if (!api_post("/api/fav", f, 0, 0)) {
    load_favs();
    say(on ? "Added to favourites \x82 L1 / R1 to the Favourites tab" : "Removed from favourites");
  }
}
static void thumbs_reset(void);
// R3: by name, size (biggest first), date (newest first) or type; folders always come first
static const char *const SORTS[] = {"name", "size", "date", "type"};
static int item_cmp(const void *a, const void *b) {
  const item_t *x = a, *y = b;
  int dx = x->type == 'd' || x->type == 'D', dy = y->type == 'd' || y->type == 'D';
  if (dx != dy) return dy - dx;
  if (set.sort == 1 && !dx && x->size != y->size) return x->size < y->size ? 1 : -1;
  if (set.sort == 2 && x->mtime != y->mtime) return x->mtime < y->mtime ? 1 : -1;
  if (set.sort == 3 && !dx) {
    const char *ex = strrchr(x->name, '.'), *ey = strrchr(y->name, '.');
    int c = strcasecmp(ex ? ex : "", ey ? ey : "");
    if (c) return c;
  }
  return strcasecmp(x->name, y->name);
}
static void sort_items(void) {
  if (nitems > 1) qsort(items, nitems, sizeof *items, item_cmp);
}
// lists dir; on success it becomes cwd and the focus goes to `focus` if it's there
static int load_dir(const char *dir_in, const char *focus_in) {
  char dir[1024], focus_buf[256], e[2200], path[2400];
  const char *focus = 0;
  snprintf(dir, sizeof dir, "%s", dir_in); // dir_in may be cwd itself, which this rewrites
  if (focus_in) snprintf(focus_buf, sizeof focus_buf, "%s", focus_in), focus = focus_buf;
  urlenc(dir, e, sizeof e);
  snprintf(path, sizeof path, "/api/ls?tsv=1&path=%s", e);
  resp_t r;
  int rc = http("GET", path, 0, 0, 0, 64 << 20, &r);
  if (rc) {
    say_err(rc == -2 ? "This folder is too big to list" : "The background service isn't answering");
    return -1;
  }
  if (r.status != 200) {
    char m[300] = "";
    jstr(r.body, "error", m, sizeof m);
    say("Can't open %s: %s", base_of(dir), m[0] ? m : "error");
    toast_err = 1;
    resp_free(&r);
    return -1;
  }
  char keep[256] = "";
  if (!focus && sel < nitems) snprintf(keep, sizeof keep, "%s", items[sel].name), focus = keep; // a refresh keeps the focus
  int same = !strcmp(dir, cwd);
  char **marked = 0;
  int nm = 0;
  if (same && nmarked) { // a refresh keeps the selection too
    marked = calloc(nmarked, sizeof *marked);
    for (int i = 0; marked && i < nitems; i++)
      if (items[i].mark) marked[nm++] = strdup(items[i].name);
  }
  free_items();
  for (char *l = r.body; l && *l;) {
    char *e2 = strchr(l, '\n');
    if (e2) *e2 = 0;
    char *t1 = strchr(l, '\t'), *t2 = t1 ? strchr(t1 + 1, '\t') : 0, *t3 = t2 ? strchr(t2 + 1, '\t') : 0,
         *t4 = t3 ? strchr(t3 + 1, '\t') : 0;
    if (t4) {
      if (nitems == icap) {
        item_t *ni = realloc(items, (icap = icap ? icap * 2 : 256) * sizeof *items);
        if (!ni) break;
        items = ni;
      }
      item_t *it = &items[nitems];
      it->type = l[0];
      it->size = atoll(t1 + 1), it->mtime = atoll(t2 + 1), it->lock = atoi(t3 + 1);
      char *t5 = strchr(t4 + 1, '\t'); // a game / app / package: ID, name, icon
      if (t5) *t5 = 0;
      tsv_unesc(t4 + 1);
      it->name = strdup(t4 + 1);
      it->mark = 0, it->tid = it->tname = 0, it->ic = 0;
      if (t5) {
        char *t6 = strchr(t5 + 1, '\t'), *t7 = t6 ? strchr(t6 + 1, '\t') : 0;
        if (t7) {
          *t6 = *t7 = 0;
          tsv_unesc(t5 + 1), tsv_unesc(t6 + 1);
          if (t5[1]) it->tid = strdup(t5 + 1);
          if (t6[1]) it->tname = strdup(t6 + 1);
          it->ic = atoi(t7 + 1);
        }
      }
      if (it->name) nitems++;
    }
    l = e2 ? e2 + 1 : 0;
  }
  resp_free(&r);
  sort_items();
  snprintf(cwd, sizeof cwd, "%s", dir);
  sel = 0;
  for (int i = 0; focus && i < nitems; i++)
    if (!strcmp(items[i].name, focus)) sel = i;
  for (int k = 0; k < nm; k++) {
    for (int i = 0; i < nitems; i++)
      if (!strcmp(items[i].name, marked[k]) && !items[i].mark) items[i].mark = 1, nmarked++;
    free(marked[k]);
  }
  free(marked);
  list_err = 0, list_msg[0] = 0;
  thumbs_reset();
  return 0;
}
static void refresh(void) { load_dir(cwd, 0); }

// ---- background job polling ----
static struct {
  int id, live, shown, seen_id, need_pw, fin; // fin: all read, the compressor is writing its last part
  int queued, minimised;                      // jobs waiting their turn; the card was minimised (stays so for them)
  long long elapsed;                          // ms the job has run
  float spd[96];                              // recent speeds (bytes/s), for the graph
  int nspd;
  long long last_db;
  unsigned long long last_t;
  char state[16], op[8], cur[300], err[400], result[1100];
  long long tb, db, tn, dn, speed;
} job = {.seen_id = -1};
// the info card: details from /api/info plus a folder size or SHA-256 worked out by a job
static struct {
  int open, job_id, isdir, system, link;
  char path[1300], name[300], type[16], mode[8], target[600], sha[80], sizes[160], note[200];
  long long size, created, modified, accessed, uid, gid;
} info;
static int is_info_op(const char *op) { return !strcmp(op, "hash") || !strcmp(op, "size"); }
static const char *job_verb(const char *op, int done) {
  static const char *const o[][3] = {{"copy", "Copying", "Copied"}, {"move", "Moving", "Moved"}, {"delete", "Deleting", "Deleted"},
                                     {"zip", "Zipping", "ZIP created"}, {"7z", "Compressing to 7z", "7z archive created"},
                                     {"tar", "Compressing", "Archive created"},
                                     {"extract", "Extracting", "Extracted"}, {"chmod", "Changing permissions", "Permissions changed"},
                                     {"hash", "Working out SHA-256", "SHA-256 ready"}, {"size", "Counting", "Counted"}};
  for (unsigned i = 0; i < sizeof o / sizeof *o; i++)
    if (!strcmp(op, o[i][0])) return o[i][done ? 2 : 1];
  return done ? "Done" : "Working";
}
static float job_frac(void) { // 0..1: by bytes, or by items for delete / chmod
  int items = !strcmp(job.op, "delete") || !strcmp(job.op, "chmod");
  float pc = items ? (job.tn ? (float)job.dn / job.tn : 0) : job.tb ? (float)job.db / job.tb : job.tn ? (float)job.dn / job.tn : 0;
  return pc > 1 ? 1 : pc < 0 ? 0 : pc;
}
static float job_shown_pc; // eased, for the bars
static void left_text(long long sec, char *o, size_t n);
static void ask_xpw(int again);
static int x_after = -1, x_wait; // an extract this app started: the first job after x_after (a password, if it needs one)
// /api/job's answer, fetched on its own thread so a busy payload never stalls a frame
static char *jp_body;
static volatile int jp_fast;
static void *jp_thread(void *u) {
  (void)u;
  for (;;) {
    resp_t r;
    if (!http_now("GET", "/api/job", 0, 0, 0, 1 << 16, &r) && r.status == 200) {
      char *old = __atomic_exchange_n(&jp_body, r.body, __ATOMIC_ACQ_REL);
      free(old);
      r.body = 0;
    }
    resp_free(&r);
    usleep(jp_fast ? 250000 : 1000000);
  }
  return 0;
}
static void jp_start(void) {
  pthread_t t;
  pthread_attr_t a;
  pthread_attr_init(&a);
  pthread_attr_setstacksize(&a, 128 << 10);
  pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
  pthread_create(&t, &a, jp_thread, 0);
  pthread_attr_destroy(&a);
}
static void job_apply(char *body);
static unsigned long long jp_fast_until; // quick polls for a moment after starting something
static void poll_job(void) { // right now (after starting a job)
  jp_fast_until = now_ms + 4000, jp_fast = 1;
  resp_t r;
  if (http("GET", "/api/job", 0, 0, 0, 1 << 16, &r) || r.status != 200) {
    resp_free(&r);
    return;
  }
  job_apply(r.body);
  resp_free(&r);
}
static void poll_job_async(void) { // the newest answer from the poller, if there is one
  char *b = __atomic_exchange_n(&jp_body, (char *)0, __ATOMIC_ACQ_REL);
  if (b) job_apply(b), free(b);
  jp_fast = job.live || now_ms < jp_fast_until;
}
static void job_apply(char *body) {
  resp_t r = {0};
  r.body = body;
  int id = (int)jnum(r.body, "id");
  jstr(r.body, "state", job.state, sizeof job.state);
  jstr(r.body, "op", job.op, sizeof job.op);
  jstr(r.body, "cur", job.cur, sizeof job.cur);
  jstr(r.body, "err", job.err, sizeof job.err);
  jstr(r.body, "result", job.result, sizeof job.result);
  job.tb = jnum(r.body, "total_b"), job.db = jnum(r.body, "done_b");
  job.tn = jnum(r.body, "total_n"), job.dn = jnum(r.body, "done_n"), job.speed = jnum(r.body, "speed");
  job.need_pw = (int)jnum(r.body, "need_pw"), job.fin = (int)jnum(r.body, "finishing");
  int queued = (int)jnum(r.body, "queued");
  job.elapsed = jnum(r.body, "elapsed");
  int live = !strcmp(job.state, "running") || !strcmp(job.state, "counting");
  if (queued > job.queued && job.live) say("Added to the queue \x82 it starts when the current job is done");
  job.queued = queued;
  if (id != job.id) job.nspd = 0, job.last_db = 0, job.last_t = 0; // a new job: a new speed graph
  if (live && !strcmp(job.state, "running") && now_ms - job.last_t >= 250) { // the speed right now, for the graph
    if (job.last_t && job.db >= job.last_db) {
      float v = (job.db - job.last_db) * 1000.f / (float)(now_ms - job.last_t);
      if (job.nspd == 96) memmove(job.spd, job.spd + 1, 95 * sizeof *job.spd), job.nspd--;
      job.spd[job.nspd++] = v;
    }
    job.last_db = job.db, job.last_t = now_ms;
  }
  if (job.seen_id < 0) job.seen_id = id; // whatever ran before the app opened
  int quiet = is_info_op(job.op); // folder size and SHA-256 show in the info card, not as a job
  if (id != job.id && live && !quiet) job.shown = !job.minimised; // a new job (from here, the web UI or the queue)
  if (info.open && id == info.job_id) { // progress and result for the info card
    char a[64], b[64];
    if (live && !strcmp(job.op, "hash")) snprintf(info.note, sizeof info.note, "Working out\x80 %d%%", job.tb ? (int)(job.db * 100 / job.tb) : 0);
    else if (live) snprintf(info.note, sizeof info.note, "Counting\x80 %s so far", human(job.tb, a, sizeof a));
    else if (!strcmp(job.state, "done") && !strcmp(job.op, "hash")) snprintf(info.sha, sizeof info.sha, "%s", job.result), info.note[0] = 0;
    else if (!strcmp(job.state, "done")) {
      long long by = 0, nf = 0, nd = 0;
      sscanf(job.result, "%lld %lld %lld", &by, &nf, &nd);
      snprintf(info.sizes, sizeof info.sizes, "%s \x82 %lld file%s, %lld folder%s", human(by, b, sizeof b), nf, nf == 1 ? "" : "s", nd,
               nd == 1 ? "" : "s");
      info.note[0] = 0;
    } else if (!live) snprintf(info.note, sizeof info.note, "%s", job.err[0] ? job.err : "Stopped");
  }
  if ((id != job.seen_id || job.live) && !live && id == job.id) {
    if (!quiet) {
      char w[400];
      if (!strcmp(job.op, "extract") || !strcmp(job.op, "zip") || !strcmp(job.op, "7z") || !strcmp(job.op, "tar"))
        snprintf(w, sizeof w, "%s: %s", job_verb(job.op, 1), base_of(job.result));
      else snprintf(w, sizeof w, "%s", job_verb(job.op, 1));
      if (!strcmp(job.state, "done")) say("%s", w);
      else if (!strcmp(job.state, "failed") && job.need_pw && x_wait && id > x_after && !strcmp(job.op, "extract"))
        x_wait = 0, ask_xpw(job.need_pw == 2); // a password, then again
      else if (!strcmp(job.state, "failed")) say_err(job.err[0] ? job.err : "That didn't work");
      else if (!strcmp(job.state, "cancelled")) say_err("Cancelled");
      refresh();
      load_places();
    }
    job.seen_id = id, job.shown = 0;
    if (!job.queued) job.minimised = 0; // nothing more to come: the next job shows its card
  }
  job.id = id, job.live = live;
}

// ---- drawing ----
static void make_bg(void) { // the web UI's glow gradient, in the theme's colours
  if (!bgbuf && !(bgbuf = malloc((size_t)W * H * 4))) halt("FileDen: out of memory");
  unsigned a = P->g0, z = P->g1, l = P->glow[0];
  float ar = (a >> 16) & 255, ag = (a >> 8) & 255, ab = a & 255, zr = (z >> 16) & 255, zg = (z >> 8) & 255, zb = z & 255;
  float lr = (l >> 16) & 255, lg = (l >> 8) & 255, lb = l & 255, gw = P->gw[0] * .58f;
  for (int y = 0; y < H; y++)
    for (int x = 0; x < W; x++) {
      float t = (x + y) / (float)(W + H);
      float r = ar + (zr - ar) * t, g = ag + (zg - ag) * t, b = ab + (zb - ab) * t;
      float dx = (x - W * .85f) / (W * .6f), dy = y / (H * .5f), k = fmaxf(0, 1 - sqrtf(dx * dx + dy * dy)) * gw;
      r += (lr - r) * k, g += (lg - g) * k, b += (lb - b) * k;
      bgbuf[(size_t)y * W + x] = RGB((int)r, (int)g, (int)b);
    }
}
static void set_theme(const char *id) {
  for (int i = 0; i < NPALS; i++)
    if (!strcmp(PALS[i].id, id) && i != theme) {
      theme = i;
      make_bg();
    }
}
static void blit_logo(int x0, int y0, int sz) {
  if (!logo) return;
  for (int y = 0; y < sz; y++)
    for (int x = 0; x < sz; x++) {
      if (x0 + x < 0 || x0 + x >= W || y0 + y < 0 || y0 + y >= H) continue;
      unsigned p = logo[(y * logo_n / sz) * logo_n + x * logo_n / sz]; // RGBA bytes
      int a = p >> 24;
      if (!a) continue;
      unsigned hex = ((p & 255) << 16) | (p & 0xff00) | ((p >> 16) & 255);
      blendi(&fb[(size_t)(y0 + y) * W + x0 + x], hex, a + (a >> 7));
    }
}
static void fill_tri(float ax, float ay, float bx, float by, float cx, float cy, unsigned hex) { // small icons only
  int x0 = (int)fminf(ax, fminf(bx, cx)), x1 = (int)ceilf(fmaxf(ax, fmaxf(bx, cx)));
  int y0 = (int)fminf(ay, fminf(by, cy)), y1 = (int)ceilf(fmaxf(ay, fmaxf(by, cy)));
  for (int y = y0; y <= y1; y++)
    for (int x = x0; x <= x1; x++) {
      int in = 0;
      for (int sy = 0; sy < 4; sy++) // 4x4 supersampling
        for (int sx = 0; sx < 4; sx++) {
          float px = x + (sx + .5f) / 4, py = y + (sy + .5f) / 4;
          float d1 = (px - bx) * (ay - by) - (ax - bx) * (py - by), d2 = (px - cx) * (by - cy) - (bx - cx) * (py - cy),
                d3 = (px - ax) * (cy - ay) - (cx - ax) * (py - ay);
          int neg = d1 < 0 || d2 < 0 || d3 < 0, pos = d1 > 0 || d2 > 0 || d3 > 0;
          in += !(neg && pos);
        }
      if (in && x >= 0 && x < W && y >= 0 && y < H) blendi(fb + (size_t)y * W + x, hex, in * 16);
    }
}
static void line_aa(float ax, float ay, float bx, float by, float wd, unsigned hex) { // round-capped stroke
  int x0 = (int)(fminf(ax, bx) - wd), x1 = (int)(fmaxf(ax, bx) + wd), y0 = (int)(fminf(ay, by) - wd), y1 = (int)(fmaxf(ay, by) + wd);
  for (int y = y0; y <= y1; y++)
    for (int x = x0; x <= x1; x++) {
      float c = .5f - (seg_d(x + .5f, y + .5f, ax, ay, bx, by) - wd / 2);
      if (c > 0 && x >= 0 && x < W && y >= 0 && y < H) blendi(fb + (size_t)y * W + x, hex, c >= 1 ? 256 : (int)(c * 256));
    }
}
static void tick_box(int x, int cy, int on) {
  if (!on) {
    rrect(x, cy - 15, 30, 30, 9, C_LINE, 1, 2);
    return;
  }
  rrect(x, cy - 15, 30, 30, 9, C_ACC, 1, 0);
  line_aa(x + 8, cy + 1, x + 13, cy + 6, 3.4f, P->on_acc);
  line_aa(x + 13, cy + 6, x + 22, cy - 5, 3.4f, P->on_acc);
}
static void lock_glyph(int x, int y, unsigned col) { // 18 x 22 padlock
  rrect(x + 3, y, 12, 13, 6, col, 1, 2.2f);
  rrect(x, y + 8, 18, 14, 4, col, 1, 0);
}
static void draw_icon(int x, int y, int k, int dim) { // 44 x 44
  unsigned acc = C_ACC, line = C_MUTED;
  float a = dim ? .45f : 1;
  if (k == K_DIR) {
    rrect(x + 2, y + 6, 18, 10, 4, P->acc2, a, 0);
    rrect(x + 2, y + 10, 40, 28, 6, acc, a, 0);
    rrect(x + 2, y + 17, 40, 2, 1, P->acc2, a * .8f, 0);
    return;
  }
  rrect(x + 7, y + 2, 30, 40, 7, line, a, 2.2f);
  if (k == K_TXT)
    for (int i = 0; i < 3; i++) rrect(x + 13, y + 13 + i * 7, i == 2 ? 11 : 18, 3, 1.5f, acc, a, 0);
  else if (k == K_IMG) {
    rrect(x + 23, y + 9, 7, 7, 3.5f, acc, a, 0);
    fill_tri(x + 11, y + 34, x + 18, y + 22, x + 25, y + 34, acc);
    fill_tri(x + 19, y + 34, x + 26, y + 26, x + 33, y + 34, acc);
  } else if (k == K_VID) fill_tri(x + 17, y + 14, x + 17, y + 30, x + 29, y + 22, acc);
  else if (k == K_ARC) { // zipper teeth and a pull
    for (int i = 0; i < 4; i++) rrect(x + (i & 1 ? 22 : 17), y + 7 + i * 5, 5, 3, 1.5f, acc, a, 0);
    rrect(x + 16, y + 27, 12, 10, 3, acc, a, 0);
    rrect(x + 19, y + 30, 6, 3, 1.5f, P->top, a, 0);
  } else if (k == K_PKG) {
    rrect(x + 13, y + 15, 18, 14, 3, acc, a, 2);
    rrect(x + 13, y + 20, 18, 2, 1, acc, a, 0);
  } else rrect(x + 14, y + 30, 16, 3, 1.5f, line, a * .7f, 0);
}

static char *http_ok_quit(void);
// hold O to close (after ProsperoStore's timing)
#define CLOSE_DELAY 300
#define CLOSE_HOLD 1500
static int close_ms;
static float close_shown, close_filled;
// hold Triangle to select everything here (or clear it all, when all of it is ticked); a tap ticks one
#define SA_DELAY 250
#define SA_HOLD 650
static float sa_fill; // 0..1 while Triangle is held
static void dim_screen(int k) { // darken the frame by k/256
  for (int i = 0; i < W * H; i++) {
    unsigned p = fb[i];
    fb[i] = 0xff000000u | ((((p & 0xff00ff) * (256 - k)) >> 8) & 0xff00ff) | ((((p & 0xff00) * (256 - k)) >> 8) & 0xff00);
  }
}
static void draw_close(void) {
  if (close_shown <= 0.01f) return;
  float a = close_shown;
  dim_screen((int)(a * 175));
  const char *w1 = "Keep holding", *w2 = "to close";
  int fs = 44, gs = 56, gap = 18;
  int tw = text_w(1, fs, w1) + gap + gs + gap + text_w(1, fs, w2), x = (W - tw) / 2, cy = H / 2 - 30;
  int op = (int)(a * 256);
  x += text_a(x, cy - fs / 2 - 4, 1, fs, C_TEXT, op, w1) + gap;
  key_alpha = a;
  glyph_key(x, cy - gs / 2, gs, 'o');
  key_alpha = 1;
  x += gs + gap;
  text_a(x, cy - fs / 2 - 4, 1, fs, C_TEXT, op, w2);
  int bw = 620, bx = (W - bw) / 2, by = cy + 62;
  rrect(bx, by, bw, 8, 4, 0xffffff, .25f * a, 0);
  int fw = (int)(bw * close_filled);
  if (fw > 0) rrect(bx, by, fw < 8 ? 8 : fw, 8, 4, 0xffffff, a, 0);
  const char *sub = set.web ? "The web server keeps running for your phone or PC" : "The web server is off, so FileDen stops when it closes";
  text_a((W - text_w(0, 22, sub)) / 2, by + 34, 0, 22, C_MUTED, op, sub);
}
static void quit_app(void) {
  close_filled = close_shown = 1;
  draw_close();
  present();
  char *qr = http_ok_quit();
#ifdef HOST_PREVIEW
  fprintf(stderr, "quit: web=%d reply=%s\n", set.web, qr ? qr : "(none)");
#endif
  free(qr);
#ifdef HOST_PREVIEW
  exit(0);
#else
  static int (*load_exec)(const char *, const char **);
  static int tried;
  if (!tried) {
    tried = 1;
    static const char *mods[] = {"libSceSystemService.sprx", "/system/common/lib/libSceSystemService.sprx", 0};
    for (int i = 0; mods[i] && !load_exec; i++) {
      int h = sceKernelLoadStartModule(mods[i], 0, 0, 0, 0, 0);
      void *f = 0;
      if (h >= 0 && !sceKernelDlsym(h, "sceSystemServiceLoadExec", &f) && f) load_exec = (int (*)(const char *, const char **))f;
    }
  }
  if (load_exec) {
    int r = load_exec("exit", 0);
    for (int i = 0; r == 0 && i < 15; i++) sceKernelUsleep(100000);
  }
  kill(getpid(), SIGKILL);
  _exit(0);
#endif
}

// button hints: glyph or keycap with the label, in the accent style
static int bumper(int x, int cy, int size, const char *lab) {
  int w = (int)(size * 1.45f), h = (int)(size * .74f), y = cy - h / 2;
  unsigned tc = chip(x, y, w, h, h / 2.f);
  text_c(x + w / 2.f, y + h / 2.f, 1, (int)(size * .4f + .5f), tc, lab);
  return w;
}
static int stick_key(int x, int cy, int size, const char *lab) { // a thumbstick seen from above: L3 / R3
  float r = size / 2.f;
  unsigned tc = chip(x, cy - r, size, size, r);
  rrect(x + size * .2f, cy - r * .6f, size * .6f, size * .6f, r * .6f, tc, .35f, 1.2f); // the stick's top
  text_c(x + r, cy, 1, (int)(size * .36f + .5f), tc, lab);
  return size;
}
static int options_key(int x, int cy, int size) {
  int w = (int)(size * 1.3f), h = (int)(size * .74f);
  unsigned tc = chip(x, cy - h / 2, w, h, h / 2.f);
  float hw = size * .22f, st = size * .06f;
  for (int i = -1; i <= 1; i++) rrect(x + w / 2.f - hw, cy + i * size * .12f - st / 2, 2 * hw, st, st / 2, tc, 1, 0);
  return w;
}
static int dpad_key(int x, int cy, int size, int horiz) { // d-pad: the left/right (or up/down) arms lit
  int s = size, y = cy - s / 2;
  unsigned tc = chip(x, y, s, s, s / 2.f);
  float c = s / 2.f, arm = s * .2f, th = s * .14f;
  rrect(x + c - arm - th / 2 - 2, y + c - th / 2, arm, th, th / 2, tc, horiz ? 1 : .35f, 0);
  rrect(x + c + th / 2 + 2, y + c - th / 2, arm, th, th / 2, tc, horiz ? 1 : .35f, 0);
  rrect(x + c - th / 2, y + c - arm - th / 2 - 2, th, arm, th / 2, tc, horiz ? .35f : 1, 0);
  rrect(x + c - th / 2, y + c + th / 2 + 2, th, arm, th / 2, tc, horiz ? .35f : 1, 0);
  return s;
}
static void ghint(int *x, int y, char k, const char *what) {
  if (k == 'O') *x += options_key(*x, y + 15, 34) + 12;
  else if (k == '1' || k == '2' || k == '3' || k == '4') *x += bumper(*x, y + 15, 34, k == '1' ? "L1" : k == '2' ? "R1" : k == '3' ? "L2" : "R2") + 12;
  else if (k == 'h' || k == 'v') *x += dpad_key(*x, y + 15, 34, k == 'h') + 12;
  else if (k == 'R' || k == 'L') *x += stick_key(*x, y + 15, 34, k == 'R' ? "R3" : "L3") + 12;
  else if (k == 'm') { // L2 and R2 together
    *x += bumper(*x, y + 15, 34, "L2") + 8;
    *x += bumper(*x, y + 15, 34, "R2") + 12;
  }
  else {
    glyph_key(*x, y - 2, 34, k);
    *x += 34 + 12;
  }
  text(*x + 1, y + 6, 0, 19, 0x000000, what);
  *x += text(*x, y + 4, 0, 19, 0xdfe6f2, what) + 30;
}

static void ghint_back(int *x, int y) { // O Back, with holding it to close FileDen said quietly beside it
  glyph_key(*x, y - 2, 34, 'o');
  *x += 34 + 12;
  text(*x + 1, y + 6, 0, 19, 0x000000, "Back");
  *x += text(*x, y + 4, 0, 19, 0xdfe6f2, "Back") + 10;
  *x += text(*x, y + 6, 0, 17, C_SUB, "\x82 hold to close") + 30;
}

// ---- splash + payload start ----
static long long mono_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}
static void draw_splash(float prog, const char *st, int err) {
  memcpy(fb, bgbuf, (size_t)W * H * 4);
  int is = 200, ix = (W - is) / 2, iy = H / 2 - 230;
  blit_logo(ix, iy, is);
  const char *t = "FileDen";
  text((W - text_w(1, 56, t)) / 2, iy + is + 34, 1, 56, C_TEXT, t);
  int bw = 520, bx = (W - bw) / 2, by = iy + is + 136;
  rrect(bx, by, bw, 8, 4, 0xffffff, .25f, 0);
  if (err) rrect(bx, by, bw, 8, 4, 0xff6b6b, 1, 0);
  else { // indeterminate: a white segment glides across the track, eased, and loops
    float t2 = prog - floorf(prog), e = t2 < .5f ? 4 * t2 * t2 * t2 : 1 - powf(-2 * t2 + 2, 3) / 2;
    int sw = bw * 3 / 10, sx = bx - sw + (int)(e * (bw + sw)), a = sx < bx ? bx : sx, b = sx + sw > bx + bw ? bx + bw : sx + sw;
    if (b - a >= 2) rrect(a, by, b - a, 8, (b - a) < 8 ? (b - a) / 2.f : 4, 0xffffff, 1, 0);
  }
  text((W - text_w(0, 24, st)) / 2, by + 30, 0, 24, err ? 0xffb0b0 : C_MUTED, st);
}
static void splash(const char *st, int err) {
  draw_splash((mono_ms() % 1000000) / 1400.f, st, err);
  present();
}
static void slog(const char *fmt, ...) {
  mkdir(DATA, 0777);
  FILE *f = fopen(LOG_PATH, "a");
  if (!f) return;
  time_t t = time(0);
  char ts[32];
  strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", localtime(&t));
  fprintf(f, "%s app: ", ts);
  va_list ap;
  va_start(ap, fmt);
  vfprintf(f, fmt, ap);
  va_end(ap);
  fputc('\n', f);
  fclose(f);
}
static int port_open(void) {
  int s = connect_local();
  if (s < 0) return 0;
  close(s);
  return 1;
}
static void splash_for(const char *st, int ms) {
  for (long long end = mono_ms() + ms; mono_ms() < end;) {
    splash(st, 0);
    usleep(16000);
  }
}
#define SPLASH_MIN_MS 1800
#define SPLASH_DONE_MS 600
static int wait_server(const char *st, int ms) {
  for (long long end = mono_ms() + ms, i = 0; mono_ms() < end; i++) {
    if (i % 6 == 0 && port_open()) return 1;
    splash(st, 0);
    usleep(16000);
  }
  return port_open();
}
static void splash_fail(const char *why) { // X retries, O exits
  char m[200];
  snprintf(m, sizeof m, "%s  \x82  X Retry \x82 O Exit", why);
  slog("failed: %s", why);
  unsigned prev = ~0u;
  for (;;) {
    splash(m, 1);
    unsigned b = pad_buttons(), down = b & ~prev;
    prev = b;
    if (down & PAD_CROSS) return;
    if (down & PAD_CIRCLE) quit_app();
    usleep(16000);
  }
}
static void start_server(void) {
  int sent = 0;
  long long t0 = mono_ms();
  for (;;) {
    if (wait_server("Starting background service\x80", sent ? 2000 : 300)) break;
    mkdir(DATA, 0777);
    FILE *fl = fopen(DATA "/from_app", "w"); // the payload then keeps its notification short
    if (fl) fclose(fl);
    FILE *f = fopen("/app0/sce_sys/fileden.elf", "rb");
    if (!f) {
      splash_fail("fileden.elf is missing from the app folder");
      continue;
    }
    slog("server not running, sending payload to the ELF loader on :9021");
    int s = socket(AF_INET, SOCK_STREAM, 0), ok = 0;
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons(9021);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (s >= 0 && !connect(s, (struct sockaddr *)&a, sizeof a)) {
      char buf[16384];
      size_t n;
      ok = 1;
      while ((n = fread(buf, 1, sizeof buf, f)) > 0)
        if (send(s, buf, n, 0) != (ssize_t)n) {
          ok = 0;
          break;
        }
    }
    if (s >= 0) close(s);
    fclose(f);
    if (!ok) {
      splash_fail("Couldn't start the background service: no ELF loader on port 9021");
      continue;
    }
    sent = 1;
    if (wait_server("Starting background service\x80", 8000)) break;
    splash_fail("The background service didn't start");
  }
  long long left = SPLASH_MIN_MS - (mono_ms() - t0) - SPLASH_DONE_MS;
  if (left > 0) splash_for("Starting background service\x80", (int)left);
  splash_for("Ready", SPLASH_DONE_MS);
}
// on close: ask the payload to stop unless the web server should stay up
static char *http_ok_quit(void) {
  resp_t r;
  if (!set.web && !http_now("POST", "/api/quit", "x=1", 3, 0, 4096, &r)) return r.body;
  return 0;
}

// ---- screens ----
enum { SCR_BROWSE, SCR_IMAGE, SCR_TEXT, SCR_SETTINGS, SCR_PICK, SCR_FAVS };
static int scr;
static float sel_y, shown = 1;

// modal layers over the screen
enum { M_NONE, M_MENU, M_CONFIRM, M_INFO, M_JOB, M_PERM, M_INTRO };
static int modal;
// action menu
enum { A_OPEN, A_COPY, A_CUT, A_PASTE, A_COPYTO, A_MOVETO, A_RENAME, A_DELETE, A_INFO, A_PERMS, A_COMPRESS,
       A_XHERE, A_XTO, A_NEWDIR, A_NEWFILE, A_ASTEXT, A_SELALL, A_SELNONE, A_CLIPX, A_JOB, A_FAV, A_FAVDIR,
       A_FAV_OPEN, A_FAV_SHOW, A_FAV_REMOVE, A_SELECT, A_VLIST, A_VGRID, A_SORT, A_REFRESH, A_UNDO, A_SEP, A_FMT0 }; // A_FMT0 + i: FMTS[i]
static int menu[40], nmenu, msel, mtop, menu_mode;
static char undo_what[400]; // the menu's Undo line, from /api/undo
static float menu_hl = -1, menu_in; // the sliding highlight (px), the open animation 0..1 // menu_mode: 0 actions, 1 compress formats, 2 a favourite
// Compress: every format the payload writes
static const struct {
  const char *ext, *label, *api, *z;
  int pw; // asks for a password (AES-256)
} FMTS[] = {{"zip", "ZIP", "/api/zip", "", 0},
            {"zip", "ZIP with a password", "/api/zip", "", 1},
            {"7z", "7z \x82 smallest, slower", "/api/7z", "", 0},
            {"7z", "7z with a password", "/api/7z", "", 1},
            {"tar", "TAR \x82 no compression", "/api/tar", "", 0},
            {"tar.gz", "TAR.GZ \x82 gzip", "/api/tar", "gz", 0},
            {"tar.xz", "TAR.XZ \x82 xz, small", "/api/tar", "xz", 0},
            {"tar.bz2", "TAR.BZ2 \x82 bzip2", "/api/tar", "bz2", 0},
            {"tar.zst", "TAR.ZST \x82 zstd, fast", "/api/tar", "zst", 0}};
#define NFMTS ((int)(sizeof FMTS / sizeof *FMTS))
static int arc_fmt; // the format being named
static char arc_name[600], x_form[6200]; // the archive's name while its password is typed; the extract to retry
static char conf_title[300], conf_text[600];
static int conf_kind; // what X does in the confirm dialog
enum { C_DELETE, C_DISCARD, C_BINARY };

// on-screen keyboard, with a cursor and file-name friendly keys
static const char *KB[3][4] = {{"1234567890-", "qwertyuiop_", "asdfghjkl.,", "zxcvbnm()'&"},
                               {"1234567890-", "QWERTYUIOP_", "ASDFGHJKL.,", "ZXCVBNM()'&"},
                               {"!@#$%^&*+=~", "[]{}<>;:\"|\\", "/?.,'`()_- ", "0123456789+"}};
static int kb_open, kb_set, kb_r, kb_c, kb_target, kb_cur;
enum { KT_NEWDIR, KT_NEWFILE, KT_RENAME, KT_LINE, KT_INSERT, KT_ARCHIVE, KT_MODE, KT_ARCPW, KT_XPW };
static char kb_buf[4096], kb_title[200];

// image viewer
static unsigned *img_frame; // prepared 1920x1080 frame
static unsigned char *img_px; // decoded RGBA (the payload's JPEG: fitted to the screen, or full size when zoomed)
static int img_w, img_h, img_actual, img_px_x, img_px_y, img_idx, img_ok, img_full, img_ow, img_oh;
static char img_msg[300], img_name[256];
static long long img_size, img_mtime;
static void frame(void);

// text editor
static char **lines;
static int nlines, lcap, tsel, ttop, tdirty, tcrlf;
static char tpath[1300];

static void open_kb(int target, const char *title, const char *init) {
  kb_target = target, kb_open = 1, kb_set = 0, kb_r = 1, kb_c = 0;
  snprintf(kb_buf, sizeof kb_buf, "%s", init);
  snprintf(kb_title, sizeof kb_title, "%s", title);
  kb_cur = (int)strlen(kb_buf);
  if (target == KT_RENAME || target == KT_NEWFILE || target == KT_ARCHIVE) { // cursor before the extension, like a desktop
    const char *d = target == KT_ARCHIVE ? strstr(kb_buf, ".tar.") : 0;
    if (!d) d = strrchr(kb_buf, '.');
    if (d && d != kb_buf) kb_cur = (int)(d - kb_buf);
  }
  if (target == KT_MODE) kb_r = 0, kb_c = 0; // the digits row
}

// ---- header / footer ----
static void header(const char *title, const char *sub) {
  blit_logo(MX, MY - 4, 60);
  text(MX + 78, MY - 2, 1, 36, C_TEXT, title);
  text(MX + 80, MY + 42, 0, 19, C_SUB, sub);
  // web server status, top right
  char a[120];
  int on = set.ok && set.web;
  const char *st = !set.ok ? "Background service not answering" : on ? "Web server on" : "Web server off";
  int tw = text_w(0, 20, st), x = W - MX - tw, left = x - 22;
  rrect(x - 22, MY + 9, 11, 11, 5.5f, !set.ok ? 0xff6b7d : on ? 0x59d38f : 0x7d8487, 1, 0);
  text(x, MY + 2, 0, 20, C_MUTED, st);
  a[0] = 0;
  if (on && set.ip[0]) snprintf(a, sizeof a, "%s:%d", set.ip, PORT);
  else if (set.ok) snprintf(a, sizeof a, "Options \x82 Settings to turn it on");
  if (a[0]) {
    int aw = text_w(0, 18, a);
    text(W - MX - aw, MY + 32, 0, 18, C_SUB, a);
    if (W - MX - aw < left) left = W - MX - aw;
  }
  // a job running in the background service: a small card beside the status
  if (job.live && !job.shown && !is_info_op(job.op)) {
    int cw = 440, ch = 70, cx = left - 30 - cw, cy = MY - 8;
    float pc = job.fin ? .99f : job_frac();
    job_shown_pc += (pc - job_shown_pc) * .2f;
    rrect(cx, cy, cw, ch, 16, P->card_f, .96f, 0);
    rrect(cx, cy, cw, ch, 16, C_ACC, .55f, 1.5f);
    char t[400], cl[400], pt[16];
    snprintf(pt, sizeof pt, "%d%%", (int)(pc * 100));
    int pw = text_w(1, 20, pt);
    int n = snprintf(t, sizeof t, "%s%s", job_verb(job.op, 0), !strcmp(job.state, "counting") ? "\x80" : job.fin ? " \x82 finishing" : "");
    if (job.queued) snprintf(t + n, sizeof t - n, " \x82 +%d queued", job.queued);
    clip_text(cl, sizeof cl, 1, 20, t, cw - 48 - pw - 12);
    text(cx + 22, cy + 9, 1, 20, C_TEXT, cl);
    text(cx + cw - 22 - pw, cy + 9, 1, 20, C_ACC, pt);
    rrect(cx + 22, cy + 39, cw - 44, 5, 2.5f, 0xffffff, .14f, 0);
    if (job_shown_pc > .005f) rrect(cx + 22, cy + 39, fmaxf(5, (cw - 44) * job_shown_pc), 5, 2.5f, C_ACC, 1, 0);
    char rt[80] = "", a[64];
    if (job.fin) snprintf(rt, sizeof rt, "almost done");
    else if (job.speed > 0 && job.tb > job.db) { // write speed and time left
      left_text((job.tb - job.db) / job.speed, a, sizeof a);
      char sp[64];
      snprintf(rt, sizeof rt, "%s/s \x82 %s", human(job.speed, sp, sizeof sp), a);
    }
    int rtw = rt[0] ? text_w(0, 15, rt) : 0;
    clip_text(cl, sizeof cl, 0, 15, job.cur[0] ? job.cur : "Working in the background", cw - 44 - rtw - (rtw ? 14 : 0));
    text(cx + 22, cy + 48, 0, 15, C_SUB, cl);
    if (rt[0]) text(cx + cw - 22 - rtw, cy + 48, 0, 15, C_MUTED, rt);
    const char *tp = "Touchpad: show"; // how to get the big card back
    text(cx + cw - text_w(0, 15, tp) - 6, cy + ch + 6, 0, 15, C_SUB, tp);
  }
}
static void footer_counter(const char *s) { text(W - MX - text_w(0, 20, s), H - MY - 26, 0, 20, C_SUB, s); }

// ---- browse screen ----
#define LIST_TOP (MY + 240)
#define ROW_H 78
static int vis_rows(void) { return (H - MY - 96 - LIST_TOP) / ROW_H; }
static void draw_tabs(void) {
  int y = MY + 128, x = MX, fv = scr == SCR_FAVS, cp = fv ? -1 : cur_place();
  x += bumper(x, y + 16, 40, "L1") + 26;
  { // Favourites: a small star, then the word
    int w = text_w(fv, 26, "Favourites"), sx = x + 11, sy = y + 17;
    for (int k = 0; k < 5; k++) { // five-pointed star: two triangles per point, around the centre
      float a = -1.5708f + k * 1.2566f, b = a - .6283f, c = a + .6283f, ox = sx + cosf(a) * 12, oy = sy + sinf(a) * 12;
      unsigned col = fv ? C_ACC : C_MUTED;
      fill_tri(sx, sy, sx + cosf(b) * 5, sy + sinf(b) * 5, ox, oy, col);
      fill_tri(sx, sy, ox, oy, sx + cosf(c) * 5, sy + sinf(c) * 5, col);
    }
    text(x + 30, y, fv, 26, fv ? C_TEXT : C_MUTED, "Favourites");
    if (fv) rrect(x + 30 + w / 2 - 14, y + 40, 28, 4, 2, C_ACC, 1, 0);
    x += 30 + w + 40;
  }
  for (int i = 0; i < nplaces; i++) {
    int on = i == cp, w = text_w(on, 26, places[i].label);
    text(x, y, on, 26, on ? C_TEXT : C_MUTED, places[i].label);
    if (on) rrect(x + w / 2 - 14, y + 40, 28, 4, 2, C_ACC, 1, 0);
    x += w + 40;
  }
  bumper(x - 14, y + 16, 40, "R1");
  // right side: free space of this place, clipboard
  char a[64], b[64], line[200];
  int rx = W - MX;
  if (!fv && cp >= 0 && places[cp].tot > 0) {
    snprintf(line, sizeof line, "%s free of %s", human(places[cp].fr, a, sizeof a), human(places[cp].tot, b, sizeof b));
    rx -= text_w(0, 20, line);
    text(rx, y + 6, 0, 20, C_SUB, line);
    rx -= 30;
  }
  if (nclip) {
    snprintf(line, sizeof line, "%d to %s", nclip, clip_move ? "move" : "paste");
    int pw = text_w(1, 18, line) + 28;
    rx -= pw;
    rrect(rx, y + 2, pw, 32, 16, C_ACC, 1, 1.6f);
    text_c(rx + pw / 2.f, y + 18, 1, 18, C_ACC, line);
  }
}
static void draw_path(void) {
  int y = MY + 190;
  char shown_p[1200], cl[1200];
  snprintf(shown_p, sizeof shown_p, "%s", cwd);
  draw_icon(MX, y - 8, K_DIR, 0);
  clip_text(cl, sizeof cl, 1, 24, shown_p, W - 2 * MX - 520);
  text(MX + 58, y, 1, 24, C_TEXT, cl);
  char r[120];
  int n = nmarked ? snprintf(r, sizeof r, "%d selected \x82 %d item%s", nmarked, nitems, nitems == 1 ? "" : "s")
                  : snprintf(r, sizeof r, "%d item%s", nitems, nitems == 1 ? "" : "s");
  if (set.sort) snprintf(r + n, sizeof r - n, " \x82 by %s", SORTS[set.sort]);
  text(W - MX - text_w(0, 20, r), y + 4, 0, 20, C_SUB, r);
}
static int is_cut(const item_t *it) {
  if (!nclip || !clip_move || strcmp(clip_dir, cwd)) return 0;
  for (int i = 0; i < nclip; i++)
    if (!strcmp(base_of(clip[i]), it->name)) return 1;
  return 0;
}
static void draw_grid(void);
static int list_first(void) {
  int vis = vis_rows(), first = sel - vis / 2;
  if (first > nitems - vis) first = nitems - vis;
  return first < 0 ? 0 : first;
}
// a square icon (IC x IC) drawn at size x size with rounded corners
static void draw_sq(const unsigned *px, int x0, int y0, int size, int dim) {
  float r = size * .16f;
  for (int y = 0; y < size; y++) {
    if (y0 + y < 0 || y0 + y >= H) continue;
    unsigned *d = fb + (size_t)(y0 + y) * W + x0;
    const unsigned *s = px + (size_t)(y * IC / size) * IC;
    for (int x = 0; x < size; x++) {
      float cx = x < r ? r - x : x > size - 1 - r ? x - (size - 1 - r) : 0, cy = y < r ? r - y : y > size - 1 - r ? y - (size - 1 - r) : 0;
      if (cx * cx + cy * cy > r * r) continue;
      unsigned c = s[x * IC / size];
      d[x] = dim ? (c & 0xfefefe) >> 1 | 0xff000000u : c;
    }
  }
}
// what a row says: a package shows its game's name first, a game / app folder its ID then the name
static void item_names(const item_t *it, const char **main, const char **extra) {
  *main = it->name, *extra = it->tname;
  if (it->tname && !isdir(it)) *main = it->tname, *extra = it->name;
}
static void draw_browse(void) {
  char ver[64];
  snprintf(ver, sizeof ver, "v" VERSION " \x82 File explorer");
  header("FileDen", ver);
  draw_tabs();
  draw_path();
  int top = LIST_TOP, vis = vis_rows(), cw = W - 2 * MX;
  if (!nitems) {
    const char *e1 = "This folder is empty", *e2 = nclip ? "Press Square and choose Paste to put your items here"
                                                         : "Press Square for New folder and New file";
    text((W - text_w(1, 32, e1)) / 2, H / 2 + 10, 1, 32, C_TEXT, e1);
    text((W - text_w(0, 22, e2)) / 2, H / 2 + 62, 0, 22, C_MUTED, e2);
    return;
  }
  if (set.grid) {
    draw_grid();
    return;
  }
  int first = list_first();
  float want = sel - first;
  sel_y += (want - sel_y) * .35f;
  if (fabsf(want - sel_y) < .01f) sel_y = want;
  rrect(MX - 6, top + sel_y * ROW_H - 6, cw + 12, ROW_H - 10 + 12, 20, P->fglow, .22f, 5);
  rrect(MX, top + sel_y * ROW_H, cw, ROW_H - 10, 16, P->card_f, 1, 0);
  rrect(MX, top + sel_y * ROW_H, cw, ROW_H - 10, 16, P->focus, 1, 2.5f);
  char a[64], b[64], cl[1200];
  for (int i = first; i < nitems && i < first + vis; i++) {
    item_t *it = &items[i];
    int y = top + (i - first) * ROW_H, cy = y + (ROW_H - 10) / 2, cut = is_cut(it);
    if (i != sel) rrect(MX, y, cw, ROW_H - 10, 16, it->mark ? P->card_f : P->card, it->mark ? .95f : .55f, 0);
    int x = MX + 22;
    if (nmarked) { // selection ticks only while something is selected
      tick_box(x, cy, it->mark);
      x += 48;
    }
    thumb_t *th = thumbs && nthumbs == nitems ? &thumbs[i] : 0;
    if (th && th->state == 1 && th->sq) draw_sq(th->px, x - 4, cy - 26, 52, cut); // the game's / app's icon
    else draw_icon(x, cy - 22, kind_of(it), cut);
    x += 64;
    int rw = 430, room = cw - (x - MX) - rw - 24;
    const char *mn, *xn;
    item_names(it, &mn, &xn);
    clip_text(cl, sizeof cl, 1, 26, mn, xn ? room * 2 / 3 : room);
    text(x, cy - 17, 1, 26, cut ? C_SUB : C_TEXT, cl);
    int ex = x + text_w(1, 26, cl) + 14;
    if (xn && room - (ex - x) > 80) { // the game's name (or the package's file name), quieter
      char t2[600];
      clip_text(t2, sizeof t2, 0, 22, xn, room - (ex - x) - 30);
      text(ex, cy - 13, 0, 22, C_SUB, "\x82");
      ex += text_w(0, 22, "\x82") + 10;
      ex += text(ex, cy - 13, 0, 22, C_MUTED, t2) + 14;
    }
    if (it->type == 'D' || it->type == 'L') ex += text(ex, cy - 12, 0, 18, C_SUB, "link") + 12;
    if (it->lock) lock_glyph(ex, cy - 12, C_SUB);
    // right: size and date
    const char *s = isdir(it) ? "Folder" : human(it->size, a, sizeof a);
    when(it->mtime, b, sizeof b);
    int dx = MX + cw - 24 - text_w(0, 20, b);
    text(dx, cy - 12, 0, 20, C_SUB, b);
    text(MX + cw - 24 - rw + 40, cy - 12, 0, 20, C_MUTED, s);
  }
  if (nitems > vis) { // scroll bar
    float h = (float)vis / nitems * (vis * ROW_H - 10), y = top + (float)first / nitems * (vis * ROW_H - 10);
    rrect(W - MX + 20, y, 5, fmaxf(h, 30), 2.5f, 0xffffff, .3f, 0);
  }
}
static void draw_favs(void) {
  header("FileDen", "v" VERSION " \x82 Favourites");
  draw_tabs();
  int y = MY + 190, top = LIST_TOP, vis = vis_rows(), cw = W - 2 * MX;
  text(MX, y, 1, 24, C_TEXT, "Favourites");
  char r[80], cl[1200];
  snprintf(r, sizeof r, "%d of 64", nfavs);
  text(W - MX - text_w(0, 20, r), y + 4, 0, 20, C_SUB, r);
  if (!nfavs) {
    const char *e1 = "No favourites yet", *e2 = "Press Square on a file or folder and choose Add to favourites";
    text((W - text_w(1, 32, e1)) / 2, H / 2 + 10, 1, 32, C_TEXT, e1);
    text((W - text_w(0, 22, e2)) / 2, H / 2 + 62, 0, 22, C_MUTED, e2);
    return;
  }
  int first = fsel - vis / 2;
  if (first > nfavs - vis) first = nfavs - vis;
  if (first < 0) first = 0;
  float want = fsel - first;
  sel_y += (want - sel_y) * .35f;
  if (fabsf(want - sel_y) < .01f) sel_y = want;
  rrect(MX - 6, top + sel_y * ROW_H - 6, cw + 12, ROW_H - 10 + 12, 20, P->fglow, .22f, 5);
  rrect(MX, top + sel_y * ROW_H, cw, ROW_H - 10, 16, P->card_f, 1, 0);
  rrect(MX, top + sel_y * ROW_H, cw, ROW_H - 10, 16, P->focus, 1, 2.5f);
  for (int i = first; i < nfavs && i < first + vis; i++) {
    fav_t *f = &favs[i];
    int ry = top + (i - first) * ROW_H, cy = ry + (ROW_H - 10) / 2;
    if (i != fsel) rrect(MX, ry, cw, ROW_H - 10, 16, P->card, .55f, 0);
    item_t t = {(char *)base_of(f->path), f->dir ? 'd' : 'f', -1, 0, 0, 0, 0, 0, 0};
    draw_icon(MX + 22, cy - 22, kind_of(&t), f->gone);
    int x = MX + 86, pw = 700;
    clip_text(cl, sizeof cl, 1, 26, strcmp(f->path, "/") ? base_of(f->path) : "Root", cw - (x - MX) - pw - 24);
    text(x, cy - 17, 1, 26, f->gone ? C_SUB : C_TEXT, cl);
    char par[1024];
    parent_of(f->path, par, sizeof par);
    clip_text(cl, sizeof cl, 0, 20, f->gone ? "Not there any more" : par, pw);
    text(MX + cw - 24 - text_w(0, 20, cl), cy - 12, 0, 20, f->gone ? 0xffb37a : C_SUB, cl);
  }
}
static void footer_favs(void) {
  int x = MX, y = H - MY - 30;
  if (nfavs) ghint(&x, y, 'x', "Open"), ghint(&x, y, 's', "Menu");
  ghint_back(&x, y);
  ghint(&x, y, '1', "Tabs");
  ghint(&x, y, 'O', "Settings");
}
static void selall_icon(int x, int y, unsigned col) { // 30 x 30: three ticked rows
  for (int i = 0; i < 3; i++) {
    int ry = y + i * 11;
    rrect(x, ry, 8, 8, 2.5f, col, 1, 0);
    rrect(x + 12, ry + 2, 18, 4, 2, col, .85f, 0);
  }
}
// with anything ticked, the counter in the corner becomes "2 of 16 | (T) Hold to select all", filling while held
static void selall_chip(int fy) {
  char b[64];
  snprintf(b, sizeof b, "%d of %d", nmarked, nitems);
  const char *lab = nitems && nmarked == nitems ? "Hold to clear all" : "Hold to select all";
  int w = 18 + 30 + 12 + text_w(1, 19, b) + 22 + 34 + 10 + text_w(0, 19, lab) + 22, h = 54, x = W - MX - w, y = fy - 10;
  rrect(x, y, w, h, 27, P->card_f, .97f, 0);
  if (sa_fill > 0) rrect(x, y, fmaxf(h, w * sa_fill), h, 27, C_ACC, .28f, 0);
  rrect(x, y, w, h, 27, C_ACC, .7f, 1.5f);
  selall_icon(x + 18, y + 12, C_ACC);
  int cx = x + 60;
  cx += text(cx, y + 15, 1, 19, C_TEXT, b) + 22;
  rrect(cx - 12, y + 12, 1.5f, 30, 0, C_LINE, 1, 0);
  glyph_key(cx, y + 11, 32, 't');
  text(cx + 44, y + 16, 0, 19, sa_fill > 0 ? C_TEXT : C_MUTED, lab);
}
static void footer_browse(void) {
  int x = MX, y = H - MY - 30;
  ghint(&x, y, 'x', nitems && isdir(&items[sel]) ? "Open" : "Open");
  ghint_back(&x, y);
  ghint(&x, y, 's', "Menu");
  ghint(&x, y, 't', "Select");
  ghint(&x, y, 'L', set.grid ? "List" : "Grid");
  ghint(&x, y, 'R', "Sort");
  ghint(&x, y, 'O', "Settings");
  char c[40];
  snprintf(c, sizeof c, "%d / %d", nitems ? sel + 1 : 0, nitems);
  if (nmarked || sa_fill > 0) selall_chip(y);
  else footer_counter(c);
}

// ---- actions ----
static int targets(char ***out) { // marked items, or the focused one
  int n = 0;
  char **t = calloc(nmarked ? nmarked : 1, sizeof *t);
  if (!t) return 0;
  for (int i = 0; i < nitems; i++)
    if (items[i].mark || (!nmarked && i == sel)) {
      char p[1300];
      join(p, sizeof p, cwd, items[i].name);
      t[n++] = strdup(p);
    }
  *out = t;
  return n;
}
static void free_list(char **l, int n) {
  for (int i = 0; i < n; i++) free(l[i]);
  free(l);
}
static void clear_marks(void) {
  for (int i = 0; i < nitems; i++) items[i].mark = 0;
  nmarked = 0;
}
static void do_clip(int move) {
  if (!nitems) return;
  free_list(clip, nclip);
  nclip = targets(&clip);
  clip_move = move;
  snprintf(clip_dir, sizeof clip_dir, "%s", cwd);
  clear_marks();
  say("%s %d item%s \x82 open a folder, then Square \x82 Paste", move ? "Cut" : "Copied", nclip, nclip == 1 ? "" : "s");
}
static char *form_paths(char **l, int n, const char *dest) { // paths=a%0Ab&dest=...
  size_t cap = 64;
  for (int i = 0; i < n; i++) cap += strlen(l[i]) * 3 + 4;
  cap += dest ? strlen(dest) * 3 + 8 : 0;
  char *f = malloc(cap), e[3800];
  if (!f) return 0;
  size_t k = snprintf(f, cap, "paths=");
  for (int i = 0; i < n; i++) {
    urlenc(l[i], e, sizeof e);
    k += snprintf(f + k, cap - k, "%s%s", i ? "%0A" : "", e);
  }
  if (dest) {
    urlenc(dest, e, sizeof e);
    snprintf(f + k, cap - k, "&dest=%s", e);
  }
  return f;
}
static void do_paste(void) {
  if (!nclip) return;
  if (clip_move && !strcmp(clip_dir, cwd)) {
    say_err("They're already in this folder");
    return;
  }
  char *f = form_paths(clip, nclip, cwd);
  if (f && !api_post(clip_move ? "/api/move" : "/api/copy", f, 0, 0)) {
    if (clip_move) free_list(clip, nclip), clip = 0, nclip = 0;
    job.shown = 1, modal = M_JOB;
    poll_job();
  }
  free(f);
}
static void do_delete(void) {
  char **l;
  int n = targets(&l);
  char *f = form_paths(l, n, 0);
  if (f && !api_post("/api/delete", f, 0, 0)) {
    clear_marks();
    job.shown = 1, modal = M_JOB;
    poll_job();
  }
  free(f);
  free_list(l, n);
}
static void ask_delete(void) {
  if (!nitems) return;
  int n = nmarked ? nmarked : 1;
  if (n == 1) {
    const char *nm = items[sel].name;
    for (int i = 0; nmarked && i < nitems; i++)
      if (items[i].mark) nm = items[i].name;
    snprintf(conf_title, sizeof conf_title, "Delete \"%s\"?", nm);
  } else snprintf(conf_title, sizeof conf_title, "Delete %d items?", n);
  snprintf(conf_text, sizeof conf_text, "From %s. This can't be undone.", cwd);
  if (!set.confirm) {
    do_delete();
    return;
  }
  conf_kind = C_DELETE, modal = M_CONFIRM;
}
static void start_archive(int fmt, const char *name, const char *pw);
static void start_extract(const char *form, const char *pw);
static int text_open(const char *path, int force);
static struct {
  char path[1300], name[300];
  int mode, isdir, recursive, r, c; // r: 0..2 owner/group/everyone, 3 special bits, 4 the number, 5 everything inside
} perm;
static void kb_submit(void) {
  char form[5000], e[3000], p[1300], out[2000];
  kb_open = 0;
  if (kb_target == KT_LINE || kb_target == KT_INSERT) {
    char *nl = strdup(kb_buf);
    if (!nl) return;
    if (kb_target == KT_LINE) {
      free(lines[tsel]);
      lines[tsel] = nl;
    } else {
      if (nlines == lcap) {
        char **n2 = realloc(lines, (lcap = lcap ? lcap * 2 : 64) * sizeof *lines);
        if (!n2) {
          free(nl);
          return;
        }
        lines = n2;
      }
      int at = nlines ? tsel + 1 : 0;
      memmove(lines + at + 1, lines + at, (nlines - at) * sizeof *lines);
      lines[at] = nl;
      nlines++;
      tsel = at;
    }
    tdirty = 1;
    return;
  }
  if (kb_target == KT_ARCPW || kb_target == KT_XPW) { // passwords can hold anything, even /
    if (!kb_buf[0]) {
      say_err("Type a password, or press Circle to cancel");
      kb_open = 1;
      return;
    }
    if (kb_target == KT_ARCPW) start_archive(arc_fmt, arc_name, kb_buf);
    else start_extract(x_form, kb_buf);
    memset(kb_buf, 0, sizeof kb_buf);
    return;
  }
  if (kb_target == KT_MODE) { // permissions as a number: 1 to 4 octal digits
    size_t n = strlen(kb_buf);
    if (!n || n > 4 || strspn(kb_buf, "01234567") != n) {
      say_err("The mode is 1 to 4 digits from 0 to 7, like 0755");
      kb_open = 1;
      return;
    }
    perm.mode = (int)strtol(kb_buf, 0, 8);
    return;
  }
  if (!kb_buf[0]) return;
  if (strchr(kb_buf, '/')) {
    say_err("Names can't contain /");
    return;
  }
  if (kb_target == KT_ARCHIVE) { // the new archive, in this folder
    if (FMTS[arc_fmt].pw) { // then its password
      snprintf(arc_name, sizeof arc_name, "%s", kb_buf);
      char t[700];
      snprintf(t, sizeof t, "Password for %s (AES-256: 7-Zip, WinRAR and the like open it)", arc_name);
      open_kb(KT_ARCPW, t, "");
      return;
    }
    start_archive(arc_fmt, kb_buf, 0);
    return;
  }
  if (kb_target == KT_RENAME) {
    if (!nitems || !strcmp(kb_buf, items[sel].name)) return;
    join(p, sizeof p, cwd, items[sel].name);
    urlenc(p, e, sizeof e);
    char en[1200];
    urlenc(kb_buf, en, sizeof en);
    snprintf(form, sizeof form, "path=%s&name=%s", e, en);
    if (!api_post("/api/rename", form, out, sizeof out)) {
      say("Renamed \x82 Square \x82 Undo puts it back");
      char nm[600];
      snprintf(nm, sizeof nm, "%s", kb_buf);
      load_dir(cwd, nm);
    }
    return;
  }
  join(p, sizeof p, cwd, kb_buf);
  urlenc(p, e, sizeof e);
  snprintf(form, sizeof form, "path=%s", e);
  if (!api_post(kb_target == KT_NEWDIR ? "/api/mkdir" : "/api/newfile", form, 0, 0)) {
    char nm[600];
    snprintf(nm, sizeof nm, "%s", kb_buf);
    load_dir(cwd, nm);
    if (kb_target == KT_NEWFILE) { // straight into the editor: X edits a line, Triangle adds one, Options saves
      if (!text_open(p, 1)) say("New file \x82 X edits a line, Triangle adds one, Options saves");
    } else say("Folder created");
  }
}

// ---- folder picker (Copy to, Move to, Extract to) ----
static void panel(int x, int y, int w, int h);
static void free_list(char **l, int n);
static char *form_paths(char **l, int n, const char *dest);
static void clear_marks(void);
enum { P_COPYTO, P_MOVETO, P_XTO };
static struct {
  int purpose, n, sel, npaths, cap;
  char cwd[1024], **names, **paths; // folders shown; what gets copied / moved / extracted
  float sel_y;
} pick;
static void pick_free_names(void) {
  for (int i = 0; i < pick.n; i++) free(pick.names[i]);
  pick.n = 0;
}
static int pick_load(const char *dir_in, const char *focus) {
  char dir[1024], e[2200], q[2400];
  snprintf(dir, sizeof dir, "%s", dir_in);
  urlenc(dir, e, sizeof e);
  snprintf(q, sizeof q, "/api/ls?tsv=1&path=%s", e);
  resp_t r;
  if (http("GET", q, 0, 0, 0, 64 << 20, &r) || r.status != 200) {
    say_err("Can't open that folder");
    resp_free(&r);
    return -1;
  }
  pick_free_names();
  for (char *l = r.body; l && *l;) {
    char *nl = strchr(l, '\n');
    if (nl) *nl = 0;
    char *t = l;
    for (int k = 0; k < 4 && t; k++) t = strchr(t + 1, '\t');
    if (t && (l[0] == 'd' || l[0] == 'D')) {
      if (pick.n == pick.cap) {
        char **nn = realloc(pick.names, (pick.cap = pick.cap ? pick.cap * 2 : 128) * sizeof *nn);
        if (!nn) break;
        pick.names = nn;
      }
      tsv_unesc(t + 1);
      if ((pick.names[pick.n] = strdup(t + 1))) pick.n++;
    }
    l = nl ? nl + 1 : 0;
  }
  resp_free(&r);
  snprintf(pick.cwd, sizeof pick.cwd, "%s", dir);
  pick.sel = 0;
  for (int i = 0; focus && i < pick.n; i++)
    if (!strcmp(pick.names[i], focus)) pick.sel = i;
  return 0;
}
static int targets(char ***out);
static void open_picker(int purpose) {
  free_list(pick.paths, pick.npaths);
  pick.npaths = targets(&pick.paths);
  if (!pick.npaths) return;
  pick.purpose = purpose;
  if (pick_load(cwd, 0)) return;
  scr = SCR_PICK, shown = 0, pick.sel_y = 0;
}
static void pick_choose(void) { // copy / move / extract into the folder the picker shows
  char *f = 0, e[3000];
  if (pick.purpose == P_XTO) {
    char e2[3000];
    urlenc(pick.paths[0], e, sizeof e);
    urlenc(pick.cwd, e2, sizeof e2);
    size_t n = strlen(e) + strlen(e2) + 32;
    if ((f = malloc(n))) snprintf(f, n, "path=%s&dest=%s", e, e2);
  } else f = form_paths(pick.paths, pick.npaths, pick.cwd);
  const char *api = pick.purpose == P_COPYTO ? "/api/copy" : pick.purpose == P_MOVETO ? "/api/move" : "/api/extract";
  if (f && pick.purpose == P_XTO) {
    scr = SCR_BROWSE, shown = 0;
    start_extract(f, 0);
  } else if (f && !api_post(api, f, 0, 0)) {
    scr = SCR_BROWSE, shown = 0;
    if (pick.purpose == P_MOVETO) clear_marks();
    job.shown = 1, modal = M_JOB;
    poll_job();
  }
  free(f);
}
static void draw_pick(void) {
  const char *t = pick.purpose == P_COPYTO ? "Copy to" : pick.purpose == P_MOVETO ? "Move to" : "Extract to";
  char sub[400], cl[1200];
  if (pick.npaths == 1) snprintf(sub, sizeof sub, "%s \"%s\" \x82 open a folder, then choose it", t, base_of(pick.paths[0]));
  else snprintf(sub, sizeof sub, "%s: %d items \x82 open a folder, then choose it", t, pick.npaths);
  clip_text(cl, sizeof cl, 0, 19, sub, W - 2 * MX - 600);
  header(t, cl);
  // drives
  int y = MY + 128, x = MX, cp = -1;
  size_t bl = 0;
  for (int i = 0; i < nplaces; i++) {
    size_t l = strlen(places[i].path);
    int in = !strcmp(places[i].path, "/") || (!strncmp(pick.cwd, places[i].path, l) && (pick.cwd[l] == '/' || !pick.cwd[l]));
    if (in && (cp < 0 || l > bl)) cp = i, bl = l;
  }
  x += bumper(x, y + 16, 40, "L1") + 26;
  for (int i = 0; i < nplaces; i++) {
    int on = i == cp, w = text_w(on, 26, places[i].label);
    text(x, y, on, 26, on ? C_TEXT : C_MUTED, places[i].label);
    if (on) rrect(x + w / 2 - 14, y + 40, 28, 4, 2, C_ACC, 1, 0);
    x += w + 40;
  }
  bumper(x - 14, y + 16, 40, "R1");
  draw_icon(MX, MY + 182, K_DIR, 0);
  clip_text(cl, sizeof cl, 1, 24, pick.cwd, W - 2 * MX - 200);
  text(MX + 58, MY + 190, 1, 24, C_TEXT, cl);
  int top = LIST_TOP, vis = vis_rows(), cw = W - 2 * MX;
  if (!pick.n) {
    const char *e1 = "No folders in here", *e2 = "Press Square to choose this folder";
    text((W - text_w(1, 30, e1)) / 2, H / 2 + 10, 1, 30, C_TEXT, e1);
    text((W - text_w(0, 22, e2)) / 2, H / 2 + 58, 0, 22, C_MUTED, e2);
  } else {
    int first = pick.sel - vis / 2;
    if (first > pick.n - vis) first = pick.n - vis;
    if (first < 0) first = 0;
    pick.sel_y += ((pick.sel - first) - pick.sel_y) * .35f;
    rrect(MX - 6, top + pick.sel_y * ROW_H - 6, cw + 12, ROW_H + 2, 20, P->fglow, .22f, 5);
    rrect(MX, top + pick.sel_y * ROW_H, cw, ROW_H - 10, 16, P->card_f, 1, 0);
    rrect(MX, top + pick.sel_y * ROW_H, cw, ROW_H - 10, 16, P->focus, 1, 2.5f);
    for (int i = first; i < pick.n && i < first + vis; i++) {
      int ry = top + (i - first) * ROW_H, cy = ry + (ROW_H - 10) / 2;
      if (i != pick.sel) rrect(MX, ry, cw, ROW_H - 10, 16, P->card, .55f, 0);
      draw_icon(MX + 22, cy - 22, K_DIR, 0);
      clip_text(cl, sizeof cl, 1, 26, pick.names[i], cw - 160);
      text(MX + 86, cy - 17, 1, 26, C_TEXT, cl);
    }
  }
  int fx = MX, fy = H - MY - 30;
  ghint(&fx, fy, 'x', "Open folder");
  ghint_back(&fx, fy);
  ghint(&fx, fy, 's', pick.purpose == P_COPYTO ? "Copy here" : pick.purpose == P_MOVETO ? "Move here" : "Extract here");
  ghint(&fx, fy, 't', "Cancel");
}
static void pick_press(unsigned k) {
  if (k == PAD_UP && pick.n) pick.sel = (pick.sel + pick.n - 1) % pick.n;
  else if (k == PAD_DOWN && pick.n) pick.sel = (pick.sel + 1) % pick.n;
  else if (k == PAD_CROSS && pick.n) {
    char p[1300];
    join(p, sizeof p, pick.cwd, pick.names[pick.sel]);
    pick_load(p, 0);
    pick.sel_y = 0;
  } else if (k == PAD_CIRCLE && strcmp(pick.cwd, "/")) {
    char par[1024], me[256];
    snprintf(me, sizeof me, "%s", base_of(pick.cwd));
    parent_of(pick.cwd, par, sizeof par);
    pick_load(par, me);
  } else if (k == PAD_SQUARE) pick_choose();
  else if (k == PAD_TRIANGLE) scr = SCR_BROWSE, shown = 0;
  else if ((k == PAD_L1 || k == PAD_R1) && nplaces) {
    int cp = 0;
    for (int i = 0; i < nplaces; i++)
      if (!strncmp(pick.cwd, places[i].path, strlen(places[i].path))) cp = i;
    cp = (cp + (k == PAD_L1 ? -1 : 1) + nplaces) % nplaces;
    pick_load(places[cp].path, 0);
  }
}

// ---- compress / extract ----
static void start_archive(int fmt, const char *name, const char *pw) {
  char **l;
  int n = targets(&l);
  char *f = form_paths(l, n, cwd), e[1200], ep[1100] = "";
  size_t fl = f ? strlen(f) : 0;
  urlenc(name, e, sizeof e);
  if (pw) urlenc(pw, ep, sizeof ep);
  size_t extra = strlen(e) + strlen(ep) + 32;
  char *g = f ? realloc(f, fl + extra) : 0;
  if (g) {
    snprintf(g + fl, extra, "&name=%s&z=%s%s%s", e, FMTS[fmt].z, ep[0] ? "&pw=" : "", ep);
    if (!api_post(FMTS[fmt].api, g, 0, 0)) {
      clear_marks();
      job.shown = 1, modal = M_JOB;
      poll_job();
    }
  } else free(f);
  free(g);
  free_list(l, n);
}
static void ask_archive(int fmt) { // name it first
  char nm[600], stem[600];
  int n = nmarked ? nmarked : 1;
  if (n == 1) {
    const char *b = items[sel].name;
    for (int i = 0; nmarked && i < nitems; i++)
      if (items[i].mark) b = items[i].name;
    snprintf(stem, sizeof stem, "%s", b);
    char *d = strrchr(stem, '.');
    if (d && d != stem && !isdir(&items[sel])) *d = 0; // "notes.txt" -> "notes.zip"
  } else snprintf(stem, sizeof stem, "%s", strcmp(cwd, "/") ? base_of(cwd) : "Archive");
  snprintf(nm, sizeof nm, "%s.%s", stem, FMTS[fmt].ext);
  char t[80];
  snprintf(t, sizeof t, "Name of the %s archive", !strcmp(FMTS[fmt].ext, "7z") ? "7z" : FMTS[fmt].ext[0] == 'z' ? "ZIP" : "TAR");
  arc_fmt = fmt;
  open_kb(KT_ARCHIVE, t, nm);
}
// extract (form: path=&dest=), with a password the second time round
static void start_extract(const char *form, const char *pw) {
  char f[7400], ep[1100] = "";
  if (form != x_form) snprintf(x_form, sizeof x_form, "%s", form);
  if (pw) urlenc(pw, ep, sizeof ep);
  snprintf(f, sizeof f, "%s%s%s", x_form, ep[0] ? "&pw=" : "", ep);
  int before = job.id;
  if (!api_post("/api/extract", f, 0, 0)) {
    x_after = before, x_wait = 1;
    job.shown = 1, modal = M_JOB;
    poll_job();
  }
  memset(f, 0, sizeof f);
}
static void ask_xpw(int again) {
  const char *p = strstr(x_form, "path="), *e = p ? strchr(p, '&') : 0;
  char enc[3000], nm[3000], t[700];
  snprintf(enc, sizeof enc, "%.*s", p ? (int)((e ? e : p + strlen(p)) - p - 5) : 0, p ? p + 5 : "");
  size_t k = 0; // url-decode the name for the title
  for (const char *c = enc; *c && k + 1 < sizeof nm; c++)
    if (*c == '%' && c[1] && c[2]) {
      char h[3] = {c[1], c[2], 0};
      nm[k++] = (char)strtol(h, 0, 16), c += 2;
    } else nm[k++] = *c;
  nm[k] = 0;
  snprintf(t, sizeof t, again ? "Wrong password for %s. Try again" : "%s is locked. Type its password", base_of(nm));
  if (again) sfx(SFX_ERROR);
  open_kb(KT_XPW, t, "");
}
static void extract_here(void) {
  char p[1300], e[3000], e2[3000], f[6100];
  join(p, sizeof p, cwd, items[sel].name);
  urlenc(p, e, sizeof e);
  urlenc(cwd, e2, sizeof e2);
  snprintf(f, sizeof f, "path=%s&dest=%s", e, e2);
  start_extract(f, 0);
}

// ---- info card ----
static void hash_start(void) {
  char e[3000], f[3100];
  urlenc(info.path, e, sizeof e);
  snprintf(f, sizeof f, "path=%s", e);
  if (api_post(info.isdir ? "/api/size" : "/api/hash", f, 0, 0)) {
    snprintf(info.note, sizeof info.note, "Busy: another job is running");
    toast[0] = 0;
    return;
  }
  poll_job();
  info.job_id = job.id;
  snprintf(info.note, sizeof info.note, info.isdir ? "Counting\x80" : "Working out\x80");
}
static void open_info(void) {
  if (!nitems) return;
  char p[1300], e[3000], q[3100];
  join(p, sizeof p, cwd, items[sel].name);
  urlenc(p, e, sizeof e);
  snprintf(q, sizeof q, "/api/info?path=%s", e);
  resp_t r;
  if (http("GET", q, 0, 0, 0, 1 << 16, &r) || r.status != 200) {
    resp_free(&r);
    say_err("Couldn't read the details");
    return;
  }
  memset(&info, 0, sizeof info);
  snprintf(info.path, sizeof info.path, "%s", p);
  jstr(r.body, "name", info.name, sizeof info.name);
  jstr(r.body, "type", info.type, sizeof info.type);
  jstr(r.body, "mode", info.mode, sizeof info.mode);
  jstr(r.body, "target", info.target, sizeof info.target);
  info.size = jnum(r.body, "size"), info.created = jnum(r.body, "created"), info.modified = jnum(r.body, "modified");
  info.accessed = jnum(r.body, "accessed"), info.uid = jnum(r.body, "uid"), info.gid = jnum(r.body, "gid");
  info.system = (int)jnum(r.body, "system"), info.link = (int)jnum(r.body, "link");
  info.isdir = !strcmp(info.type, "folder");
  resp_free(&r);
  info.open = 1, info.job_id = -1, modal = M_INFO;
  if (info.isdir || info.size <= (512LL << 20)) hash_start(); // big files: on request (it takes a while)
}
static void rwx(const char *oct, char *o) { // "0755" -> "rwxr-xr-x"
  int m = (int)strtol(oct, 0, 8);
  for (int i = 0; i < 9; i++) o[i] = (m >> (8 - i)) & 1 ? "rwx"[i % 3] : '-';
  o[9] = 0;
}
static void draw_info(void) {
  int w = 1240, h = 210 + (info.isdir ? 6 : 7) * 52 + (info.link ? 52 : 0), x = (W - w) / 2, y = (H - h) / 2;
  panel(x, y, w, h);
  char cl[1300], a[64], v[400];
  int kind = info.isdir ? K_DIR : K_FILE;
  if (!info.isdir) {
    item_t t = {info.name, 'f', info.size, 0, 0, 0, 0, 0, 0};
    kind = kind_of(&t);
  }
  draw_icon(x + 40, y + 34, kind, 0);
  clip_text(cl, sizeof cl, 1, 30, info.name, w - 200);
  text(x + 104, y + 36, 1, 30, C_TEXT, cl);
  if (info.system) {
    int lx = x + 104 + text_w(1, 30, cl) + 18;
    lock_glyph(lx, y + 44, 0xffb37a);
    text(lx + 28, y + 42, 1, 20, 0xffb37a, "PS5 system item \x82 read-only in FileDen");
  }
  struct { const char *k; char v[400]; } rows[10];
  int n = 0;
  snprintf(rows[n].v, 400, "%s", info.path), rows[n++].k = "Where";
  if (info.isdir) snprintf(rows[n].v, 400, "%s", info.sizes[0] ? info.sizes : info.note), rows[n++].k = "Size";
  else snprintf(rows[n].v, 400, "%s (%lld bytes)", human(info.size, a, sizeof a), info.size), rows[n++].k = "Size";
  if (!info.isdir)
    snprintf(rows[n].v, 400, "%s", info.sha[0] ? info.sha : info.note[0] ? info.note : "Press X to work it out (large file)"),
        rows[n++].k = "SHA-256";
  snprintf(rows[n].v, 400, "%s", info.created > 0 ? when(info.created, a, sizeof a) : "Not recorded"), rows[n++].k = "Created";
  snprintf(rows[n].v, 400, "%s", when(info.modified, a, sizeof a)), rows[n++].k = "Last modified";
  snprintf(rows[n].v, 400, "%s", when(info.accessed, a, sizeof a)), rows[n++].k = "Last opened";
  char r9[12];
  rwx(info.mode, r9);
  snprintf(rows[n].v, 400, "%s  (%s) \x82 owner %lld, group %lld", r9, info.mode, info.uid, info.gid), rows[n++].k = "Permissions";
  if (info.link) snprintf(rows[n].v, 400, "Link to %s", info.target), rows[n++].k = "Link";
  for (int i = 0; i < n; i++) {
    int ry = y + 110 + i * 52;
    text(x + 40, ry, 0, 21, C_SUB, rows[i].k);
    int f = !strcmp(rows[i].k, "SHA-256") && info.sha[0] ? 0 : 1;
    clip_text(v, sizeof v, f, f ? 22 : 20, rows[i].v, w - 330);
    text(x + 260, ry - 1, f, f ? 22 : 20, C_TEXT, v);
  }
  int bx = x + 40, by = y + h - 64;
  if (!info.isdir && !info.sha[0] && info.job_id < 0) ghint(&bx, by, 'x', "Work out SHA-256");
  ghint(&bx, by, 'o', "Close");
}

// ---- permissions card ----
static void open_perm(void) {
  if (!nitems) return;
  char p[1300], e[3000], q[3100], m[16] = "";
  join(p, sizeof p, cwd, items[sel].name);
  urlenc(p, e, sizeof e);
  snprintf(q, sizeof q, "/api/info?path=%s", e);
  resp_t r;
  if (http("GET", q, 0, 0, 0, 1 << 16, &r) || r.status != 200) {
    resp_free(&r);
    say_err("Couldn't read the permissions");
    return;
  }
  jstr(r.body, "mode", m, sizeof m);
  resp_free(&r);
  memset(&perm, 0, sizeof perm);
  snprintf(perm.path, sizeof perm.path, "%s", p);
  snprintf(perm.name, sizeof perm.name, "%s", items[sel].name);
  perm.mode = (int)strtol(m, 0, 8) & 07777;
  perm.isdir = isdir(&items[sel]);
  modal = M_PERM;
}
static void perm_apply(void) {
  char e[3000], f[3200], out[200];
  urlenc(perm.path, e, sizeof e);
  snprintf(f, sizeof f, "path=%s&mode=%o&recursive=%d", e, perm.mode, perm.isdir && perm.recursive);
  if (api_post("/api/chmod", f, out, sizeof out)) return;
  modal = M_NONE;
  if (strstr(out, "\"job\":true")) {
    job.shown = 1, modal = M_JOB;
    poll_job();
  } else say("Permissions set to %04o", perm.mode);
}
static void draw_perm(void) {
  int rows = perm.isdir ? 6 : 5, w = 1060, h = 250 + rows * 70 + (perm.isdir ? 0 : 40), x = (W - w) / 2, y = (H - h) / 2;
  panel(x, y, w, h);
  char cl[400], t[80], r9[12], oc[8];
  snprintf(t, sizeof t, "Permissions: %s", perm.name);
  clip_text(cl, sizeof cl, 1, 28, t, w - 330);
  text(x + 40, y + 34, 1, 28, C_TEXT, cl);
  snprintf(oc, sizeof oc, "%04o", perm.mode);
  rwx(oc, r9);
  snprintf(t, sizeof t, "%s  %s", oc, r9);
  text(x + w - 40 - text_w(1, 26, t), y + 36, 1, 26, C_ACC, t);
  static const char *const who[] = {"Owner", "Group", "Everyone", "Special"}, *const what[] = {"Read", "Write", "Run / open"},
                           *const spec[] = {"Set user ID", "Set group ID", "Sticky"};
  int gx = x + 290, gy = y + 100, cw = 236, rh = 70;
  for (int c = 0; c < 3; c++) text(gx + c * cw + 20, gy, 0, 20, C_SUB, what[c]);
  for (int r = 0; r < 4; r++) {
    int ry = gy + 36 + r * rh;
    text(x + 40, ry + 14, 1, 24, r == 3 ? C_MUTED : C_TEXT, who[r]);
    for (int c = 0; c < 3; c++) {
      int bit = r < 3 ? 1 << (8 - (r * 3 + c)) : 04000 >> c, on = (perm.mode & bit) != 0, f = perm.r == r && perm.c == c;
      int bx = gx + c * cw + 20;
      if (f) rrect(bx - 12, ry - 6, cw - 20, 58, 14, P->fglow, .25f, 4), rrect(bx - 8, ry - 2, cw - 28, 50, 12, P->focus, 1, 2);
      tick_box(bx, ry + 23, on);
      text(bx + 42, ry + 12, 0, 20, on ? C_TEXT : C_SUB, r == 3 ? spec[c] : on ? "Yes" : "No");
    }
  }
  int ny = gy + 36 + 4 * rh; // the number, typed
  if (perm.r == 4) rrect(x + 28, ny - 8, w - 56, 58, 14, P->focus, 1, 2);
  text(x + 40, ny + 6, 1, 24, C_TEXT, "Mode number");
  rrect(gx + 20, ny - 2, 150, 46, 10, P->ph1, 1, 0);
  rrect(gx + 20, ny - 2, 150, 46, 10, perm.r == 4 ? C_ACC : C_LINE, 1, 1.5f);
  text_c(gx + 95, ny + 21, 1, 26, C_TEXT, oc);
  text(gx + 196, ny + 9, 0, 20, C_SUB, perm.r == 4 ? "X to type any mode, like 0755 or 4777" : "Any mode from 0000 to 7777");
  int ay = ny + rh;
  if (perm.isdir) {
    int f = perm.r == 5;
    if (f) rrect(x + 28, ay - 8, w - 56, 58, 14, P->focus, 1, 2);
    tick_box(x + 44, ay + 21, perm.recursive);
    text(x + 92, ay + 9, 1, 22, C_TEXT, "Also change everything inside this folder");
  } else text(x + 40, ay + 9, 0, 20, C_SUB, "Apps on the PS5 need 0777 (everything Yes): Square sets it");
  int bx = x + 40, by = y + h - 64;
  ghint(&bx, by, 'x', perm.r == 4 ? "Type the number" : "Change");
  ghint(&bx, by, 't', "Type the number");
  ghint(&bx, by, 's', "Set 0777");
  ghint(&bx, by, 'O', "Apply");
  ghint(&bx, by, 'o', "Cancel");
}
static void perm_type(void) {
  char oc[8];
  snprintf(oc, sizeof oc, "%04o", perm.mode);
  open_kb(KT_MODE, "Permissions as a number (0000 to 7777)", oc);
}
static void perm_press(unsigned k) {
  int rows = perm.isdir ? 6 : 5;
  if (k == PAD_UP) perm.r = (perm.r + rows - 1) % rows;
  else if (k == PAD_DOWN) perm.r = (perm.r + 1) % rows;
  else if (k == PAD_LEFT && perm.r < 4) perm.c = (perm.c + 2) % 3;
  else if (k == PAD_RIGHT && perm.r < 4) perm.c = (perm.c + 1) % 3;
  else if (k == PAD_CROSS) {
    if (perm.r == 5) perm.recursive = !perm.recursive;
    else if (perm.r == 4) perm_type();
    else if (perm.r == 3) perm.mode ^= 04000 >> perm.c;
    else perm.mode ^= 1 << (8 - (perm.r * 3 + perm.c));
  } else if (k == PAD_TRIANGLE) perm_type();
  else if (k == PAD_SQUARE) perm.mode = 0777;
  else if (k == PAD_OPTIONS || k == PAD_R1) perm_apply();
  else if (k == PAD_CIRCLE) modal = M_NONE;
}

// ---- image viewer (name and details on top, the picture in a frame; Triangle: full screen) ----
#define VIEW_TOP (MY + 86)
#define VIEW_BOT (H - MY - 66)
static void img_box(int *x, int *y, int *w, int *h) {
  if (img_full || img_actual) *x = 0, *y = 0, *w = W, *h = H;
  else *x = MX, *y = VIEW_TOP, *w = W - 2 * MX, *h = VIEW_BOT - VIEW_TOP;
}
static void img_build(void) { // compose the frame for the current mode, zoom and pan
  if (!img_frame && !(img_frame = malloc((size_t)W * H * 4))) return;
  int full = img_full || img_actual;
  if (full) {
    unsigned bg = RGB(0x0c, 0x0d, 0x0f);
    for (size_t i = 0; i < (size_t)W * H; i++) img_frame[i] = bg;
  } else memcpy(img_frame, bgbuf, (size_t)W * H * 4);
  int bx, by, bw, bh;
  img_box(&bx, &by, &bw, &bh);
  if (!full) { // the frame the picture sits in
    unsigned *keep = fb;
    fb = img_frame;
    rrect(bx, by, bw, bh, 18, P->ph1, .9f, 0);
    rrect(bx, by, bw, bh, 18, C_LINE, 1, 1.5f);
    fb = keep;
  }
  if (!img_ok) return;
  if (img_actual) { // 1:1, panned
    int ox = img_w < W ? (W - img_w) / 2 : 0, oy = img_h < H ? (H - img_h) / 2 : 0;
    for (int y = 0; y < H; y++) {
      int sy = y - oy + img_px_y;
      if (sy < 0 || sy >= img_h) continue;
      for (int x = 0; x < W; x++) {
        int sx = x - ox + img_px_x;
        if (sx < 0 || sx >= img_w) continue;
        const unsigned char *s = img_px + ((size_t)sy * img_w + sx) * 4;
        img_frame[(size_t)y * W + x] = RGB(s[0], s[1], s[2]);
      }
    }
    return;
  }
  int pad = full ? 0 : 14;
  bx += pad, by += pad, bw -= 2 * pad, bh -= 2 * pad;
  float sc = fminf((float)bw / img_w, (float)bh / img_h);
  if (sc > 4) sc = 4; // small images: up to 4x
  int dw = (int)(img_w * sc + .5f), dh = (int)(img_h * sc + .5f), ox = bx + (bw - dw) / 2, oy = by + (bh - dh) / 2;
  if (dw < 1) dw = 1;
  if (dh < 1) dh = 1;
  float inv = 1 / sc;
  for (int y = 0; y < dh; y++) {
    if (oy + y < 0 || oy + y >= H) continue;
    float fy0 = y * inv, fy1 = (y + 1) * inv;
    for (int x = 0; x < dw; x++) {
      if (ox + x < 0 || ox + x >= W) continue;
      float fx0 = x * inv, fx1 = (x + 1) * inv;
      unsigned r = 0, g = 0, b = 0, n = 0;
      if (sc < 1) { // shrink: average a box of up to 6 x 6 samples
        int sx0 = (int)fx0, sx1 = (int)ceilf(fx1), sy0 = (int)fy0, sy1 = (int)ceilf(fy1);
        int stx = (sx1 - sx0 + 5) / 6, sty = (sy1 - sy0 + 5) / 6;
        if (stx < 1) stx = 1;
        if (sty < 1) sty = 1;
        for (int sy = sy0; sy < sy1 && sy < img_h; sy += sty)
          for (int sx = sx0; sx < sx1 && sx < img_w; sx += stx) {
            const unsigned char *s = img_px + ((size_t)sy * img_w + sx) * 4;
            r += s[0], g += s[1], b += s[2], n++;
          }
      } else { // grow: nearest pixel (sharp for pixel art and screenshots)
        int sx = (int)((x + .5f) * inv), sy = (int)((y + .5f) * inv);
        if (sx >= img_w) sx = img_w - 1;
        if (sy >= img_h) sy = img_h - 1;
        const unsigned char *s = img_px + ((size_t)sy * img_w + sx) * 4;
        r = s[0], g = s[1], b = s[2], n = 1;
      }
      if (n) img_frame[(size_t)(oy + y) * W + ox + x] = RGB(r / n, g / n, b / n);
    }
  }
}
// the picture from the payload as a JPEG no bigger than mw x mh (it reads JPEG, PNG, GIF, BMP, TGA, PSD, HDR,
// WebP and PS5 HDR .jxr screenshots); 0 or a message in img_msg
static int img_fetch(int idx, int mw, int mh) {
  char p[1300], e[3000], q[3200];
  join(p, sizeof p, cwd, items[idx].name);
  urlenc(p, e, sizeof e);
  snprintf(q, sizeof q, "/api/image?w=%d&h=%d&path=%s", mw, mh, e);
  resp_t r;
  int rc = http("GET", q, 0, 0, 0, MAX_IMAGE_BYTES, &r), w = 0, h = 0, c = 0;
  if (rc == -2) snprintf(img_msg, sizeof img_msg, "Too big to show here");
  else if (rc) snprintf(img_msg, sizeof img_msg, "The background service isn't answering");
  else if (r.status != 200) {
    char m[200] = "";
    jstr(r.body, "error", m, sizeof m);
    snprintf(img_msg, sizeof img_msg, "%s", m[0] ? m : "Can't decode this image");
  } else if (!stbi_info_from_memory((unsigned char *)r.body, (int)r.len, &w, &h, &c) || (unsigned long long)w * h > MAX_IMAGE_PIXELS)
    snprintf(img_msg, sizeof img_msg, "Can't show this image here");
  else {
    unsigned char *px = stbi_load_from_memory((unsigned char *)r.body, (int)r.len, &w, &h, &c, 4);
    if (px) {
      free(img_px);
      img_px = px, img_w = w, img_h = h, img_ok = 1;
      if (r.xw > 0) img_ow = r.xw, img_oh = r.xh;
      resp_free(&r);
      return 0;
    }
    snprintf(img_msg, sizeof img_msg, "Can't decode this image");
  }
  resp_free(&r);
  return -1;
}
static void img_load(int idx) {
  if (idx < 0 || idx >= nitems) return;
  img_idx = idx;
  free(img_px);
  img_px = 0, img_ok = 0, img_actual = 0, img_px_x = img_px_y = 0, img_msg[0] = 0, img_ow = img_oh = 0;
  snprintf(img_name, sizeof img_name, "%s", items[idx].name);
  img_size = items[idx].size, img_mtime = items[idx].mtime;
  img_build();
  if (ext_in(items[idx].name, IMG_WEB)) {
    snprintf(img_msg, sizeof img_msg, "This image type can only be shown in a browser: open http://%s:%d/ on a phone or PC",
             set.ip[0] ? set.ip : "<PS5-IP>", PORT);
    return;
  }
  frame(); // "Loading" while the payload converts it
  img_fetch(idx, W, H);
  img_build();
}
static void img_zoom(void) { // fit <-> actual size (the full picture is fetched for that)
  if (!img_ok) return;
  if (!img_actual) {
    long long px = (long long)img_ow * img_oh;
    if (img_ow > img_w || img_oh > img_h) {
      if (px > (long long)MAX_IMAGE_PIXELS) {
        say_err("Too many pixels to zoom in here: use a phone or PC");
        return;
      }
      snprintf(img_msg, sizeof img_msg, "Loading the full picture\x80");
      say("Loading the full picture\x80");
      frame();
      if (img_fetch(img_idx, 8192, 8192)) {
        say_err(img_msg);
        return;
      }
    }
    img_actual = 1;
    img_px_x = img_w > W ? (img_w - W) / 2 : 0, img_px_y = img_h > H ? (img_h - H) / 2 : 0;
  } else img_actual = 0;
  img_build();
}
static int img_step(int d) { // next / previous image in this folder
  for (int k = 1; k <= nitems; k++) {
    int i = ((img_idx + d * k) % nitems + nitems) % nitems;
    if (kind_of(&items[i]) == K_IMG) {
      sel = i;
      img_load(i);
      return 1;
    }
  }
  return 0;
}
static void draw_image(void) {
  if (img_frame) memcpy(fb, img_frame, (size_t)W * H * 4);
  else memcpy(fb, bgbuf, (size_t)W * H * 4);
  int full = img_full || img_actual, n = 0, pos = 0;
  for (int i = 0; i < nitems; i++)
    if (kind_of(&items[i]) == K_IMG) n++, pos += i <= img_idx;
  char cl[600], info[300], a[64], b[64], c[40];
  if (full) // soft shades so the text reads over any picture
    for (int y = 0; y < 120; y++)
      for (int x = 0; x < W; x++) blendi(fb + (size_t)y * W + x, 0x0c0e10, (120 - y) * 150 / 120);
  clip_text(cl, sizeof cl, 1, 28, img_name, W - 2 * MX - 220);
  text(MX, full ? MY - 14 : MY, 1, 28, C_TEXT, cl);
  int iw = img_ow ? img_ow : img_w, ih = img_ow ? img_oh : img_h;
  const char *e = ext_of(img_name);
  int hdr = !strcasecmp(e, "jxr") || !strcasecmp(e, "wdp") || !strcasecmp(e, "hdp");
  if (img_ok)
    snprintf(info, sizeof info, "%s \x82 %s \x82 %d \xc3\x97 %d%s", when(img_mtime, b, sizeof b), human(img_size, a, sizeof a), iw, ih,
             img_actual ? " \x82 actual size" : "");
  else snprintf(info, sizeof info, "%s \x82 %s", when(img_mtime, b, sizeof b), human(img_size, a, sizeof a));
  int iwid = text(MX, full ? MY + 24 : MY + 40, 0, 20, C_MUTED, info);
  if (hdr) text(MX + iwid + 16, full ? MY + 24 : MY + 40, 0, 20, 0xffb37a, "HDR .jxr shown as JPEG");
  snprintf(c, sizeof c, "%d / %d", pos, n);
  text(W - MX - text_w(1, 22, c), full ? MY - 10 : MY + 4, 1, 22, C_MUTED, c);
  if (!img_ok) {
    int bx, by, bw, bh;
    img_box(&bx, &by, &bw, &bh);
    const char *t = img_msg[0] ? img_msg : "Loading\x80";
    text((W - text_w(0, 24, t)) / 2, by + bh / 2 - 12, 0, 24, img_msg[0] ? 0xffb37a : C_MUTED, t);
  }
  if (full)
    for (int y = H - 110; y < H; y++)
      for (int x = 0; x < W; x++) blendi(fb + (size_t)y * W + x, 0x0c0e10, (y - (H - 110)) * 150 / 110);
  int x = MX, fy = H - MY - 30;
  if (img_actual) ghint(&x, fy, 'v', "Move"), ghint(&x, fy, 'x', "Fit to screen");
  else {
    if (n > 1) ghint(&x, fy, 'h', "Previous / next");
    if (img_ok) ghint(&x, fy, 'x', "Actual size");
    ghint(&x, fy, 't', img_full ? "Show details" : "Full screen");
  }
  ghint(&x, fy, 's', "Info");
  ghint_back(&x, fy);
}

// ---- grid view: folders as tiles, pictures with previews (R3, or Settings) ----
#define GCOLS 7
#define GGAP 20
#define TW ((W - 2 * MX - (GCOLS - 1) * GGAP) / GCOLS)
#define TH 129
#define TILE_H (TH + 60)
#define GROW (TILE_H + GGAP)
static unsigned long long last_input_ms;
static int thumbs_gen;
static void thumbs_reset(void) {
  thumbs_gen++;
  for (int i = 0; i < nthumbs; i++) free(thumbs[i].px);
  free(thumbs);
  thumbs = nitems ? calloc(nitems, sizeof *thumbs) : 0;
  nthumbs = thumbs ? nitems : 0;
}
static int grid_rows(void) { return (H - MY - 96 - LIST_TOP) / GROW; }
static int grid_first(void) { // first row shown, keeping the focus in view
  int rows = (nitems + GCOLS - 1) / GCOLS, vis = grid_rows(), f = sel / GCOLS - vis / 2;
  if (f > rows - vis) f = rows - vis;
  return f < 0 ? 0 : f;
}
// previews come from a worker thread, one at a time: the screen never waits for one
static struct {
  int busy, done, gen, idx, sq; // busy: a request is out; done: its answer is in; sq: an icon
  unsigned *px;
  char q[3200];
} tw;
// thumbs_gen is bumped when the list changes, so a late preview for the old list is dropped
static void *thumb_main(void *u) {
  (void)u;
  resp_t r;
  unsigned *o = 0;
  if (!http_now("GET", tw.q, 0, 0, 0, 4 << 20, &r) && r.status == 200) {
    int w, h, c;
    unsigned char *px = stbi_load_from_memory((unsigned char *)r.body, (int)r.len, &w, &h, &c, 4);
    int ow = tw.sq ? IC : TW, oh = tw.sq ? IC : TH;
    if (px && (o = malloc((size_t)ow * oh * 4))) { // scale to cover the tile (or the icon square), centred
      float sc = fmaxf((float)ow / w, (float)oh / h), ox = (w - ow / sc) / 2, oy = (h - oh / sc) / 2;
      for (int y = 0; y < oh; y++) {
        int sy = (int)(oy + (y + .5f) / sc);
        sy = sy < 0 ? 0 : sy >= h ? h - 1 : sy;
        for (int x = 0; x < ow; x++) {
          int sx = (int)(ox + (x + .5f) / sc);
          sx = sx < 0 ? 0 : sx >= w ? w - 1 : sx;
          const unsigned char *s = px + ((size_t)sy * w + sx) * 4;
          o[(size_t)y * ow + x] = RGB(s[0], s[1], s[2]);
        }
      }
    }
    if (px) stbi_image_free(px);
  }
  resp_free(&r);
  tw.px = o;
  __atomic_store_n(&tw.done, 1, __ATOMIC_RELEASE);
  return 0;
}
// game / app icons are kept once loaded, so a folder opened again shows them straight away
#define ICACHE_N 96 // about 5.5 MB
static struct {
  char *key; // path, then its time and size
  unsigned *px;
} icache[ICACHE_N];
static int icache_next;
static void icache_key(int i, char *k, size_t n) {
  char p[1300];
  join(p, sizeof p, cwd, items[i].name);
  snprintf(k, n, "%s|%lld|%lld", p, items[i].mtime, items[i].size);
}
static int icache_get(int i) {
  char k[1400];
  icache_key(i, k, sizeof k);
  for (int j = 0; j < ICACHE_N; j++)
    if (icache[j].key && !strcmp(icache[j].key, k)) {
      unsigned *px = malloc((size_t)IC * IC * 4);
      if (!px) return -1;
      memcpy(px, icache[j].px, (size_t)IC * IC * 4);
      thumbs[i].px = px, thumbs[i].state = 1, thumbs[i].sq = 1;
      return 0;
    }
  return -1;
}
static void icache_put(int i, const unsigned *px) {
  char k[1400];
  icache_key(i, k, sizeof k);
  unsigned *c = malloc((size_t)IC * IC * 4);
  char *kc = strdup(k);
  if (!c || !kc) return free(c), free(kc);
  memcpy(c, px, (size_t)IC * IC * 4);
  int j = icache_next++ % ICACHE_N;
  free(icache[j].key), free(icache[j].px);
  icache[j].key = kc, icache[j].px = c;
}
static void thumb_take(void) { // the worker's answer, if it's in
  if (!tw.busy || !__atomic_load_n(&tw.done, __ATOMIC_ACQUIRE)) return;
  if (tw.gen == thumbs_gen && thumbs && tw.idx < nthumbs) {
    thumbs[tw.idx].px = tw.px, thumbs[tw.idx].state = tw.px ? 1 : 2, thumbs[tw.idx].sq = tw.sq;
    if (tw.px && tw.sq && tw.idx < nitems) icache_put(tw.idx, tw.px);
  } else free(tw.px);
  tw.px = 0, tw.busy = tw.done = 0;
}
static void thumb_load(int i) {
  char p[1300], e[3000];
  join(p, sizeof p, cwd, items[i].name);
  urlenc(p, e, sizeof e);
  tw.sq = items[i].ic; // a game / app folder or a package: its icon (the payload finds it)
  snprintf(tw.q, sizeof tw.q, "/api/image?w=%d&h=%d&path=%s", tw.sq ? 256 : 320, tw.sq ? 256 : 180, e);
  tw.gen = thumbs_gen, tw.idx = i, tw.done = 0, tw.px = 0;
  pthread_t t;
  pthread_attr_t a;
  pthread_attr_init(&a);
  pthread_attr_setstacksize(&a, 256 << 10);
  pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
  if (!pthread_create(&t, &a, thumb_main, 0)) tw.busy = 1, thumbs[i].state = 3; // 3: on its way
  else thumbs[i].state = 2;
  pthread_attr_destroy(&a);
}
static int list_first(void);
static void thumbs_tick(void) { // the next preview / icon in view, once the buttons have been still for a moment
  thumb_take();
  static int was_grid = -1; // list to grid: pictures skipped in the list get their previews now
  if (was_grid != set.grid && thumbs) {
    for (int i = 0; i < nthumbs; i++)
      if (thumbs[i].state == 2) thumbs[i].state = 0;
    was_grid = set.grid;
  }
  if (scr != SCR_BROWSE || !thumbs || nthumbs != nitems) return;
  int f = set.grid ? grid_first() * GCOLS : list_first(), l = f + (set.grid ? grid_rows() * GCOLS : vis_rows());
  for (int i = f; i < l && i < nitems; i++) // icons seen before: straight away, no request
    if (!thumbs[i].state && items[i].ic) icache_get(i);
  if (tw.busy || modal != M_NONE || kb_open || now_ms - last_input_ms < 150) return;
  for (int i = f; i < l && i < nitems; i++)
    if (!thumbs[i].state) {
      if (items[i].ic || (set.grid && kind_of(&items[i]) == K_IMG && !ext_in(items[i].name, IMG_WEB))) {
        thumb_load(i);
        return;
      }
      thumbs[i].state = 2;
    }
}
static float gfx = -1, gfy; // the grid's focus ring, sliding to the focused tile
static void draw_grid(void) {
  int first = grid_first(), vis = grid_rows();
  if (nitems) {
    float tx = MX + (sel % GCOLS) * (TW + GGAP), ty = LIST_TOP + (sel / GCOLS - first) * GROW;
    if (gfx < 0 || !shown) gfx = tx, gfy = ty;
    gfx += (tx - gfx) * .38f, gfy += (ty - gfy) * .38f;
    if (fabsf(tx - gfx) < .5f && fabsf(ty - gfy) < .5f) gfx = tx, gfy = ty;
  }
  char cl[600], a[64];
  for (int i = first * GCOLS; i < nitems && i < (first + vis) * GCOLS; i++) {
    item_t *it = &items[i];
    int x = MX + (i % GCOLS) * (TW + GGAP), y = LIST_TOP + (i / GCOLS - first) * GROW, f = i == sel, cut = is_cut(it);
    rrect(x, y, TW, TILE_H, 14, f ? P->card_f : it->mark ? P->card_f : P->card, f ? 1 : .6f, 0);
    thumb_t *t = thumbs && nthumbs == nitems ? &thumbs[i] : 0;
    if (t && t->px && t->sq) { // a game / app icon, whole, on a dark backing
      rrect(x, y, TW, TH, 14, 0x000000, .25f, 0);
      draw_sq(t->px, x + (TW - IC) / 2, y + (TH - IC) / 2, IC, cut);
    } else if (t && t->px)
      for (int yy = 0; yy < TH; yy++) {
        unsigned *d = fb + (size_t)(y + yy) * W + x;
        const unsigned *s = t->px + (size_t)yy * TW;
        int cut2 = yy < 10 ? 10 - (int)sqrtf(100 - (10 - yy) * (10 - yy)) : 0; // round the top corners
        for (int xx = cut2; xx < TW - cut2; xx++) d[xx] = cut ? (s[xx] & 0xfefefe) >> 1 | 0xff000000u : s[xx];
      }
    else draw_icon(x + TW / 2 - 22, y + TH / 2 - 22, kind_of(it), cut);
    if (it->mark || nmarked) tick_box(x + 14, y + 22, it->mark);
    const char *gn = it->tname ? it->tname : it->name; // a game's name, on a tile
    clip_text(cl, sizeof cl, 1, 20, gn, TW - 24 - (it->lock ? 26 : 0));
    text(x + 12, y + TH + 8, 1, 20, cut ? C_SUB : C_TEXT, cl);
    if (it->lock) lock_glyph(x + TW - 30, y + TH + 10, C_SUB);
    char sub[200];
    const char *sz = isdir(it) ? "Folder" : human(it->size, a, sizeof a);
    if (it->tname) snprintf(sub, sizeof sub, "%s \x82 %s", it->tid ? it->tid : (isdir(it) ? it->name : "Package"), sz);
    else snprintf(sub, sizeof sub, "%s", sz);
    clip_text(cl, sizeof cl, 0, 16, sub, TW - 24);
    text(x + 12, y + TH + 34, 0, 16, C_SUB, cl);
  }
  if (nitems) { // drawn last, over the tiles it passes
    rrect(gfx - 6, gfy - 6, TW + 12, TILE_H + 12, 20, P->fglow, .25f, 5);
    rrect(gfx, gfy, TW, TILE_H, 14, P->focus, 1, 2.5f);
  }
  int rows = (nitems + GCOLS - 1) / GCOLS;
  if (rows > vis) { // scroll bar
    float h = (float)vis / rows * (vis * GROW - GGAP), y = LIST_TOP + (float)first / rows * (vis * GROW - GGAP);
    rrect(W - MX + 20, y, 5, fmaxf(h, 30), 2.5f, 0xffffff, .3f, 0);
  }
}

// ---- text editor ----
static void text_free(void) {
  for (int i = 0; i < nlines; i++) free(lines[i]);
  nlines = 0, tsel = ttop = tdirty = 0;
}
static int text_open(const char *path, int force) {
  char e[3000], q[3200];
  for (int i = 0; i < nitems; i++) // known size: no need to ask the server
    if (!strcmp(items[i].name, base_of(path)) && items[i].size > (long long)MAX_TEXT_BYTES)
      return say_err("Unable to open as text: it's over 4 MB (a phone or PC can edit up to 8 MB)"), -1;
  urlenc(path, e, sizeof e);
  snprintf(q, sizeof q, "/api/file?path=%s", e);
  resp_t r;
  int rc = http("GET", q, 0, 0, 0, MAX_TEXT_BYTES, &r);
  if (rc == -2) return say_err("Unable to open as text: it's over 4 MB (a phone or PC can edit up to 8 MB)"), -1;
  if (rc || r.status != 200) {
    resp_free(&r);
    return say_err("Unable to open: couldn't read the file"), -1;
  }
  for (size_t i = 0, run = 0; i < r.len; i++) // the editor shows lines; one this long can't be shown or edited
    if (r.body[i] == '\n') run = 0;
    else if (++run > MAX_LINE) {
      resp_free(&r);
      return say_err("Unable to open as text: it has a line over 4,000 characters"), -1;
    }
  if (!force && memchr(r.body, 0, r.len < 8192 ? r.len : 8192)) {
    resp_free(&r);
    snprintf(conf_title, sizeof conf_title, "Open \"%s\" as text?", base_of(path));
    snprintf(conf_text, sizeof conf_text, "It looks like a binary file. Saving it from the editor will damage it.");
    snprintf(tpath, sizeof tpath, "%s", path);
    conf_kind = C_BINARY, modal = M_CONFIRM;
    return -1;
  }
  text_free();
  tcrlf = strstr(r.body, "\r\n") != 0;
  for (char *l = r.body; l;) {
    char *nl = memchr(l, '\n', r.len - (l - r.body));
    size_t n = nl ? (size_t)(nl - l) : r.len - (l - r.body);
    if (!nl && !n && nlines) break; // the file ends with a newline
    if (tcrlf && n && l[n - 1] == '\r') n--;
    if (nlines == lcap) {
      char **n2 = realloc(lines, (lcap = lcap ? lcap * 2 : 256) * sizeof *lines);
      if (!n2) break;
      lines = n2;
    }
    char *s = malloc(n + 1);
    if (!s) break;
    for (size_t i = 0; i < n; i++) s[i] = l[i] ? l[i] : ' '; // NULs would cut the line short
    s[n] = 0;
    lines[nlines++] = s;
    l = nl ? nl + 1 : 0;
  }
  resp_free(&r);
  snprintf(tpath, sizeof tpath, "%s", path);
  scr = SCR_TEXT, shown = 0;
  return 0;
}
static int text_save(void) {
  size_t n = 0;
  for (int i = 0; i < nlines; i++) n += strlen(lines[i]) + 2;
  char *b = malloc(n + 1), e[3000], q[3200];
  if (!b) return say_err("Out of memory"), -1;
  size_t k = 0;
  for (int i = 0; i < nlines; i++) {
    size_t l = strlen(lines[i]);
    memcpy(b + k, lines[i], l);
    k += l;
    if (tcrlf) b[k++] = '\r';
    b[k++] = '\n';
  }
  urlenc(tpath, e, sizeof e);
  snprintf(q, sizeof q, "/api/save?path=%s", e);
  resp_t r;
  int rc = http("POST", q, b, k, 1, 1 << 16, &r), ok = !rc && r.status == 200 && r.body && strstr(r.body, "\"ok\":true");
  if (!ok) {
    char m[300] = "";
    if (!rc && r.body) jstr(r.body, "error", m, sizeof m);
    say_err(m[0] ? m : "Couldn't save");
  } else {
    tdirty = 0;
    say("Saved");
  }
  resp_free(&r);
  free(b);
  return ok ? 0 : -1;
}
#define TROW 36
static int text_rows(void) { return (H - MY - 100 - (MY + 120)) / TROW; }
static void draw_text(void) {
  char sub[1300];
  snprintf(sub, sizeof sub, "%s%s", tpath, tdirty ? "  \x82  unsaved changes" : "");
  char cl[1300];
  clip_text(cl, sizeof cl, 0, 19, sub, W - 2 * MX - 600);
  header(base_of(tpath), cl);
  int top = MY + 120, rows = text_rows(), cw = W - 2 * MX;
  rrect(MX, top - 12, cw, rows * TROW + 24, 18, 0x16181a, .92f, 0);
  if (tsel < ttop) ttop = tsel;
  if (tsel >= ttop + rows) ttop = tsel - rows + 1;
  char num[16];
  int gw = text_w(0, 20, "00000") + 20;
  for (int i = ttop; i < nlines && i < ttop + rows; i++) {
    int y = top + (i - ttop) * TROW;
    if (i == tsel) {
      rrect(MX + 8, y - 2, cw - 16, TROW, 10, P->card_f, 1, 0);
      rrect(MX + 8, y - 2, cw - 16, TROW, 10, P->focus, 1, 2);
    }
    snprintf(num, sizeof num, "%d", i + 1);
    text(MX + gw - text_w(0, 20, num), y + 5, 0, 20, i == tsel ? C_ACC : C_SUB, num);
    // tabs as spaces for display
    char exp[1200];
    size_t w = 0;
    for (const char *s = lines[i]; *s && w + 5 < sizeof exp; s++) {
      if (*s == '\t') {
        for (int t = 0; t < 4; t++) exp[w++] = ' ';
      } else exp[w++] = *s;
    }
    exp[w] = 0;
    clip_text(cl, sizeof cl, 0, 22, exp, cw - gw - 50);
    text(MX + gw + 22, y + 3, 0, 22, C_TEXT, cl);
  }
  if (!nlines) text(MX + gw + 22, top + 3, 0, 22, C_SUB, "Empty file \x82 press Triangle to add a line");
  int x = MX, fy = H - MY - 30;
  ghint(&x, fy, 'x', "Edit line");
  ghint(&x, fy, 't', "New line");
  ghint(&x, fy, 's', "Delete line");
  ghint(&x, fy, '1', "Page");
  ghint(&x, fy, 'O', "Save");
  ghint_back(&x, fy);
  char c[60];
  snprintf(c, sizeof c, "Line %d / %d", nlines ? tsel + 1 : 0, nlines);
  footer_counter(c);
}

// ---- settings ----
static int ssel;
enum { S_WEB, S_HIDDEN, S_CONFIRM, S_THEME, S_VIEW, S_SOUND, S_ABOUT, NSET };
static void draw_field(int y, int focused, const char *label, const char *val, const char *desc) {
  int cw = W - 2 * MX, h = 98;
  if (focused) {
    rrect(MX - 6, y - 6, cw + 12, h + 12, 20, P->fglow, .22f, 5);
    rrect(MX, y, cw, h, 16, P->card_f, 1, 0);
    rrect(MX, y, cw, h, 16, P->focus, 1, 2.5f);
  } else rrect(MX, y, cw, h, 16, P->card, .7f, 0);
  text(MX + 28, y + 15, 1, 26, C_TEXT, label);
  text(MX + 28, y + 55, 0, 20, C_SUB, desc);
  if (val) { // on / off switch, or a value
    int on = !strcmp(val, "On");
    if (!strcmp(val, "On") || !strcmp(val, "Off")) {
      int sx = MX + cw - 28 - 76, sy = y + h / 2 - 21;
      rrect(sx, sy, 76, 42, 21, on ? C_ACC : P->surf2, 1, 0);
      if (!on) rrect(sx, sy, 76, 42, 21, C_LINE, 1, 1.5f);
      rrect(on ? sx + 40 : sx + 6, sy + 6, 30, 30, 15, on ? P->on_acc : C_MUTED, 1, 0);
    } else if (val[0] == '<') { // a choice: < value >, Left / Right change it
      int vw = text_w(1, 22, val + 1), vx = MX + cw - 28 - 40 - vw;
      text(vx, y + h / 2 - 13, 1, 22, focused ? C_TEXT : C_MUTED, val + 1);
      fill_tri(vx - 30, y + h / 2, vx - 18, y + h / 2 - 9, vx - 18, y + h / 2 + 9, focused ? C_ACC : C_SUB);
      fill_tri(vx + vw + 30, y + h / 2, vx + vw + 18, y + h / 2 - 9, vx + vw + 18, y + h / 2 + 9, focused ? C_ACC : C_SUB);
    } else if (val[0] == '!') { // a button
      int bw = text_w(1, 22, val + 1) + 48, bx = MX + cw - 28 - bw, by = y + h / 2 - 24;
      rrect(bx, by, bw, 48, 24, 0xff8a8a, focused ? .2f : 0, 0);
      rrect(bx, by, bw, 48, 24, 0xff8a8a, 1, 1.8f);
      text_c(bx + bw / 2.f, by + 24, 1, 22, 0xff8a8a, val + 1);
    } else text(MX + cw - 28 - text_w(0, 22, val), y + h / 2 - 12, 0, 22, C_MUTED, val);
  }
}
static void draw_settings(void) {
  header("Settings", "FileDen v" VERSION);
  char d[300];
  int y = MY + 122, g = 110;
  if (set.ip[0]) snprintf(d, sizeof d, "On: phones and PCs can use FileDen at http://%s:%d/, even with this app closed. Off: only this app", set.ip, PORT);
  else snprintf(d, sizeof d, "On: phones and PCs can use FileDen on port %d, even with this app closed. Off: only this app", PORT);
  char th[64];
  snprintf(th, sizeof th, "<%s", PALS[theme].name);
  draw_field(y, ssel == S_WEB, "Web server", set.web ? "On" : "Off", d);
  draw_field(y + g, ssel == S_HIDDEN, "Show hidden files", set.hidden ? "On" : "Off", "Names that start with a dot");
  draw_field(y + 2 * g, ssel == S_CONFIRM, "Ask before deleting", set.confirm ? "On" : "Off", "Deleting can't be undone");
  draw_field(y + 3 * g, ssel == S_THEME, "Theme", th, "Colours for this app and the web UI");
  draw_field(y + 4 * g, ssel == S_VIEW, "Grid view", set.grid ? "On" : "Off", "Folders as a grid with picture previews (L3 switches it too)");
  draw_field(y + 5 * g, ssel == S_SOUND, "Sounds", snd_mode ? "On" : "Off", "Soft sounds when you move, choose and go back");
  draw_field(y + 6 * g, ssel == S_ABOUT, "About and controls", "Show the controls",
             "FileDen v" VERSION " \x82 free software, GPL-3.0 \x82 port 9923 \x82 title ID PPSA99923");
  int x = MX, fy = H - MY - 30;
  ghint(&x, fy, 'x', "Change");
  ghint_back(&x, fy);
}

// ---- overlays ----
static void panel(int x, int y, int w, int h) {
  rrect(x, y, w, h, 24, P->top, .985f, 0);
  rrect(x, y, w, h, 24, C_LINE, 1, 1.5f);
}
// the controls card: at first start (until X is pressed on it), and from Settings > About
static void draw_intro(void) {
  int w = 1280, h = 720, x = (W - w) / 2, y = (H - h) / 2;
  panel(x, y, w, h);
  blit_logo(x + 48, y + 40, 72);
  text(x + 140, y + 44, 1, 36, C_TEXT, "Welcome to FileDen");
  text(x + 142, y + 92, 0, 20, C_SUB, "The buttons, in short");
  static const struct {
    const char *keys, *what;
  } L[] = {{"v", "Move around"}, {"x", "Open"}, {"o", "Up one folder \x82 hold to close"}, {"s", "Menu: copy, move, compress, extract\x80"}, {"t", "Select \x82 hold to select all"}},
    R[] = {{"12", "Favourites and drives"}, {"34", "First / last item"}, {"L", "Grid or list"}, {"R", "Sort by name, size, date, type"}, {"O", "Settings"}};
  for (int c = 0; c < 2; c++)
    for (int i = 0; i < 5; i++) {
      const char *k = c ? R[i].keys : L[i].keys;
      int kx = x + 60 + c * 610, ky = y + 170 + i * 72;
      for (; k[1]; k++) { // all but the last key without a label
        int x2 = kx;
        ghint(&x2, ky, k[0], "");
        kx = x2 - 30 + 8;
      }
      ghint(&kx, ky, k[0], c ? R[i].what : L[i].what);
    }
  char t[200];
  if (set.ip[0] && set.web) snprintf(t, sizeof t, "On a phone or PC: http://%s:%d/", set.ip, PORT);
  else snprintf(t, sizeof t, "Turn on the web server in Settings to use FileDen from a phone or PC");
  rrect(x + 60, y + 548, w - 120, 1.5f, 0, C_LINE, 1, 0);
  text(x + 60, y + 572, 0, 20, C_MUTED, t);
  int bx = x + 60, by = y + h - 70;
  ghint(&bx, by, 'x', "Continue");
  const char *n = set.intro ? "This won't show again \x82 Settings > About and controls brings it back" : "Settings > About and controls";
  text(x + w - 60 - text_w(0, 17, n), by + 6, 0, 17, C_SUB, n);
}
static const char *menu_label(int a, char *o, size_t n) {
  int c = nmarked ? nmarked : 1;
  const char *what = nmarked > 1 ? "items" : "item";
#define MANY(word) (nmarked > 1 ? (snprintf(o, n, word " %d %s", c, what), o) : word)
  switch (a) {
  case A_OPEN: return "Open";
  case A_COPY: return MANY("Copy");
  case A_CUT: return MANY("Cut / Move");
  case A_PASTE:
    if (!nclip) return "Paste here";
    return snprintf(o, n, "%s %d item%s here", clip_move ? "Move" : "Paste", nclip, nclip == 1 ? "" : "s"), o;
  case A_SELECT: return nitems && items[sel].mark ? "Unselect" : "Select";
  case A_VLIST: return "View: List";
  case A_VGRID: return "View: Grid";
  case A_SORT: {
    static const char *const nm[] = {"Name", "Size", "Date", "Type"};
    return snprintf(o, n, "Sort: %s", nm[set.sort & 3]), o;
  }
  case A_REFRESH: return "Refresh";
  case A_UNDO: return clip_text(o, n, 0, 22, undo_what, 420), o;
  case A_COPYTO: return "Copy to\x80";
  case A_MOVETO: return "Move to\x80";
  case A_RENAME: return "Rename";
  case A_DELETE: return MANY("Delete");
  case A_INFO: return "Details";
  case A_PERMS: return "Permissions";
  case A_COMPRESS: return MANY("Compress\x80");
  case A_FAV: {
    char p[1300];
    join(p, sizeof p, cwd, items[sel].name);
    return is_fav(p) ? "Remove from favourites" : "Add to favourites";
  }
  case A_FAVDIR: return is_fav(cwd) ? "Unfavourite this folder" : "Favourite this folder";
  case A_FAV_OPEN: return "Open";
  case A_FAV_SHOW: return "Show in its folder";
  case A_FAV_REMOVE: return "Remove from favourites";
  case A_XHERE: return "Extract here";
  case A_XTO: return "Extract to\x80";
  case A_NEWDIR: return "New folder";
  case A_NEWFILE: return "New text file";
  case A_ASTEXT: return "Open as text";
  case A_SELALL: return "Select all";
  case A_SELNONE: return "Clear selection";
  case A_CLIPX: return "Clear clipboard";
  case A_JOB: return "Show progress";
  }
  if (a >= A_FMT0 && a < A_FMT0 + NFMTS) return snprintf(o, n, "%s", FMTS[a - A_FMT0].label), o;
  return "";
#undef MANY
}
static int menu_locked(void) { // any target is a PS5 system item
  for (int i = 0; i < nitems; i++)
    if ((items[i].mark || (!nmarked && i == sel)) && items[i].lock) return 1;
  return 0;
}
static void menu_first(void);
#define ADD(a) (menu[nmenu++] = (a))
#define SEP() (nmenu && menu[nmenu - 1] != A_SEP ? ADD(A_SEP) : 0)
static void open_menu(void) { // grouped like a desktop context menu; lines between the groups
  nmenu = 0, msel = 0, mtop = 0, menu_mode = 0;
  int have = nitems > 0, one = have && nmarked <= 1, lk = have && menu_locked();
  item_t *it = have ? &items[sel] : 0;
  if (have && nmarked == 1)
    for (int i = 0; i < nitems; i++)
      if (items[i].mark) it = &items[i], sel = i;
  int isfile = one && !isdir(it), arc = isfile && kind_of(it) == K_ARC;
  // open
  if (one && !nmarked && !arc) ADD(A_OPEN);
  if (arc) ADD(A_XHERE), ADD(A_XTO);
  if (isfile && !nmarked && kind_of(it) != K_TXT) ADD(A_ASTEXT);
  if (have && !nmarked) ADD(A_SELECT);
  // move things around
  SEP();
  if (have) ADD(A_COPY);
  if (have && !lk) ADD(A_CUT);
  ADD(A_PASTE); // greyed out with nothing to paste
  if (have) ADD(A_COPYTO);
  if (have && !lk) ADD(A_MOVETO);
  if (one && !lk) ADD(A_RENAME);
  ADD(A_NEWDIR), ADD(A_NEWFILE);
  { // the last rename or move (from here or the web UI) can be put back
    resp_t r;
    undo_what[0] = 0;
    if (!http("GET", "/api/undo", 0, 0, 0, 1 << 14, &r) && r.status == 200 && jnum(r.body, "can")) jstr(r.body, "what", undo_what, sizeof undo_what);
    resp_free(&r);
    if (undo_what[0]) ADD(A_UNDO);
  }
  // view
  SEP();
  ADD(A_VLIST), ADD(A_VGRID), ADD(A_SORT), ADD(A_REFRESH);
  // archives, favourites, selection
  SEP();
  if (have) ADD(A_COMPRESS);
  if (one) ADD(A_FAV);
  if (!nmarked && strcmp(cwd, "/")) ADD(A_FAVDIR);
  if (have && nmarked < nitems) ADD(A_SELALL);
  if (nmarked) ADD(A_SELNONE);
  if (nclip) ADD(A_CLIPX);
  if (job.live) ADD(A_JOB);
  // details
  SEP();
  if (one) ADD(A_INFO);
  if (one && !lk) ADD(A_PERMS);
  // delete, last and on its own
  SEP();
  if (have && !lk) ADD(A_DELETE);
  if (nmenu && menu[nmenu - 1] == A_SEP) nmenu--;
  modal = M_MENU, menu_hl = -1, menu_in = 0;
  menu_first();
}
static int menu_ok(int a) { return a != A_SEP && !(a == A_PASTE && !nclip); } // can the focus land on it?
static const char *menu_hint(int a) { // a shortcut, or a tick, on the right
  if (a == A_SELECT) return "Triangle";
  if (a == A_SELALL || a == A_SELNONE) return "Hold Triangle";
  if (a == A_VLIST) return set.grid ? "L3" : "\x01"; // \x01: a tick, drawn
  if (a == A_VGRID) return set.grid ? "\x01" : "L3";
  if (a == A_SORT) return "R3";
  return 0;
}
static void menu_first(void) {
  msel = 0;
  while (msel < nmenu && !menu_ok(menu[msel])) msel++;
}
static void menu_step(int d) {
  for (int k = 0; k < nmenu; k++) {
    msel = (msel + d + nmenu) % nmenu;
    if (menu_ok(menu[msel])) return;
  }
}
static void open_fmt_menu(void) { // Compress: pick the format, then the name
  nmenu = 0, msel = 0, mtop = 0, menu_mode = 1;
  for (int i = 0; i < NFMTS; i++) menu[nmenu++] = A_FMT0 + i;
  modal = M_MENU, menu_hl = -1, menu_in = 0;
}
static void open_fav_menu(void) {
  if (!nfavs) return;
  nmenu = 0, msel = 0, mtop = 0, menu_mode = 2;
  if (!favs[fsel].gone) menu[nmenu++] = A_FAV_OPEN;
  if (!favs[fsel].gone && strcmp(favs[fsel].path, "/")) menu[nmenu++] = A_FAV_SHOW;
  menu[nmenu++] = A_FAV_REMOVE;
  modal = M_MENU, menu_hl = -1, menu_in = 0;
}
// a compact card next to the focused item, pointing at it
#define MI_H 40
#define MS_H 11
static void menu_anchor(int *ax, int *ay, int *ah) { // the focused thing on screen: left x, top y, height
  if (scr == SCR_FAVS) *ax = MX + 160, *ay = LIST_TOP + (int)(sel_y * ROW_H), *ah = ROW_H - 10;
  else if (scr == SCR_BROWSE && set.grid && nitems) {
    int first = grid_first();
    *ax = MX + (sel % GCOLS) * (TW + GGAP) + 20, *ay = LIST_TOP + (sel / GCOLS - first) * GROW, *ah = TILE_H;
  } else if (scr == SCR_BROWSE && nitems) *ax = MX + 160, *ay = LIST_TOP + (int)(sel_y * ROW_H), *ah = ROW_H - 10;
  else *ax = MX + 160, *ay = MY + 180, *ah = 40;
}
static void draw_menu(void) {
  int w = 500, pad = 10, title = menu_mode ? 46 : 0, total = title;
  for (int i = 0; i < nmenu; i++) total += menu[i] == A_SEP ? MS_H : MI_H;
  int maxh = H - 2 * MY - 120, h = (total > maxh ? maxh : total) + 2 * pad;
  int ax, ay, ah;
  menu_anchor(&ax, &ay, &ah);
  // beside the item, the arrow on the card's left edge pointing at it; the card slides to stay on screen
  int x = ax + 260, y = ay + ah / 2 - h / 3, up = 2, lo = MY + 20, hi = H - MY - 56 - h;
  if (y > hi) y = hi;
  if (y < lo) y = lo;
  if (x + w > W - MX) x = W - MX - w;
  int arrow_y = ay + ah / 2;
  if (arrow_y < y + 24 || arrow_y > y + h - 24) up = -1; // the item is off the card's side: no arrow
  menu_in += (1 - menu_in) * .3f;
  if (menu_in > .995f) menu_in = 1;
  x += (int)((1 - menu_in) * -22); // eases out of the item it points at
  panel(x, y, w, h);
  if (up >= 0) { // the little arrow, pointing left at the item
    float tx = x, ty = arrow_y;
    fill_tri(tx + 1.5f, ty - 15, tx + 1.5f, ty + 15, tx - 14, ty, P->top);
    line_aa(tx, ty - 15, tx - 14, ty, 1.5f, C_LINE);
    line_aa(tx - 14, ty, tx, ty + 15, 1.5f, C_LINE);
  }
  char t[400], cl[400];
  int cy = y + pad;
  if (menu_mode == 1) { // Compress: what, then the formats
    snprintf(t, sizeof t, "Compress %s", nmarked > 1 ? "the selection to" : "to");
    text(x + 26, cy + 8, 1, 20, C_SUB, t);
    cy += title;
  } else if (menu_mode == 2) {
    clip_text(cl, sizeof cl, 1, 22, base_of(favs[fsel].path), w - 52);
    text(x + 26, cy + 8, 1, 20, favs[fsel].gone ? 0xffb37a : C_SUB, cl);
    cy += title;
  }
  // keep the focus in view when the list is taller than the card
  int fy = 0;
  for (int i = 0; i < msel; i++) fy += menu[i] == A_SEP ? MS_H : MI_H;
  int view = h - 2 * pad - title, scroll = 0;
  if (total - title > view) {
    scroll = fy + MI_H / 2 - view / 2;
    if (scroll < 0) scroll = 0;
    if (scroll > total - title - view) scroll = total - title - view;
  }
  int top = cy, bot = y + h - pad, ry = cy - scroll;
  float want = (float)(fy - scroll);
  if (menu_hl < 0) menu_hl = want;
  menu_hl += (want - menu_hl) * .38f;
  if (fabsf(want - menu_hl) < .5f) menu_hl = want;
  { // the highlight slides from row to row
    float hy = top + menu_hl;
    if (hy + MI_H > top && hy < bot) {
      rrect(x + 10, hy + 2, w - 20, MI_H - 4, 12, P->card_f, 1, 0);
      rrect(x + 10, hy + 2, w - 20, MI_H - 4, 12, P->focus, 1, 2);
    }
  }
  for (int i = 0; i < nmenu; i++) {
    int a = menu[i], rh = a == A_SEP ? MS_H : MI_H;
    if (ry + rh <= top || ry >= bot) {
      ry += rh;
      continue;
    }
    if (a == A_SEP) {
      rrect(x + 18, ry + MS_H / 2 - 1, w - 36, 2, 1, C_LINE, 1, 0);
    } else {
      char lb[160];
      const char *l = menu_label(a, lb, sizeof lb), *hint = menu_hint(a);
      int ok = menu_ok(a);
      unsigned col = !ok ? C_SUB : a == A_DELETE ? 0xff8a8a : C_TEXT;
      clip_text(cl, sizeof cl, 1, 21, l, w - 150);
      text(x + 26, ry + 8, 1, 21, col, cl);
      if (hint && hint[0] == 1) { // the current view: a tick
        float tx = x + w - 46, ty = ry + MI_H / 2.f;
        line_aa(tx, ty, tx + 7, ty + 7, 3, C_ACC);
        line_aa(tx + 7, ty + 7, tx + 20, ty - 8, 3, C_ACC);
      } else if (hint) text(x + w - 26 - text_w(0, 17, hint), ry + 11, 0, 17, C_MUTED, hint);
    }
    ry += rh;
  }
  if (total - title > view) { // scroll bar
    float th = (float)view / (total - title) * view, tyb = top + (float)scroll / (total - title) * view;
    rrect(x + w - 8, tyb, 3, th, 1.5f, 0xffffff, .35f, 0);
  }
  int hx = MX, hy = H - MY - 30;
  ghint(&hx, hy, 'x', "Choose");
  ghint(&hx, hy, 'o', menu_mode == 1 ? "Back" : "Close");
}
static void draw_confirm(void) {
  int w = 900, h = 290, x = (W - w) / 2, y = (H - h) / 2;
  panel(x, y, w, h);
  char cl[400];
  clip_text(cl, sizeof cl, 1, 30, conf_title, w - 80);
  text(x + 40, y + 36, 1, 30, C_TEXT, cl);
  clip_text(cl, sizeof cl, 0, 22, conf_text, w - 80);
  text(x + 40, y + 96, 0, 22, C_MUTED, cl);
  int bx = x + 40, by = y + h - 70;
  const char *yes = conf_kind == C_DELETE ? "Delete" : conf_kind == C_DISCARD ? "Save and close" : "Open anyway";
  ghint(&bx, by, 'x', yes);
  if (conf_kind == C_DISCARD) ghint(&bx, by, 's', "Discard changes");
  ghint(&bx, by, 'o', "Cancel");
}
// bytes as a number and a unit chosen by `scale` (so "11.5" "of 18.0 GB" share one unit)
static const char *size_unit(long long scale, double *div) {
  static const char *const u[] = {"B", "KB", "MB", "GB", "TB"};
  int i = 0;
  double d = 1;
  while (scale >= d * 1000 && i < 4) d *= 1024, i++;
  *div = d;
  return u[i];
}
static void left_text(long long sec, char *o, size_t n) {
  if (sec >= 3600) snprintf(o, n, "%lldh %02lldm left", sec / 3600, sec / 60 % 60);
  else if (sec >= 60) snprintf(o, n, "%lld min left", (sec + 30) / 60);
  else if (sec >= 10) snprintf(o, n, "%llds left", sec);
  else snprintf(o, n, "seconds left");
}
// the speed history as a filled area across [x, x+w), one column per pixel
static void speed_area(int x, int y, int w, int h, float alpha) {
  if (job.nspd < 2 || w < 4) return;
  float mx = 1;
  for (int i = 0; i < job.nspd; i++) mx = fmaxf(mx, job.spd[i]);
  for (int c = 0; c < w; c++) {
    float f = (float)c / (w - 1) * (job.nspd - 1);
    int i = (int)f;
    if (i >= job.nspd - 1) i = job.nspd - 2;
    float v = job.spd[i] + (job.spd[i + 1] - job.spd[i]) * (f - i), ty = y + h - 4 - (h - 10) * v / mx;
    rrect(x + c, ty, 1, y + h - ty, 0, C_ACC, alpha, 0);
    rrect(x + c, ty - 1, 1, 2.5f, 0, C_ACC, .95f, 0);
  }
}
static void draw_job(void) { // the job, centred: Cancel or Minimise (it keeps going in the background service)
  int w = 1180, h = 470, x = (W - w) / 2, y = (H - h) / 2;
  panel(x, y, w, h);
  float pc = job.fin ? .99f : job_frac();
  job_shown_pc += (pc - job_shown_pc) * .2f;
  int items = !strcmp(job.op, "delete") || !strcmp(job.op, "chmod"), counting = !strcmp(job.state, "counting");
  char t[600], cl[600], a[64], b[64];
  // the percentage, big, then what it's doing
  snprintf(a, sizeof a, "%d%%", (int)(pc * 100));
  text(x + 40, y + 28, 1, 96, C_ACC, a);
  int tx = x + 40 + text_w(1, 96, "100%") + 40, tw = w - (tx - x) - 40;
  if (counting) snprintf(t, sizeof t, "%s\x80", job_verb(job.op, 0));
  else if (job.fin) snprintf(t, sizeof t, "%s \x82 finishing", job_verb(job.op, 0));
  else if (job.tn > 1) snprintf(t, sizeof t, "%s \x82 %lld of %lld", job_verb(job.op, 0), job.dn < job.tn ? job.dn + 1 : job.tn, job.tn);
  else snprintf(t, sizeof t, "%s", job_verb(job.op, 0));
  clip_text(cl, sizeof cl, 1, 30, t, tw);
  text(tx, y + 44, 1, 30, C_TEXT, cl);
  clip_text(cl, sizeof cl, 0, 21, job.fin ? "Compressing the last part" : job.cur[0] ? job.cur : counting ? "Counting what there is to do" : " ", tw);
  text(tx, y + 92, 0, 21, C_MUTED, cl);
  // the numbers: how much, how fast it's written, how long to go, how long so far
  const char *lab[4];
  char val[4][80];
  int nc = 0;
  if (items) snprintf(val[nc], 80, "%lld of %lld", job.dn, job.tn);
  else {
    double dv;
    const char *u = size_unit(job.tb, &dv);
    snprintf(val[nc], 80, "%.1f of %.1f %s", job.db / dv, job.tb / dv, u);
  }
  lab[nc++] = items ? "Items" : "Done";
  long long sp = job.speed;
  if (job.nspd >= 4) { // the last couple of seconds, steadier than the average from the start
    float sum = 0;
    int k = job.nspd < 8 ? job.nspd : 8;
    for (int i = job.nspd - k; i < job.nspd; i++) sum += job.spd[i];
    sp = (long long)(sum / k);
  }
  if (!items) {
    if (sp > 0) snprintf(val[nc], 80, "%s/s", human(sp, b, sizeof b));
    else snprintf(val[nc], 80, "\x80");
    lab[nc++] = "Write speed";
  }
  if (job.fin) snprintf(val[nc], 80, "Almost done");
  else if (!items && sp > 0 && job.tb > job.db) left_text((job.tb - job.db) / sp, b, sizeof b), snprintf(val[nc], 80, "About %s", b);
  else if (items && job.dn && job.tn > job.dn && job.elapsed > 1500) left_text((job.tn - job.dn) * job.elapsed / job.dn / 1000, b, sizeof b), snprintf(val[nc], 80, "About %s", b);
  else snprintf(val[nc], 80, "\x80");
  size_t tl = strlen(val[nc]);
  if (tl > 5 && !strcmp(val[nc] + tl - 5, " left")) val[nc][tl - 5] = 0; // the label says it
  lab[nc++] = "Time left";
  long long e = job.elapsed / 1000;
  snprintf(val[nc], 80, "%lld:%02lld", e / 60, e % 60);
  lab[nc++] = "Running for";
  int sx = x + 40, sy = y + 168, cw = (w - 80) / 4;
  for (int i = 0; i < nc; i++) {
    text(sx + i * cw, sy, 0, 18, C_SUB, lab[i]);
    clip_text(cl, sizeof cl, 1, 30, val[i], cw - 20);
    text(sx + i * cw, sy + 26, 1, 30, C_TEXT, cl);
  }
  // the speed graph, full width, with the progress along its floor
  int gx = x + 40, gw = w - 80, gy = y + 256, gh = 120;
  rrect(gx, gy, gw, gh, 14, 0x000000, .25f, 0);
  if (!items) speed_area(gx + 2, gy, gw - 4, gh - 12, .22f);
  rrect(gx, gy + gh - 10, gw, 10, 5, 0xffffff, .12f, 0);
  if (job_shown_pc > .002f) rrect(gx, gy + gh - 10, fmaxf(10, gw * job_shown_pc), 10, 5, C_ACC, 1, 0);
  if (job.fin) { // a light sweeping along the bar: still working
    float t2 = fmodf(now_ms / 1100.f, 1.f), bx = gx, bw = gw, mx = bx - 160 + t2 * (bw + 160), s0 = mx < bx ? bx : mx, e2 = mx + 160 > bx + bw ? bx + bw : mx + 160;
    if (e2 - s0 > 4) rrect(s0, gy + gh - 10, e2 - s0, 10, 5, 0xffffff, .45f, 0);
  }
  // buttons, and the queue
  int hx = x + 40, hy = y + h - 64;
  ghint(&hx, hy, 's', "Cancel");
  ghint(&hx, hy, 'o', "Minimise");
  if (job.queued) ghint(&hx, hy, 't', "Cancel all");
  if (job.queued) snprintf(t, sizeof t, "%d more queued after this one", job.queued);
  else snprintf(t, sizeof t, "Minimised, it carries on while you browse");
  text(x + w - 40 - text_w(0, 17, t), hy + 6, 0, 17, C_SUB, t);
}
static void draw_kb(void) { // keyboard over the bottom of the screen
  int kw = 1300, kh = 520, kx = (W - kw) / 2, ky = H - MY - 60 - kh;
  panel(kx, ky, kw, kh);
  char kt[300];
  clip_text(kt, sizeof kt, 1, 24, kb_title, kw - 60);
  text(kx + 30, ky + 22, 1, 24, kb_target == KT_XPW && strstr(kb_title, "Wrong") ? 0xff9a9a : C_TEXT, kt);
  // the text with a cursor bar, scrolled so the cursor stays visible
  int fw = kw - 60, fx = kx + 30, fy = ky + 64;
  rrect(fx, fy, fw, 56, 12, P->ph1, 1, 0);
  rrect(fx, fy, fw, 56, 12, C_ACC, 1, 1.5f);
  char before[4096], vis[4096];
  snprintf(before, sizeof before, "%.*s", kb_cur, kb_buf);
  const char *s = before;
  while (*s && text_w(0, 24, s) > fw - 60) { // drop characters from the left
    next_ch(&s);
  }
  snprintf(vis, sizeof vis, "%s", s);
  int cx = fx + 14 + text_w(0, 24, vis);
  text(fx + 14, fy + 14, 0, 24, C_TEXT, vis);
  char after[4096];
  clip_text(after, sizeof after, 0, 24, kb_buf + kb_cur, fx + fw - 14 - cx - 4);
  text(cx + 3, fy + 14, 0, 24, C_TEXT, after);
  if ((now_ms / 500) % 2 == 0) rrect(cx, fy + 12, 2.5f, 32, 1, C_ACC, 1, 0);
  int cs = 104, gap = 8, gx = kx + (kw - 11 * (cs + gap)) / 2;
  for (int r = 0; r < 4; r++)
    for (int c = 0; c < 11; c++) {
      int x = gx + c * (cs + gap), y = ky + 140 + r * (70 + gap);
      char ch[2] = {KB[kb_set][r][c], 0};
      int f = r == kb_r && c == kb_c;
      rrect(x, y, cs, 70, 12, f ? C_ACC : P->surf2, 1, 0);
      if (ch[0] != ' ') text_c(x + cs / 2.f, y + 35, 1, 28, f ? P->on_acc : C_TEXT, ch);
    }
  int x = kx + 30, y = ky + kh - 52;
  ghint(&x, y, 'x', "Type");
  ghint(&x, y, 's', "Delete");
  ghint(&x, y, 't', kb_set == 0 ? "ABC" : kb_set == 1 ? "Symbols" : "abc");
  ghint(&x, y, '1', "Space");
  ghint(&x, y, 'm', "Move cursor");
  ghint(&x, y, 'O', "Done");
  ghint(&x, y, 'o', "Cancel");
}
static void draw_toast(void) {
  if (!toast[0] || now_ms > toast_until) return;
  char cl[400];
  clip_text(cl, sizeof cl, 1, 22, toast, W - 2 * MX - 80);
  int w = text_w(1, 22, cl) + 56, x = W / 2 - w / 2, y = kb_open || modal != M_NONE ? MY + 8 : H - MY - 150;
  rrect(x, y, w, 56, 28, P->card_f, .97f, 0);
  rrect(x, y, w, 56, 28, toast_err ? 0xff8a8a : C_ACC, 1, 1.8f);
  text(x + 28, y + 14, 1, 22, C_TEXT, cl);
}

// ---- input ----
static void open_item(void) {
  if (!nitems) return;
  item_t *it = &items[sel];
  char p[1300];
  join(p, sizeof p, cwd, it->name);
  int k = kind_of(it);
  if (k == K_DIR) {
    if (!load_dir(p, 0)) shown = 0, sel_y = 0;
  } else if (k == K_IMG) {
    scr = SCR_IMAGE;
    img_load(sel);
  } else if (k == K_TXT) text_open(p, 0);
  else if (k == K_ARC) {
    open_menu(); // Extract here / Extract to are first
    msel = 0;
  } else if (k == K_VID) say("Play video and audio from a phone or PC: http://%s:%d/", set.ip[0] ? set.ip : "<PS5-IP>", PORT);
  else open_menu();
}
static void fav_open(int show);
static void go_up(void) {
  if (!strcmp(cwd, "/")) return;
  char par[1024], me[256];
  snprintf(me, sizeof me, "%s", base_of(cwd));
  parent_of(cwd, par, sizeof par);
  if (!load_dir(par, me)) shown = 0;
}
static void switch_place(int d) { // tabs: Favourites, then the drives
  int n = nplaces + 1, cur = scr == SCR_FAVS ? 0 : cur_place() + 1, nx = (cur + d + n) % n;
  if (!nx) {
    load_favs();
    scr = SCR_FAVS, shown = 0, sel_y = 0;
  } else if (!load_dir(places[nx - 1].path, 0)) scr = SCR_BROWSE, shown = 0, sel_y = 0;
}
static void fav_open(int show) { // a folder opens; a file opens in its folder (show: just focused there)
  if (!nfavs) return;
  fav_t *f = &favs[fsel];
  if (f->gone) {
    say_err("It isn't there any more: Square to remove it");
    return;
  }
  if (f->dir && !show) {
    if (!load_dir(f->path, 0)) scr = SCR_BROWSE, shown = 0, sel_y = 0;
    return;
  }
  char par[1024], me[300];
  parent_of(f->path, par, sizeof par);
  snprintf(me, sizeof me, "%s", base_of(f->path));
  if (load_dir(par, me)) return;
  scr = SCR_BROWSE, shown = 0, sel_y = 0;
  if (!show && nitems && !strcmp(items[sel].name, me)) open_item();
}
static void set_sort(int m) { // R3: the next order, the focus stays on the same item
  char f[24], keep[256] = "";
  snprintf(f, sizeof f, "sort=%d", m);
  if (api_post("/api/settings", f, 0, 0)) return;
  if (nitems) snprintf(keep, sizeof keep, "%s", items[sel].name);
  set.sort = m;
  sort_items();
  for (int i = 0; i < nitems; i++)
    if (!strcmp(items[i].name, keep)) sel = i;
  thumbs_reset();
  static const char *const say_as[] = {"Sorted by name", "Sorted by size, biggest first", "Sorted by date, newest first", "Sorted by type"};
  say("%s", say_as[m]);
}
static void set_toggle(const char *k, int v) {
  char f[64];
  snprintf(f, sizeof f, "%s=%d", k, v);
  if (!api_post("/api/settings", f, 0, 0)) {
    load_settings();
    if (!strcmp(k, "hidden")) refresh();
  }
}
static void menu_choose(int a) {
  modal = M_NONE;
  switch (a) {
  case A_OPEN: open_item(); break;
  case A_COPY: do_clip(0); break;
  case A_CUT: do_clip(1); break;
  case A_PASTE: do_paste(); break;
  case A_COPYTO: open_picker(P_COPYTO); break;
  case A_MOVETO: open_picker(P_MOVETO); break;
  case A_RENAME: open_kb(KT_RENAME, "Rename", items[sel].name); break;
  case A_DELETE: ask_delete(); break;
  case A_INFO: open_info(); break;
  case A_PERMS: open_perm(); break;
  case A_COMPRESS: open_fmt_menu(); break;
  case A_FAV: {
    char p[1300];
    join(p, sizeof p, cwd, items[sel].name);
    fav_set(p, !is_fav(p));
    break;
  }
  case A_FAVDIR: fav_set(cwd, !is_fav(cwd)); break;
  case A_FAV_OPEN: fav_open(0); break;
  case A_FAV_SHOW: fav_open(1); break;
  case A_FAV_REMOVE: fav_set(favs[fsel].path, 0); break;
  case A_XHERE: extract_here(); break;
  case A_XTO: open_picker(P_XTO); break;
  case A_NEWDIR: open_kb(KT_NEWDIR, "New folder name", "New folder"); break;
  case A_NEWFILE: open_kb(KT_NEWFILE, "New file name", "new.txt"); break;
  case A_ASTEXT: {
    char p[1300];
    join(p, sizeof p, cwd, items[sel].name);
    text_open(p, 0);
    break;
  }
  case A_SELECT:
    if (nitems) {
      items[sel].mark = !items[sel].mark;
      nmarked += items[sel].mark ? 1 : -1;
    }
    break;
  case A_SORT: set_sort((set.sort + 1) & 3); break;
  case A_REFRESH: refresh(), load_places(), say("Refreshed"); break;
  case A_UNDO: {
    char out[300];
    if (!api_post("/api/undo", "x=1", out, sizeof out)) {
      if (strstr(out, "\"job\":true")) { // from another drive: a move job brings it back
        job.shown = 1, modal = M_JOB;
        poll_job();
      } else {
        say("Put back");
        refresh();
      }
    }
    break;
  }
  case A_VLIST:
  case A_VGRID:
    if (set.grid != (a == A_VGRID)) set_toggle("grid", a == A_VGRID), shown = 0;
    break;
  case A_SELALL:
    for (int i = 0; i < nitems; i++) items[i].mark = 1;
    nmarked = nitems;
    break;
  case A_SELNONE: clear_marks(); break;
  case A_CLIPX:
    free_list(clip, nclip), clip = 0, nclip = 0;
    say("Clipboard cleared");
    break;
  case A_JOB: modal = M_JOB, job.shown = 1; break;
  default:
    if (a >= A_FMT0 && a < A_FMT0 + NFMTS) ask_archive(a - A_FMT0);
  }
}
static void kb_press(unsigned k) {
  size_t n = strlen(kb_buf);
  if (k == PAD_UP) kb_r = (kb_r + 3) % 4;
  else if (k == PAD_DOWN) kb_r = (kb_r + 1) % 4;
  else if (k == PAD_LEFT) kb_c = (kb_c + 10) % 11;
  else if (k == PAD_RIGHT) kb_c = (kb_c + 1) % 11;
  else if (k == PAD_CROSS || k == PAD_L1) {
    char ch = k == PAD_L1 ? ' ' : KB[kb_set][kb_r][kb_c];
    if ((ch != ' ' || k == PAD_L1) && n + 1 < sizeof kb_buf) {
      memmove(kb_buf + kb_cur + 1, kb_buf + kb_cur, n - kb_cur + 1);
      kb_buf[kb_cur++] = ch;
    }
  } else if (k == PAD_SQUARE && kb_cur > 0) { // delete the character before the cursor (whole UTF-8 sequence)
    int s = kb_cur - 1;
    while (s > 0 && ((unsigned char)kb_buf[s] & 0xc0) == 0x80) s--;
    memmove(kb_buf + s, kb_buf + kb_cur, n - kb_cur + 1);
    kb_cur = s;
  } else if (k == PAD_L2 && kb_cur > 0) {
    do kb_cur--;
    while (kb_cur > 0 && ((unsigned char)kb_buf[kb_cur] & 0xc0) == 0x80);
  } else if (k == PAD_R2 && kb_cur < (int)n) {
    do kb_cur++;
    while (kb_cur < (int)n && ((unsigned char)kb_buf[kb_cur] & 0xc0) == 0x80);
  } else if (k == PAD_TRIANGLE) kb_set = (kb_set + 1) % 3;
  else if (k == PAD_OPTIONS || k == PAD_R1) kb_submit();
  else if (k == PAD_CIRCLE) kb_open = 0;
}
static void text_close(void) {
  text_free();
  scr = SCR_BROWSE, shown = 0;
  refresh();
}
static void press(unsigned k) { // k is a single button; O arrives here only as a short press
  sfx(k & (PAD_UP | PAD_DOWN | PAD_LEFT | PAD_RIGHT | PAD_L1 | PAD_R1 | PAD_L2 | PAD_R2) ? SFX_MOVE
      : k == PAD_CIRCLE                                                              ? SFX_BACK
      : k == PAD_SQUARE && !kb_open && modal == M_NONE                               ? SFX_MENU
                                                                                     : SFX_SELECT);
  if (kb_open) {
    kb_press(k);
    return;
  }
  if (modal == M_MENU) {
    if (k == PAD_UP) menu_step(-1);
    else if (k == PAD_DOWN) menu_step(1);
    else if (k == PAD_LEFT) menu_first();
    else if (k == PAD_RIGHT) { // the last one
      msel = 0;
      menu_step(-1);
    } else if (k == PAD_CROSS && menu_ok(menu[msel])) menu_choose(menu[msel]);
    else if (k == PAD_TRIANGLE && menu_mode == 0 && nitems) modal = M_NONE, menu_choose(A_SELECT);
    else if (k == PAD_CIRCLE && menu_mode == 1) open_menu(), menu_first(); // back to the actions
    else if (k == PAD_CIRCLE || k == PAD_SQUARE) modal = M_NONE;
    return;
  }
  if (modal == M_PERM) {
    perm_press(k);
    return;
  }
  if (modal == M_INTRO) {
    if (k == PAD_CROSS || k == PAD_CIRCLE) {
      modal = M_NONE;
      if (k == PAD_CROSS && set.intro && !api_post("/api/settings", "intro=0", 0, 0)) set.intro = 0; // seen: not at start again
    }
    return;
  }
  if (modal == M_CONFIRM) {
    if (k == PAD_CROSS) {
      modal = M_NONE;
      if (conf_kind == C_DELETE) do_delete();
      else if (conf_kind == C_DISCARD) {
        if (!text_save()) text_close();
      } else text_open(tpath, 1);
    } else if (k == PAD_SQUARE && conf_kind == C_DISCARD) modal = M_NONE, text_close();
    else if (k == PAD_CIRCLE) modal = M_NONE;
    return;
  }
  if (modal == M_INFO) {
    if (k == PAD_CROSS && !info.isdir && !info.sha[0] && info.job_id < 0) hash_start();
    else if (k == PAD_CIRCLE) {
      if (info.job_id >= 0 && job.live && job.id == info.job_id) { // stop a long SHA-256 / count
        resp_t r;
        if (!http("POST", "/api/cancel", "x=1", 3, 0, 4096, &r)) resp_free(&r);
      }
      modal = M_NONE, info.open = 0;
    }
    return;
  }
  if (modal == M_JOB) {
    if (k == PAD_SQUARE || (k == PAD_TRIANGLE && job.queued)) { // this job; with Triangle, the queue too
      resp_t r;
      if (!http("POST", "/api/cancel", k == PAD_TRIANGLE ? "all=1" : "x=1", k == PAD_TRIANGLE ? 5 : 3, 0, 4096, &r)) resp_free(&r);
      if (k == PAD_TRIANGLE) say("Cancelled, and the queue cleared");
    }
    if (k == PAD_CIRCLE || k == PAD_TOUCH) modal = M_NONE, job.shown = 0, job.minimised = 1;
    return;
  }
  if (k == PAD_TOUCH) { // the touchpad brings the minimised job back
    if (job.live && !is_info_op(job.op) && modal == M_NONE) modal = M_JOB, job.shown = 1, job.minimised = 0;
    return;
  }
  if (scr == SCR_BROWSE) {
    int vis = vis_rows();
    if (set.grid && nitems && (k & (PAD_UP | PAD_DOWN | PAD_LEFT | PAD_RIGHT))) { // tiles: rows of GCOLS
      if (k == PAD_LEFT) sel = sel > 0 ? sel - 1 : nitems - 1;
      else if (k == PAD_RIGHT) sel = sel + 1 < nitems ? sel + 1 : 0;
      else if (k == PAD_UP) sel = sel - GCOLS >= 0 ? sel - GCOLS : sel;
      else sel = sel + GCOLS < nitems ? sel + GCOLS : (sel / GCOLS < (nitems - 1) / GCOLS ? nitems - 1 : sel);
    } else if (k == PAD_UP && nitems) sel = (sel + nitems - 1) % nitems;
    else if (k == PAD_DOWN && nitems) sel = (sel + 1) % nitems;
    else if (k == PAD_LEFT && nitems) sel = sel - vis < 0 ? 0 : sel - vis;
    else if (k == PAD_RIGHT && nitems) sel = sel + vis >= nitems ? nitems - 1 : sel + vis;
    else if (k == PAD_L3) {
      set_toggle("grid", !set.grid);
      shown = 0;
    } else if (k == PAD_R3) set_sort((set.sort + 1) & 3);
    else if (k == PAD_CROSS) open_item();
    else if (k == PAD_CIRCLE) go_up();
    else if (k == PAD_TRIANGLE && nitems) {
      items[sel].mark = !items[sel].mark;
      nmarked += items[sel].mark ? 1 : -1; // the focus stays put
    } else if (k == PAD_SQUARE) open_menu();
    else if (k == PAD_L1) switch_place(-1);
    else if (k == PAD_R1) switch_place(1);
    else if (k == PAD_L2 && nitems) sel = 0;
    else if (k == PAD_R2 && nitems) sel = nitems - 1;
    else if (k == PAD_OPTIONS) scr = SCR_SETTINGS, ssel = 0, shown = 0, load_settings();
  } else if (scr == SCR_IMAGE) {
    if (k == PAD_CIRCLE && img_actual) img_actual = 0, img_load(img_idx); // back to the fitted picture
    else if (k == PAD_CIRCLE) {
      scr = SCR_BROWSE, shown = 0;
      free(img_px), img_px = 0, img_ok = 0;
    } else if (k == PAD_CROSS && img_ok) img_zoom();
    else if (k == PAD_TRIANGLE && !img_actual) img_full = !img_full, img_build();
    else if (k == PAD_SQUARE) sel = img_idx, open_info();
    else if (img_actual && (k & (PAD_UP | PAD_DOWN | PAD_LEFT | PAD_RIGHT))) {
      int st = 160;
      if (k == PAD_LEFT) img_px_x -= st;
      if (k == PAD_RIGHT) img_px_x += st;
      if (k == PAD_UP) img_px_y -= st;
      if (k == PAD_DOWN) img_px_y += st;
      int mx = img_w > W ? img_w - W : 0, my = img_h > H ? img_h - H : 0;
      img_px_x = img_px_x < 0 ? 0 : img_px_x > mx ? mx : img_px_x;
      img_px_y = img_px_y < 0 ? 0 : img_px_y > my ? my : img_px_y;
      img_build();
    } else if (k == PAD_LEFT || k == PAD_L1) img_step(-1);
    else if (k == PAD_RIGHT || k == PAD_R1) img_step(1);
  } else if (scr == SCR_TEXT) {
    int rows = text_rows();
    if (k == PAD_UP && tsel > 0) tsel--;
    else if (k == PAD_DOWN && tsel + 1 < nlines) tsel++;
    else if (k == PAD_L1 || k == PAD_LEFT) tsel = tsel - rows < 0 ? 0 : tsel - rows;
    else if ((k == PAD_R1 || k == PAD_RIGHT) && nlines) tsel = tsel + rows >= nlines ? nlines - 1 : tsel + rows;
    else if (k == PAD_L2) tsel = 0;
    else if (k == PAD_R2 && nlines) tsel = nlines - 1;
    else if (k == PAD_CROSS) {
      if (nlines) {
        char t[64];
        snprintf(t, sizeof t, "Line %d", tsel + 1);
        open_kb(KT_LINE, t, lines[tsel]);
      } else open_kb(KT_INSERT, "New line", "");
    } else if (k == PAD_TRIANGLE) open_kb(KT_INSERT, "New line", "");
    else if (k == PAD_SQUARE && nlines) {
      free(lines[tsel]);
      memmove(lines + tsel, lines + tsel + 1, (nlines - tsel - 1) * sizeof *lines);
      nlines--;
      if (tsel >= nlines && tsel > 0) tsel--;
      tdirty = 1;
    } else if (k == PAD_OPTIONS) text_save();
    else if (k == PAD_CIRCLE) {
      if (tdirty) {
        snprintf(conf_title, sizeof conf_title, "Save changes to \"%s\"?", base_of(tpath));
        snprintf(conf_text, sizeof conf_text, "You edited this file and haven't saved it yet.");
        conf_kind = C_DISCARD, modal = M_CONFIRM;
      } else text_close();
    }
  } else if (scr == SCR_PICK) {
    pick_press(k);
  } else if (scr == SCR_FAVS) {
    if (k == PAD_UP && nfavs) fsel = (fsel + nfavs - 1) % nfavs;
    else if (k == PAD_DOWN && nfavs) fsel = (fsel + 1) % nfavs;
    else if (k == PAD_L2 && nfavs) fsel = 0;
    else if (k == PAD_R2 && nfavs) fsel = nfavs - 1;
    else if (k == PAD_CROSS) fav_open(0);
    else if (k == PAD_SQUARE) open_fav_menu();
    else if (k == PAD_CIRCLE) scr = SCR_BROWSE, shown = 0;
    else if (k == PAD_L1) switch_place(-1);
    else if (k == PAD_R1) switch_place(1);
    else if (k == PAD_OPTIONS) scr = SCR_SETTINGS, ssel = 0, shown = 0, load_settings();
  } else if (scr == SCR_SETTINGS) {
    if (k == PAD_UP) ssel = (ssel + NSET - 1) % NSET;
    else if (k == PAD_DOWN) ssel = (ssel + 1) % NSET;
    else if (k == PAD_CIRCLE || k == PAD_OPTIONS) scr = SCR_BROWSE, shown = 0;
    else if (k == PAD_CROSS || k == PAD_LEFT || k == PAD_RIGHT) {
      if (ssel == S_WEB) {
        set_toggle("web", !set.web);
        say(set.web ? "Web server on: open http://%s:%d/ on your phone" : "Web server off: only this app can use FileDen",
            set.ip[0] ? set.ip : "<PS5-IP>", PORT);
      } else if (ssel == S_HIDDEN) set_toggle("hidden", !set.hidden);
      else if (ssel == S_CONFIRM) set_toggle("confirm", !set.confirm);
      else if (ssel == S_VIEW) set_toggle("grid", !set.grid);
      else if (ssel == S_THEME) {
        int t = (theme + (k == PAD_LEFT ? NPALS - 1 : 1)) % NPALS;
        char f[64];
        snprintf(f, sizeof f, "theme=%s", PALS[t].id);
        if (!api_post("/api/settings", f, 0, 0)) set_theme(PALS[t].id);
      } else if (ssel == S_ABOUT) {
        if (k == PAD_CROSS) modal = M_INTRO;
      } else if (ssel == S_SOUND) {
        char f[32];
        snprintf(f, sizeof f, "sound=%d", !snd_mode);
        if (!api_post("/api/settings", f, 0, 0)) snd_mode = !snd_mode;
      }
    }
  }
}

static void frame(void) {
  if (scr == SCR_IMAGE) draw_image();
  else {
    memcpy(fb, bgbuf, (size_t)W * H * 4);
    if (scr == SCR_BROWSE) draw_browse();
    else if (scr == SCR_FAVS) draw_favs();
    else if (scr == SCR_TEXT) draw_text();
    else if (scr == SCR_PICK) draw_pick();
    else draw_settings();
    if (shown < 1) { // quick fade-in after a screen change
      shown += .25f;
      int k = (int)((1 - shown) * 140);
      if (k > 0)
        for (size_t i = (size_t)(MY + 100) * W; i < (size_t)W * H; i++) {
          unsigned p = fb[i], q = bgbuf[i];
          fb[i] = 0xff000000u | (((((p & 0xff00ff) * (256 - k)) + ((q & 0xff00ff) * k)) >> 8) & 0xff00ff) |
                  (((((p & 0xff00) * (256 - k)) + ((q & 0xff00) * k)) >> 8) & 0xff00);
        }
    }
  }
  if (job.live && job.shown && modal == M_NONE && !kb_open) modal = M_JOB;
  if (modal == M_JOB && !job.live) modal = M_NONE;
  if (modal != M_NONE || kb_open) dim_screen(110);
  if (kb_open) draw_kb();
  else if (modal == M_MENU) draw_menu();
  else if (modal == M_CONFIRM) draw_confirm();
  else if (modal == M_INFO) draw_info();
  else if (modal == M_PERM) draw_perm();
  else if (modal == M_JOB) draw_job();
  else if (modal == M_INTRO) draw_intro();
  else if (scr == SCR_BROWSE) footer_browse();
  else if (scr == SCR_FAVS) footer_favs();
  draw_toast();
  if (busy_since && now_ms - busy_since > 300) { // a request is taking a while: say so, the screen isn't stuck
    const char *t = "Working";
    int w = text_w(1, 20, t) + 92, x = (W - w) / 2, y = MY + 6;
    rrect(x, y, w, 48, 24, P->card_f, .97f, 0);
    rrect(x, y, w, 48, 24, C_ACC, .7f, 1.5f);
    text(x + 26, y + 12, 1, 20, C_TEXT, t);
    for (int i = 0; i < 3; i++) { // three dots, pulsing in turn
      float ph = fmodf(now_ms / 380.f - i * .33f, 1.f), k = ph < .5f ? ph * 2 : 2 - ph * 2;
      rrect(x + w - 58 + i * 14, y + 20, 8, 8, 4, C_ACC, .3f + .7f * k, 0);
    }
  }
  if (close_ms > CLOSE_DELAY || close_shown > 0) {
    float want = close_ms > CLOSE_DELAY ? 1 : 0;
    close_shown += (want - close_shown) * .25f;
    if (close_shown < .02f && !want) close_shown = 0;
    if (close_ms > CLOSE_DELAY) close_filled = (close_ms - CLOSE_DELAY) / (float)CLOSE_HOLD;
    draw_close();
  }
  present();
}

int main(void) {
  video_init();
  fb = malloc((size_t)W * H * 4);
  if (!fb) halt("FileDen: out of memory");
  stbtt_InitFont(&font[0], Figtree_Regular_ttf, 0);
  stbtt_InitFont(&font[1], Figtree_SemiBold_ttf, 0);
  int lw, lh, ln;
  logo = (unsigned *)stbi_load_from_memory(logo192_png, logo192_png_len, &lw, &lh, &ln, 4);
  logo_n = lw;
  make_bg();
  sceUserServiceInitialize(0);
  scePadInit();
  start_server();
  load_settings();
  snd_start();
  load_places();
  load_favs();
  if (load_dir("/data", 0) && nplaces) load_dir(places[0].path, 0);
  poll_job();
  jp_start();
  ui_thread = pthread_self(), ui_ready = 1; // from here, requests run beside the drawing
  if (set.intro) modal = M_INTRO;            // first start: the controls
  unsigned prev = ~0u; // ignore buttons still held from the splash
  unsigned long long o_since = 0, t_since = 0, last_set = 0, rep_at = 0;
  int t_all = 0, t_dir = 0;
  unsigned rep_key = 0;
  for (;;) { // never return: returning from main crashes an app process
    now_ms = sceKernelGetProcessTime() / 1000;
    unsigned b = pad_buttons(), down = b & ~prev, up = prev & ~b;
    if (prev == ~0u) down = up = 0;
    down |= pending_down, pending_down = 0; // pressed while a request was on its way
    int sa_here = scr == SCR_BROWSE && modal == M_NONE && !kb_open && nitems;
    if ((down & PAD_TRIANGLE) && sa_here) t_since = now_ms, t_all = nmarked == nitems, t_dir = thumbs_gen;
    static const unsigned keys[] = {PAD_UP, PAD_DOWN, PAD_LEFT, PAD_RIGHT, PAD_CROSS, PAD_SQUARE, PAD_TRIANGLE,
                                    PAD_OPTIONS, PAD_L1, PAD_R1, PAD_L2, PAD_R2, PAD_R3, PAD_L3, PAD_TOUCH};
    for (unsigned i = 0; i < sizeof keys / sizeof *keys; i++)
      if (down & keys[i]) {
        press(keys[i]);
        // held: repeat for moving around (and typing deletes), not for actions
        int rep = keys[i] & (PAD_UP | PAD_DOWN | PAD_LEFT | PAD_RIGHT | PAD_L2 | PAD_R2);
        if (kb_open && keys[i] == PAD_SQUARE) rep = 1;
        if (rep) rep_key = keys[i], rep_at = now_ms + 380;
      }
    if (rep_key && (b & rep_key) && now_ms >= rep_at) press(rep_key), rep_at = now_ms + 70;
    if (rep_key && !(b & rep_key)) rep_key = 0;
    // O: a short press is Back, holding it closes FileDen
    if (down & PAD_CIRCLE) o_since = now_ms;
    close_ms = (b & PAD_CIRCLE) && o_since ? (int)(now_ms - o_since) : 0;
    if ((up & PAD_CIRCLE) && o_since && now_ms - o_since < CLOSE_DELAY) press(PAD_CIRCLE);
    if (up & PAD_CIRCLE) o_since = 0;
    if (close_ms >= CLOSE_DELAY + CLOSE_HOLD) quit_app();
    // Triangle: held past the tap, it fills the chip, then ticks (or clears) everything here
    if (t_since && (!(b & PAD_TRIANGLE) || scr != SCR_BROWSE || modal != M_NONE || kb_open || t_dir != thumbs_gen)) t_since = 0;
    sa_fill = t_since && now_ms - t_since > SA_DELAY ? (float)(now_ms - t_since - SA_DELAY) / SA_HOLD : 0;
    if (sa_fill >= 1) {
      t_since = 0, sa_fill = 0;
      for (int i = 0; i < nitems; i++) items[i].mark = !t_all;
      nmarked = t_all ? 0 : nitems;
      sfx(SFX_SELECT);
      if (t_all) say("Selection cleared");
      else {
        char m[64];
        snprintf(m, sizeof m, "All %d selected", nitems);
        say(m);
      }
    }
    if (down) last_input_ms = now_ms;
    prev = b;
    poll_job_async(); // the poller thread's newest answer
    if (now_ms - last_set > 5000) {
      last_set = now_ms;
      load_settings();
      if (set.fails >= 2 && !port_open()) { // stopped (from the web UI's Settings, say): nothing left to show
        say_err("The background service stopped \x81 closing");
        for (int i = 0; i < 90; i++) frame();
        quit_app();
      }
      if (!set.ok) say_err("The background service isn't answering \x81 reopen FileDen to start it");
    }
    thumbs_tick();
    frame();
  }
}
