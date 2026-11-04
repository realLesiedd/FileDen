// FileDen - pictures for the TV app and the web UI: any image FileDen can read, as a JPEG that
// fits a given size. JPEG, PNG, GIF (first frame), BMP, TGA, PSD, HDR, PNM through stb_image;
// WebP through libwebp; JPEG XR (PS5 HDR screenshots) through jxrlib, tone-mapped to SDR.
// Small results (thumbnails) are cached in /data/fileden/thumbs.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "image.h"
#include "title.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_NO_FAILURE_STRINGS
#include "third_party/stb/stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "third_party/stb/stb_image_write.h"
#ifdef FD_WEBP
#include <webp/decode.h>
#endif
#ifdef FD_JXR
#include "jxrdec.h"
#endif

#define MAX_FILE (96u << 20)        // bigger files aren't decoded
#define MAX_PIXELS (100u << 20)     // 100 megapixels
#define CACHE_MAX_SIDE 480          // results this small are cached
#ifndef IMG_CACHE
#define IMG_CACHE "/data/fileden/thumbs"
#endif

static int ext_is(const char *p, const char *e) {
  const char *d = strrchr(p, '.');
  return d && !strcasecmp(d + 1, e);
}
int img_supported(const char *p) {
  static const char *const e[] = {"jpg", "jpeg", "jpe", "jfif", "png", "gif", "bmp", "tga", "psd", "hdr", "pnm", "ppm", "pgm",
#ifdef FD_WEBP
                                  "webp",
#endif
#ifdef FD_JXR
                                  "jxr", "wdp", "hdp",
#endif
                                  0};
  for (int i = 0; e[i]; i++)
    if (ext_is(p, e[i])) return 1;
  return 0;
}

typedef struct {
  unsigned char *b;
  size_t n, cap;
  int bad;
} mbuf_t;
static void mwrite(void *ctx, void *data, int size) {
  mbuf_t *m = ctx;
  if (m->bad || size <= 0) return;
  if (m->n + (size_t)size > m->cap) {
    size_t c = m->cap ? m->cap * 2 : 1 << 16;
    while (c < m->n + (size_t)size) c *= 2;
    unsigned char *q = realloc(m->b, c);
    if (!q) {
      m->bad = 1;
      return;
    }
    m->b = q, m->cap = c;
  }
  memcpy(m->b + m->n, data, (size_t)size);
  m->n += (size_t)size;
}

static unsigned char *read_file(const char *p, size_t *len) {
  int fd = open(p, O_RDONLY);
  if (fd < 0) return 0;
  struct stat st = {0};
  unsigned char *b = 0;
  if (!fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_size > 0 && (unsigned long long)st.st_size <= MAX_FILE &&
      (b = malloc((size_t)st.st_size))) {
    size_t got = 0;
    while (got < (size_t)st.st_size) {
      ssize_t k = read(fd, b + got, (size_t)st.st_size - got);
      if (k <= 0) break;
      got += (size_t)k;
    }
    if (got != (size_t)st.st_size) free(b), b = 0;
    else *len = got;
  } else if (!b) errno = st.st_size > (off_t)MAX_FILE ? EFBIG : EINVAL;
  close(fd);
  return b;
}

// RGBA w x h -> RGB that fits mw x mh (never larger), averaging boxes when shrinking; alpha over dark grey
static unsigned char *fit_rgb(const unsigned char *src, int w, int h, int mw, int mh, int *ow, int *oh) {
  double sc = 1;
  if (w > mw || h > mh) sc = (double)mw / w < (double)mh / h ? (double)mw / w : (double)mh / h;
  int dw = (int)(w * sc + .5), dh = (int)(h * sc + .5);
  if (dw < 1) dw = 1;
  if (dh < 1) dh = 1;
  unsigned char *o = malloc((size_t)dw * dh * 3);
  if (!o) return 0;
  for (int y = 0; y < dh; y++) {
    int y0 = (int)((long long)y * h / dh), y1 = (int)((long long)(y + 1) * h / dh);
    if (y1 <= y0) y1 = y0 + 1;
    for (int x = 0; x < dw; x++) {
      int x0 = (int)((long long)x * w / dw), x1 = (int)((long long)(x + 1) * w / dw);
      if (x1 <= x0) x1 = x0 + 1;
      int sx = (x1 - x0 + 3) / 4, sy = (y1 - y0 + 3) / 4; // at most 4 x 4 samples per pixel
      unsigned r = 0, g = 0, b = 0, n = 0;
      for (int yy = y0; yy < y1; yy += sy)
        for (int xx = x0; xx < x1; xx += sx) {
          const unsigned char *s = src + ((size_t)yy * w + xx) * 4;
          unsigned a = s[3], bg = 0x1c;
          r += (s[0] * a + bg * (255 - a)) / 255, g += (s[1] * a + bg * (255 - a)) / 255, b += (s[2] * a + bg * (255 - a)) / 255;
          n++;
        }
      unsigned char *d = o + ((size_t)y * dw + x) * 3;
      d[0] = (unsigned char)(r / n), d[1] = (unsigned char)(g / n), d[2] = (unsigned char)(b / n);
    }
  }
  *ow = dw, *oh = dh;
  return o;
}

static void cache_name(const char *p, const struct stat *st, int mw, int mh, char *o, size_t n) {
  uint64_t h = 1469598103934665603ull; // FNV-1a of the path, size, time and box
  char k[1400];
  snprintf(k, sizeof k, "%s|%lld|%lld|%dx%d", p, (long long)st->st_size, (long long)st->st_mtime, mw, mh);
  for (const char *c = k; *c; c++) h = (h ^ (unsigned char)*c) * 1099511628211ull;
  snprintf(o, n, IMG_CACHE "/%016llx.jpg", (unsigned long long)h);
}
static int cache_get(const char *cp, unsigned char **out, size_t *len, int *w, int *h) {
  unsigned char *b = read_file(cp, len);
  if (!b) return -1;
  // original size in a JPEG comment? kept simply as the first 8 bytes of the cache file
  if (*len < 9) return free(b), -1;
  memcpy(w, b, 4), memcpy(h, b + 4, 4);
  memmove(b, b + 8, *len - 8);
  *len -= 8;
  *out = b;
  return 0;
}
static void cache_prune(void) { // a simple cap: past 4000 thumbnails, start again
  DIR *d = opendir(IMG_CACHE);
  if (!d) return;
  int n = 0;
  struct dirent *e;
  while ((e = readdir(d))) n++;
  if (n > 4000) {
    rewinddir(d);
    char p[600];
    while ((e = readdir(d)))
      if (e->d_name[0] != '.') snprintf(p, sizeof p, IMG_CACHE "/%s", e->d_name), unlink(p);
  }
  closedir(d);
}
static void cache_put(const char *cp, const unsigned char *j, size_t len, int w, int h) {
  static unsigned puts;
  if (puts++ % 256 == 0) cache_prune();
  mkdir(IMG_CACHE, 0777);
  char tmp[1100];
  static unsigned seq; // requests run side by side: each writes its own temp file
  snprintf(tmp, sizeof tmp, "%s.%u", cp, __atomic_add_fetch(&seq, 1, __ATOMIC_RELAXED));
  int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0666);
  if (fd < 0) return;
  int ok = write(fd, &w, 4) == 4 && write(fd, &h, 4) == 4 && write(fd, j, len) == (ssize_t)len;
  close(fd);
  if (!ok || rename(tmp, cp)) unlink(tmp);
}

int img_to_jpeg(const char *path, int mw, int mh, unsigned char **out, size_t *len, int *w, int *h) {
  *out = 0, *len = 0, *w = *h = 0;
  if (mw < 16) mw = 16;
  if (mh < 16) mh = 16;
  if (mw > 8192) mw = 8192;
  if (mh > 8192) mh = 8192;
  struct stat st;
  char icon[1200];
  if (!stat(path, &st) && S_ISDIR(st.st_mode)) { // a game / app folder: its icon
    if (title_icon_path(path, icon, sizeof icon)) return -ENOENT;
    path = icon;
  }
  if (stat(path, &st) || !S_ISREG(st.st_mode)) return -ENOENT;
  int pkg = ext_is(path, "pkg"); // a package: the icon0.png inside it
  if (!pkg && !img_supported(path)) return -ENOTSUP;
  int small = mw <= CACHE_MAX_SIDE && mh <= CACHE_MAX_SIDE;
  char cp[1100];
  if (small) {
    cache_name(path, &st, mw, mh, cp, sizeof cp);
    if (!cache_get(cp, out, len, w, h)) return 0;
  }
  int q = small ? 80 : 88;
#ifdef FD_JXR
  if (ext_is(path, "jxr") || ext_is(path, "wdp") || ext_is(path, "hdp")) {
    static pthread_mutex_t jxr_mu = PTHREAD_MUTEX_INITIALIZER; // jxr_last_err is shared: one decode at a time
    int md = mw > mh ? mw : mh;
    pthread_mutex_lock(&jxr_mu);
    int bad = jxr_to_jpeg(path, md, q, out, len);
    if (!bad) sscanf(jxr_last_err, "%dx%d", w, h);
    pthread_mutex_unlock(&jxr_mu);
    if (bad) return -EINVAL;
    if (small) cache_put(cp, *out, *len, *w, *h);
    return 0;
  }
#endif
  size_t n = 0;
  unsigned char *file = pkg ? title_pkg_icon(path, &n) : read_file(path, &n), *px = 0;
  if (!file) return pkg ? -ENOTSUP : errno == EFBIG ? -EFBIG : -EIO;
  int iw = 0, ih = 0, c = 0, webp = 0;
#ifdef FD_WEBP
  if (n >= 12 && !memcmp(file, "RIFF", 4) && !memcmp(file + 8, "WEBP", 4)) {
    webp = 1;
    if (WebPGetInfo(file, n, &iw, &ih) && (unsigned long long)iw * ih <= MAX_PIXELS) px = WebPDecodeRGBA(file, n, &iw, &ih);
  }
#endif
  if (!webp && stbi_info_from_memory(file, (int)n, &iw, &ih, &c) && (unsigned long long)iw * ih <= MAX_PIXELS)
    px = stbi_load_from_memory(file, (int)n, &iw, &ih, &c, 4);
  free(file);
  if (!px) return iw && ih && (unsigned long long)iw * ih > MAX_PIXELS ? -EFBIG : -EINVAL;
  int ow, oh;
  unsigned char *rgb = fit_rgb(px, iw, ih, mw, mh, &ow, &oh);
#ifdef FD_WEBP
  if (webp) WebPFree(px);
  else
#endif
    stbi_image_free(px);
  if (!rgb) return -ENOMEM;
  mbuf_t m = {0};
  int ok = stbi_write_jpg_to_func(mwrite, &m, ow, oh, 3, rgb, q);
  free(rgb);
  if (!ok || m.bad || !m.n) return free(m.b), -ENOMEM;
  *out = m.b, *len = m.n, *w = iw, *h = ih;
  if (small) cache_put(cp, m.b, m.n, iw, ih);
  return 0;
}
