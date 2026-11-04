// FileDen - drawing core: five colour palettes, anti-aliased rounded
// rects, Figtree text (UTF-8, Latin-1 letters), PS button glyphs, and the VideoOut /
// scePad front end for the fake-signed app. -DHOST_PREVIEW swaps the console front
// end for a scripted pad and PNG frame dumps, to review the TV layout on a PC.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define STBI_NO_THREAD_LOCALS
#define STBI_ASSERT(x) ((void)0)
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_ONLY_GIF
#define STBI_ONLY_TGA
#define STBI_NO_STDIO
#include "../third_party/stb/stb_image.h"
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_assert(x) ((void)0)
#include "../third_party/stb/stb_truetype.h"
#include "../third_party/figtree/figtree_fonts.h"
#define W 1920
#define H 1080
#define MX 96
#define MY 54
static unsigned *fb;
#define RGB(r, g, b) (0xff000000u | ((unsigned)(b) << 16) | ((unsigned)(g) << 8) | (unsigned)(r))
#define HEX(h) RGB(((h) >> 16) & 255, ((h) >> 8) & 255, (h) & 255)
typedef struct {
  const char *id, *name;
  unsigned g0, g1;             // brand gradient: top-left to bottom-right base
  unsigned glow[3];            // soft lights: top right, bottom left, centre
  float gw[3];                 // their strength
  unsigned bg, top, text, muted, sub, line, surf, surf2;
  unsigned acc, acc2, on_acc;  // accent, stronger accent, text on the accent
  unsigned focus, fglow, card_f, card, badge; // focus ring, its glow, focused / plain card fill, pill
  unsigned key_bg, key_line;   // button hint keycaps
  unsigned ph0, ph1;           // placeholder cover gradient
} palette_t;
static const palette_t PALS[] = {
    {"montane-mint", "Montane Mint", 0x2b2f33, 0x1c2022, {0x6bcc99, 0x808585, 0x4d7361}, {.38f, .30f, .12f},
     0x1d2023, 0x30343a, 0xf5f7f6, 0xc3c8c6, 0x9ba2a0, 0x4a5056, 0x2b2f33, 0x353a3f,
     0xb4f0cc, 0x8fe3b4, 0x14201a, 0xb4f0cc, 0xb4f0cc, 0x3a4146, 0x1d2023, 0x1f2a24, 0x16181a, 0x5a6066,
     0x3d4348, 0x1f2326},
    {"classic", "Classic", 0x13174d, 0x1a0d42, {0x2961fa, 0x7538f2, 0x4d42e6}, {.62f, .50f, .22f},
     0x080e1b, 0x18345a, 0xf4f7fb, 0xb1bfd3, 0x8ea1ba, 0x293d58, 0x101c30, 0x192940,
     0x8bc8ff, 0x68b5ff, 0x06121f, 0xe3f3ff, 0x68b5ff, 0x1c3554, 0x111e32, 0x0a2441, 0x11131f, 0x6c7196,
     0x2c5381, 0x12233e},
    {"slate", "Slate", 0x2a2a2d, 0x161618, {0x9a9aa2, 0x606068, 0x404046}, {.24f, .24f, .10f},
     0x161618, 0x2a2a2d, 0xf5f5f6, 0xc6c6ca, 0x9c9ca2, 0x4a4a50, 0x26262a, 0x323236,
     0xe6e6ea, 0xbdbdc4, 0x141416, 0xf0f0f2, 0xffffff, 0x3a3a3f, 0x1a1a1c, 0x2a2a2e, 0x141416, 0x5c5c62,
     0x3e3e44, 0x1e1e22},
    {"ember", "Ember", 0x2a1f1c, 0x1a1311, {0xe0703a, 0xb04a5a, 0x7a3f2a}, {.40f, .30f, .12f},
     0x1a1311, 0x2e2320, 0xfbf6f3, 0xd6c6bd, 0xa8978d, 0x5a4840, 0x2e2320, 0x3a2d29,
     0xffb37a, 0xf2955a, 0x2a1206, 0xffc999, 0xffb37a, 0x47352e, 0x1f1714, 0x3a2418, 0x1a1311, 0x6e5a50,
     0x5a3a2c, 0x2a1c16},
    {"ocean", "Ocean", 0x0f2a33, 0x0a1a22, {0x2aa6c8, 0x1f6f9a, 0x1b4f5e}, {.40f, .30f, .12f},
     0x0a1a22, 0x12313b, 0xf2fafc, 0xb8d2da, 0x8aa9b3, 0x2f5562, 0x12313b, 0x1a3f4a,
     0x7fdcf0, 0x4cc6e0, 0x04212a, 0xa8ecf8, 0x7fdcf0, 0x1e4a57, 0x0d2229, 0x0d3340, 0x0b1b21, 0x4f7480,
     0x2a5f70, 0x12303a},
};
#define NPALS ((int)(sizeof PALS / sizeof *PALS))
static int theme; // index into PALS: Settings > Theme, shared with the web UI through the payload
#define P (&PALS[theme])
#define C_TEXT (P->text)
#define C_MUTED (P->muted)
#define C_SUB (P->sub)
#define C_LINE (P->line)
#define C_ACC (P->acc)

// coverage of pixel (px,py) inside rounded rect, antialiased
static inline float rr_cov(float px, float py, float x, float y, float w, float h, float r) {
  float cx = px + .5f, cy = py + .5f;
  float qx = cx < x + r ? x + r - cx : cx > x + w - r ? cx - (x + w - r) : 0;
  float qy = cy < y + r ? y + r - cy : cy > y + h - r ? cy - (y + h - r) : 0;
  float d;
  if (qx > 0 && qy > 0)
    d = sqrtf(qx * qx + qy * qy) - r;
  else {
    float dx = fmaxf(x - cx, cx - (x + w)), dy = fmaxf(y - cy, cy - (y + h));
    d = fmaxf(dx, dy);
  }
  float c = .5f - d;
  return c < 0 ? 0 : c > 1 ? 1 : c;
}

static inline void blendi(unsigned *d, unsigned hex, int ia) { // ia 0..256; two channels per multiply
  unsigned c = *d, h = ((hex >> 16) & 255) | (hex & 0xff00) | ((hex & 255) << 16), ic = 256 - (unsigned)ia;
  *d = 0xff000000u | ((((c & 0xff00ff) * ic + (h & 0xff00ff) * (unsigned)ia) >> 8) & 0xff00ff) |
       ((((c & 0xff00) * ic + (h & 0xff00) * (unsigned)ia) >> 8) & 0xff00);
}

// Coverage masks are computed once per shape and kept: before this every
// rounded rect, play icon and pad glyph ran sqrtf per pixel on every frame.
typedef struct {
  int kind, k[6], w, h, ready; // kind 1 rounded rect, 2 play icon, 3 pad glyph
  unsigned char *a;
} mask_t;
#define NMASK 160
static mask_t masks[NMASK];
static int nmask_next;
static mask_t *mask_find(int kind, const int *k, int w, int h) {
  for (int i = 0; i < NMASK; i++)
    if (masks[i].a && masks[i].kind == kind && masks[i].w == w && masks[i].h == h &&
        !memcmp(masks[i].k, k, sizeof masks[i].k))
      return &masks[i];
  mask_t *m = &masks[nmask_next];
  nmask_next = (nmask_next + 1) % NMASK;
  free(m->a);
  m->kind = kind;
  memcpy(m->k, k, sizeof m->k);
  m->w = w;
  m->h = h;
  m->ready = 0;
  m->a = calloc(w > 0 && h > 0 ? (size_t)w * h : 1, 1);
  return m;
}
static void mask_blit(const mask_t *m, int x0, int y0, unsigned hex, float a) {
  int al = (int)(a * 256 + .5f);
  if (al > 256) al = 256;
  for (int j = 0; j < m->h; j++) {
    int Y = y0 + j;
    if (Y < 0 || Y >= H)
      continue;
    const unsigned char *s = m->a + (size_t)j * m->w;
    unsigned *row = fb + (size_t)Y * W;
    for (int i = 0; i < m->w; i++) {
      int X = x0 + i;
      if (!s[i] || X < 0 || X >= W)
        continue;
      if (s[i] == 255 && al == 256)
        row[X] = HEX(hex);
      else
        blendi(row + X, hex, (s[i] * al + 128) >> 8);
    }
  }
}
static void rrect(float x, float y, float w, float h, float r, unsigned hex, float a, float ring) {
  int x0 = (int)floorf(x) - 1, y0 = (int)floorf(y) - 1, x1 = (int)ceilf(x + w) + 1, y1 = (int)ceilf(y + h) + 1;
  float fx = x - (x0 + 1), fy = y - (y0 + 1);
  int k[6] = {(int)(w * 8), (int)(h * 8), (int)(r * 8), (int)(ring * 8), (int)(fx * 8), (int)(fy * 8)};
  mask_t *m = mask_find(1, k, x1 - x0, y1 - y0);
  if (!m->ready) {
    m->ready = 1;
    for (int j = 0; j < m->h; j++)
      for (int i = 0; i < m->w; i++) {
        float lx = fx + 1, ly = fy + 1;
        float c = rr_cov(i, j, lx, ly, w, h, r);
        if (ring > 0 && c > 0)
          c -= rr_cov(i, j, lx + ring, ly + ring, w - 2 * ring, h - 2 * ring, r - ring > 0 ? r - ring : 0);
        m->a[(size_t)j * m->w + i] = c <= 0 ? 0 : (unsigned char)(c * 255 + .5f);
      }
  }
  mask_blit(m, x0, y0, hex, a);
}

// ---- text ----
typedef struct {
  unsigned char *bmp;
  int w, h, xo, yo, adv, ok;
} glyph_t;
static stbtt_fontinfo font[2];
static struct {
  int f, px;
  float sc;
  int asc;
  glyph_t g[256];
} gsz[16];
static int ngsz;

// glyph slots: ASCII, 128 = ellipsis, 129 = em dash, 130 = middle dot, 160..255 Latin-1
static int cp_of(int c) { return c == 128 ? 0x2026 : c == 129 ? 0x2014 : c == 130 ? 0xb7 : c; }
// next character of a UTF-8 string as a glyph slot; anything the font table lacks is '?'
// (bytes 0x80..0x82 on their own are the ellipsis / em dash / middle dot markers)
static int next_ch(const char **ps) {
  const unsigned char *s = (const unsigned char *)*ps;
  unsigned c = *s++;
  if (c < 0x80 || c <= 0x82) {
    *ps = (const char *)s;
    return (int)c;
  }
  int n = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;
  unsigned cp = n == 1 ? c & 0x1f : n == 2 ? c & 0x0f : c & 0x07;
  for (int i = 0; i < n; i++) {
    if ((*s & 0xc0) != 0x80) {
      n = 0;
      break;
    }
    cp = (cp << 6) | (*s++ & 0x3f);
  }
  *ps = (const char *)s;
  if (!n) return '?';
  return cp >= 0xa0 && cp <= 0xff ? (int)cp : cp == 0x2026 ? 128 : (cp == 0x2014 || cp == 0x2013) ? 129 : '?';
}

static int size_slot(int f, int px) {
  for (int i = 0; i < ngsz; i++)
    if (gsz[i].f == f && gsz[i].px == px)
      return i;
  int i = ngsz < 16 ? ngsz++ : 15;
  for (int k = 0; k < 256; k++)
    free(gsz[i].g[k].bmp);
  memset(&gsz[i], 0, sizeof gsz[i]);
  gsz[i].f = f;
  gsz[i].px = px;
  gsz[i].sc = stbtt_ScaleForPixelHeight(&font[f], px);
  int a, d, l;
  stbtt_GetFontVMetrics(&font[f], &a, &d, &l);
  gsz[i].asc = (int)(a * gsz[i].sc + .5f);
  return i;
}

static glyph_t *glyph(int s, int c) {
  if (c < 32 || c > 255 || (c > 130 && c < 160))
    c = '?';
  glyph_t *g = &gsz[s].g[c];
  if (!g->ok) {
    int f = gsz[s].f, adv, lsb;
    stbtt_GetCodepointHMetrics(&font[f], cp_of(c), &adv, &lsb);
    g->adv = (int)(adv * gsz[s].sc + .5f);
    g->bmp = stbtt_GetCodepointBitmap(&font[f], gsz[s].sc, gsz[s].sc, cp_of(c), &g->w, &g->h, &g->xo, &g->yo);
    g->ok = 1;
  }
  return g;
}

static int text_w(int f, int px, const char *s) {
  int sl = size_slot(f, px), w = 0;
  while (*s)
    w += glyph(sl, next_ch(&s))->adv;
  return w;
}

// y is the top of the line box; returns width drawn
static int text(int x, int y, int f, int px, unsigned hex, const char *s) {
  int sl = size_slot(f, px), x0 = x, base = y + gsz[sl].asc;
  while (*s) {
    glyph_t *g = glyph(sl, next_ch(&s));
    for (int j = 0; j < g->h; j++) {
      int Y = base + g->yo + j;
      if (Y < 0 || Y >= H)
        continue;
      for (int i = 0; i < g->w; i++) {
        int X = x + g->xo + i;
        unsigned char a = g->bmp[j * g->w + i];
        if (a && X >= 0 && X < W)
          blendi(fb + Y * W + X, hex, a + (a >> 7));
      }
    }
    x += g->adv;
  }
  return x - x0;
}

// text centred on (cx, cy) by its ink (the drawn pixels), not the line box, so
// short labels like "L1" sit optically in the middle of a key
static void text_c(float cx, float cy, int f, int px, unsigned hex, const char *s) {
  int sl = size_slot(f, px), pen = 0, x0 = 1 << 20, x1 = -(1 << 20), y0 = 1 << 20, y1 = -(1 << 20);
  for (const char *q = s; *q;) {
    glyph_t *g = glyph(sl, next_ch(&q));
    if (g->w && g->h) {
      if (pen + g->xo < x0) x0 = pen + g->xo;
      if (pen + g->xo + g->w > x1) x1 = pen + g->xo + g->w;
      if (gsz[sl].asc + g->yo < y0) y0 = gsz[sl].asc + g->yo;
      if (gsz[sl].asc + g->yo + g->h > y1) y1 = gsz[sl].asc + g->yo + g->h;
    }
    pen += g->adv;
  }
  if (x1 < x0)
    return;
  text((int)floorf(cx - (x0 + x1) / 2.f + .5f), (int)floorf(cy - (y0 + y1) / 2.f + .5f), f, px, hex, s);
}

// text at an extra opacity (0..256)
static int text_a(int x, int y, int f, int px, unsigned hex, int op, const char *s) {
  int sl = size_slot(f, px), x0 = x, base = y + gsz[sl].asc;
  while (*s) {
    glyph_t *g = glyph(sl, next_ch(&s));
    for (int j = 0; j < g->h; j++) {
      int Y = base + g->yo + j;
      if (Y < 0 || Y >= H)
        continue;
      for (int i = 0; i < g->w; i++) {
        int X = x + g->xo + i;
        unsigned char a = g->bmp[j * g->w + i];
        if (a && X >= 0 && X < W)
          blendi(fb + Y * W + X, hex, ((a + (a >> 7)) * op) >> 8);
      }
    }
    x += g->adv;
  }
  return x - x0;
}

// fit s into maxw with a trailing ellipsis
// s cut to fit maxw, with "..." when cut. One pass (widths add up), so a very long line - a binary file
// opened as text, say - costs no more than one screen of text.
static void clip_text(char *o, size_t n, int f, int px, const char *s, int maxw) {
  int sl = size_slot(f, px), ell = glyph(sl, 0x80)->adv, w = 0;
  size_t fit = 0, k = 0; // fit: bytes that still leave room for the "..."
  const char *p = s;
  while (*p && k + 5 < n) {
    const char *q = p;
    int a = glyph(sl, next_ch(&q))->adv;
    if (w + a > maxw) { // doesn't fit: cut at the last place the "..." fits too
      snprintf(o, n, "%.*s\x80", (int)fit, s);
      return;
    }
    w += a;
    k += (size_t)(q - p);
    if (w + ell <= maxw) fit = k;
    p = q;
  }
  if (*p) snprintf(o, n, "%.*s\x80", (int)fit, s); // longer than the buffer
  else snprintf(o, n, "%s", s);
}
static int icon_style = 1;
static float key_alpha = 1; // extra fade for glyph_key (the hold-to-close overlay)
static unsigned chip(float x, float y, float w, float h, float r) { // the key body; returns its glyph colour
  if (icon_style == 2) {
    rrect(x, y, w, h, r, P->acc, .95f * key_alpha, 0);
    return P->on_acc;
  }
  rrect(x, y, w, h, r, P->key_bg, .55f * key_alpha, 0);
  rrect(x, y, w, h, r, P->acc, .95f * key_alpha, 1.6f);
  return P->acc;
}
static unsigned glyph_col(char k) {
  return k == 'x' ? 0x7fa7ff : k == 'o' ? 0xff6b7d : k == 's' ? 0xe98fd8 : k == 't' ? 0x46d6b0 : 0xe4e3f5;
}
static float seg_d(float px, float py, float ax, float ay, float bx, float by) {
  float vx = bx - ax, vy = by - ay, t = ((px - ax) * vx + (py - ay) * vy) / (vx * vx + vy * vy);
  t = t < 0 ? 0 : t > 1 ? 1 : t;
  float ex = px - ax - t * vx, ey = py - ay - t * vy;
  return sqrtf(ex * ex + ey * ey);
}
static void glyph_key(int x, int y, int sz, char k) {
  int key[6] = {sz, k, icon_style ? 1 : 0, 0, 0, 0};
  mask_t *mk = mask_find(3, key, sz, sz);
  if (!mk->ready) {
    mk->ready = 1;
    float c = sz / 2.f, half = sz * .5f, st = sz * (icon_style ? .085f : .11f), in = half * (icon_style ? .46f : .52f);
    for (int j = 0; j < sz; j++)
      for (int i = 0; i < sz; i++) {
        float px = i + .5f, py = j + .5f, dx = px - c, dy = py - c, d = 99;
        if (k == 'x')
          d = fminf(seg_d(px, py, c - in, c - in, c + in, c + in), seg_d(px, py, c - in, c + in, c + in, c - in)) - st / 2;
        else if (k == 'o')
          d = fabsf(sqrtf(dx * dx + dy * dy) - (in + st * .3f)) - st / 2;
        else if (k == 's') { // rounded square outline, side 1.7 x inner, centred on the stroke
          float hs = in * .85f, rr = st * .3f, qx = fabsf(dx) - (hs - rr), qy = fabsf(dy) - (hs - rr);
          float o = sqrtf(fmaxf(qx, 0) * fmaxf(qx, 0) + fmaxf(qy, 0) * fmaxf(qy, 0)) + fminf(fmaxf(qx, qy), 0) - rr;
          d = fabsf(o) - st / 2;
        } else if (k == 'L') { // stick: rim, grip ring, four direction ticks
          float r = sqrtf(dx * dx + dy * dy), t = sz * .075f, tr = half * .9f, tb = tr - t * 1.3f;
          d = fminf(fabsf(r - (half - 1)) - .75f, fabsf(r - half * .6f) - sz * .0275f);
          float ax = fabsf(dx), ay = fabsf(dy), along = ax > ay ? ax : ay, across = ax > ay ? ay : ax;
          if (along > tb && along < tr && across < t * (tr - along) / (tr - tb))
            d = -1;
        } else if (k == 't') { // triangle outline in the box w 2.1 x inner, h 1.85 x inner
          float w = in * 2.1f, h = in * 1.85f, top = c - h * .58f;
          float ax = c, ay = top, bx = c + w / 2, by = top + h, cx2 = c - w / 2;
          d = fminf(fminf(seg_d(px, py, ax, ay, bx, by), seg_d(px, py, bx, by, cx2, by)), seg_d(px, py, cx2, by, ax, ay)) - st / 2;
        }
        float cov = .5f - d;
        mk->a[j * sz + i] = cov <= 0 ? 0 : cov >= 1 ? 255 : (unsigned char)(cov * 255);
      }
  }
  if (icon_style && k != 'L') {
    unsigned gc = chip(x + .5f, y + .5f, sz - 1, sz - 1, (sz - 1) / 2.f);
    mask_blit(mk, x, y, gc, key_alpha);
    return;
  }
  rrect(x, y, sz, sz, sz / 2.f, P->key_bg, .92f * key_alpha, 0); // the disc, like the physical button
  mask_blit(mk, x, y, glyph_col(k), key_alpha);
}
// L1 / R1 bumper (same file): a low wide key with a bright top lip and its label

// ---- front end ----
#ifdef HOST_PREVIEW
// PC preview: no console. FD_SCRIPT drives the pad, frames are written as PNG files:
//   FD_SCRIPT="wait:20;snap:list;down;x;wait:10;snap:open" ./preview
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../third_party/stb/stb_image_write.h"
#include <time.h>
#include <unistd.h>
static unsigned long long host_t0;
static unsigned long long sceKernelGetProcessTime(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  unsigned long long t = ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000;
  if (!host_t0) host_t0 = t;
  return t - host_t0 + 1000000;
}
static int sceKernelUsleep(unsigned u) { return usleep(u); }
static int sceUserServiceInitialize(void *p) { (void)p; return 0; }
static int scePadInit(void) { return 0; }
static void notify(const char *m) { fprintf(stderr, "notify: %s\n", m); }
static void halt(const char *m) {
  fprintf(stderr, "halt: %s\n", m);
  exit(1);
}
static void video_init(void) {}
static char shot_name[128];
static void present(void) {
  if (shot_name[0]) {
    char p[200];
    snprintf(p, sizeof p, "%s.png", shot_name);
    stbi_write_png(p, W, H, 4, fb, W * 4); // fb is RGBA in memory order
    fprintf(stderr, "wrote %s\n", p);
    shot_name[0] = 0;
  }
  usleep(4000);
}
#else
// VideoOut + scePad
#include <unistd.h>
#include <signal.h>
int sceKernelLoadStartModule(const char *, size_t, const void *, unsigned, void *, int *);
int sceKernelDlsym(int, const char *, void **);
size_t sceKernelGetDirectMemorySize(void);
int sceKernelAllocateDirectMemory(long long, long long, size_t, size_t, int, long long *);
int sceKernelMapDirectMemory(void **, size_t, int, int, long long, size_t);
int sceKernelSendNotificationRequest(unsigned, void *, size_t, int);
int sceKernelUsleep(unsigned);
unsigned long long sceKernelGetProcessTime(void);
int sceSystemServiceHideSplashScreen(void);
int sceVideoOutOpen(int, int, int, const void *);
int sceVideoOutSetFlipRate(int, int);
int sceVideoOutSubmitFlip(int, int, unsigned, long long);
int sceVideoOutWaitVblank(int);
void sceVideoOutSetBufferAttribute2(void *, unsigned long long, unsigned, unsigned, unsigned, unsigned long long,
                                    unsigned, unsigned long long);
int sceVideoOutRegisterBuffers2(int, int, int, void *, int, void *, int, void *);
int sceUserServiceInitialize(void *);
int sceUserServiceGetInitialUser(int *);
int scePadInit(void);
int scePadOpen(int, int, int, void *);
int scePadReadState(int, void *);

#define FRAME_BYTES 0x1000000
static int vo = -1, cur;
static unsigned char *vmem;
static unsigned xo[W], yo[H];

static void notify(const char *m) {
  static struct { unsigned char r[45]; char msg[3075]; } n;
  snprintf(n.msg, sizeof n.msg, "%s", m);
  sceKernelSendNotificationRequest(0, &n, sizeof n, 0);
}
static void halt(const char *m) {
  notify(m);
  for (;;)
    sceKernelUsleep(1000000);
}
static void video_init(void) {
  sceSystemServiceHideSplashScreen();
  if ((vo = sceVideoOutOpen(0xff, 0, 0, 0)) < 0)
    halt("FileDen: sceVideoOutOpen failed");
  size_t pool = sceKernelGetDirectMemorySize();
  long long phys = 0;
  if (pool < 2 * FRAME_BYTES || sceKernelAllocateDirectMemory(0, (long long)pool, 2 * FRAME_BYTES, 0x200000, 3, &phys) < 0)
    halt("FileDen: direct memory allocation failed");
  void *m = 0;
  if (sceKernelMapDirectMemory(&m, 2 * FRAME_BYTES, 0x33, 0, phys, 0x200000) < 0)
    halt("FileDen: direct memory mapping failed");
  vmem = m;
  memset(vmem, 0, 2 * FRAME_BYTES);
  for (unsigned y = 0; y < H; y++)
    yo[y] = (((y >> 7) * ((W + 127) >> 7)) << 16) +
            (((y << 4) & 0x70u) ^ ((y << 5) & 0xf00u) ^ ((y << 9) & 0x1000u) ^ ((y << 8) & 0x4000u));
  for (unsigned x = 0; x < W; x++)
    xo[x] = ((x >> 7) << 16) | (((x << 2) & 0xcu) ^ ((x << 5) & 0x380u) ^ ((x << 4) & 0x400u) ^
                                ((x << 6) & 0x800u) ^ ((x << 9) & 0xa000u));
  struct { void *data, *meta, *r0, *r1; } bufs[2] = {{vmem, 0, 0, 0}, {vmem + FRAME_BYTES, 0, 0, 0}};
  unsigned char attr[80] = {0};
  sceVideoOutSetFlipRate(vo, 0);
  sceVideoOutSetBufferAttribute2(attr, 0x8000000022000000ull /* rgba8 srgb */, 0, W, H, 0, 0, 0);
  if (sceVideoOutRegisterBuffers2(vo, 0, 0, bufs, 2, attr, 0, 0) < 0)
    halt("FileDen: VideoOut buffer registration failed");
}
static void present(void) {
  cur ^= 1;
  unsigned char *dst = vmem + (size_t)cur * FRAME_BYTES;
  for (int y = 0; y < H; y++) {
    const unsigned *src = fb + (size_t)y * W;
    unsigned yy = yo[y], yh = yy & ~0xffffu;
    for (int x = 0; x < W; x++)
      *(unsigned *)(dst + yh + (xo[x] & ~0xffffu) + ((yy ^ xo[x]) & 0xffffu)) = src[x];
  }
  // only the tiles the 1920x1080 picture covers (8.3 of the 16 MB buffer)
  for (unsigned char *p = dst, *e = dst + (size_t)((H + 127) / 128) * ((W + 127) / 128) * 0x10000; p < e; p += 64)
    __asm__ volatile("clflush (%0)" : : "r"(p) : "memory");
  __asm__ volatile("mfence" ::: "memory");
  sceVideoOutSubmitFlip(vo, cur, 1, 0);
  sceVideoOutWaitVblank(vo);
}

#endif

#define PAD_UP 0x10
#define PAD_RIGHT 0x20
#define PAD_DOWN 0x40
#define PAD_LEFT 0x80
#define PAD_L3 0x2
#define PAD_R3 0x4
#define PAD_L2 0x100
#define PAD_R2 0x200
#define PAD_L1 0x400
#define PAD_R1 0x800
#define PAD_CIRCLE 0x2000
#define PAD_CROSS 0x4000
#define PAD_TRIANGLE 0x1000
#define PAD_SQUARE 0x8000
#define PAD_OPTIONS 0x8
#define PAD_TOUCH 0x100000 // the touchpad, pressed
static int pad = -1, user = -1;
static unsigned char pad_stick[2] = {128, 128}; // left stick x, y from ScePadData
#ifdef HOST_PREVIEW
static const char *host_script;
static int host_wait;
static unsigned host_held;
static int host_hold_frames;
static unsigned host_key(const char *k, size_t n) {
  static const struct { const char *n; unsigned b; } K[] = {
      {"up", 0x10}, {"right", 0x20}, {"down", 0x40}, {"left", 0x80}, {"x", 0x4000}, {"o", 0x2000}, {"s", 0x8000},
      {"t", 0x1000}, {"l1", 0x400}, {"r1", 0x800}, {"l2", 0x100}, {"r2", 0x200}, {"opt", 0x8}, {"l3", 0x2}, {"r3", 0x4}, {"tp", 0x100000}, {0, 0}};
  for (int i = 0; K[i].n; i++)
    if (strlen(K[i].n) == n && !strncmp(K[i].n, k, n)) return K[i].b;
  return 0;
}
static unsigned pad_buttons(void) {
  if (!host_script) host_script = getenv("FD_SCRIPT") ? getenv("FD_SCRIPT") : "wait:30;snap:preview";
  if (host_hold_frames > 0) {
    host_hold_frames--;
    return host_held;
  }
  if (host_held) { // one frame released between presses
    host_held = 0;
    return 0;
  }
  if (host_wait > 0) {
    host_wait--;
    return 0;
  }
  if (!*host_script) exit(0);
  const char *e = host_script + strcspn(host_script, ";");
  size_t n = e - host_script;
  const char *c = memchr(host_script, ':', n);
  if (c && !strncmp(host_script, "wait", 4)) host_wait = atoi(c + 1);
  else if (c && !strncmp(host_script, "snap", 4)) snprintf(shot_name, sizeof shot_name, "%.*s", (int)(e - c - 1), c + 1);
  else if (c && !strncmp(host_script, "hold", 4)) { // hold:o:90
    const char *c2 = memchr(c + 1, ':', e - c - 1);
    if (c2) host_held = host_key(c + 1, c2 - c - 1), host_hold_frames = atoi(c2 + 1);
  } else host_held = host_key(host_script, n);
  host_script = *e ? e + 1 : e;
  return host_held;
}
#else
static unsigned pad_buttons(void) {
  static unsigned long long retry;
  unsigned long long now = sceKernelGetProcessTime();
  if (pad < 0 && now >= retry) {
    retry = now + 1000000;
    if (user < 0)
      sceUserServiceGetInitialUser(&user);
    if (user >= 0)
      pad = scePadOpen(user, 0, 0, 0);
  }
  if (pad < 0)
    return 0;
  static unsigned long long data[64]; // ScePadData (buttons first), oversized on purpose
  static unsigned last;
  if (scePadReadState(pad, data) < 0)
    return last; // a dropped read must not look like O being let go
  last = (unsigned)data[0];
  unsigned char *raw = (unsigned char *)data; // buttons (u32), then left stick x, y
  pad_stick[0] = raw[4], pad_stick[1] = raw[5];
  return (unsigned)data[0];
}
#endif
