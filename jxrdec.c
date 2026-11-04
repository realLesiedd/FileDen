// SPDX-License-Identifier: GPL-3.0-or-later
// JPEG XR -> JPEG for FileDen, using jxrlib (BSD-2-Clause) and stb_image_write (MIT/PD)
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <JXRGlue.h>

// stb_image_write is built in image.c
#define STBI_WRITE_NO_STDIO
#include "third_party/stb/stb_image_write.h"
#include "jxrdec.h"
#include <stdio.h>

char jxr_last_err[256];

static void fmtguid(char *o, size_t n, const PKPixelFormatGUID *g) {
  snprintf(o, n, "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x", (unsigned)g->Data1, g->Data2, g->Data3, g->Data4[0], g->Data4[1], g->Data4[2], g->Data4[3], g->Data4[4], g->Data4[5], g->Data4[6], g->Data4[7]);
}

static float half2f(unsigned short h) {
  unsigned s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023;
  float v;
  if (e == 0)
    v = ldexpf((float)m, -24);
  else if (e == 31)
    v = m ? 0.f : 65504.f;
  else
    v = ldexpf((float)(m | 1024), (int)e - 25);
  return s ? -v : v;
}

static unsigned char lin2srgb(float c) {
  if (!(c > 0.f))
    return 0;
  if (c >= 1.f)
    return 255;
  c = c <= 0.0031308f ? c * 12.92f : 1.055f * powf(c, 1.f / 2.4f) - 0.055f;
  return (unsigned char)(c * 255.f + 0.5f);
}

enum { K_8, K_U16, K_S16, K_F16, K_S32, K_F32, K_1010102 };

// SMPTE ST 2084 (PQ) code value 0..1 -> linear, 1.0 = 80 nits (scRGB scale)
static float pq_eotf(float e) {
  const float m1 = 0.1593017578125f, m2 = 78.84375f, c1 = 0.8359375f, c2 = 18.8515625f, c3 = 18.6875f;
  if (!(e > 0.f))
    return 0.f;
  float p = powf(e, 1.f / m2), n = p - c1;
  if (n < 0.f) n = 0.f;
  return powf(n / (c2 - c3 * p), 1.f / m1) * (10000.f / 80.f);
}

// one pixel -> linear BT.709/sRGB primaries, scRGB scale
static void px(int kind, int pq, int bgr, const unsigned char *p, float o[3]) {
  float v[3];
  switch (kind) {
  case K_U16: for (int c = 0; c < 3; c++) v[c] = ((const unsigned short *)p)[c] / 65535.f; break;
  case K_S16: for (int c = 0; c < 3; c++) v[c] = ((const short *)p)[c] / 8192.f; break; // s2.13
  case K_F16: for (int c = 0; c < 3; c++) v[c] = half2f(((const unsigned short *)p)[c]); break;
  case K_S32: for (int c = 0; c < 3; c++) v[c] = ((const int *)p)[c] / 16777216.f; break; // s7.24
  case K_F32: for (int c = 0; c < 3; c++) v[c] = ((const float *)p)[c]; break;
  default: { // 32bppRGB101010 (PS5 HDR captures): red in bits 20-29, blue in 0-9
    unsigned u = *(const unsigned *)p;
    for (int c = 0; c < 3; c++) v[c] = ((u >> (20 - 10 * c)) & 1023) / 1023.f;
  }
  }
  if (bgr) { float t = v[0]; v[0] = v[2]; v[2] = t; }
  if (!pq) {
    o[0] = v[0], o[1] = v[1], o[2] = v[2];
    return;
  }
  // PQ BT.2020 -> linear, reference white 203 nits -> 1.0 relative to 80 nits, then 2020 -> 709
  for (int c = 0; c < 3; c++) v[c] = pq_eotf(v[c]) * (80.f / 203.f);
  o[0] = 1.6605f * v[0] - 0.5876f * v[1] - 0.0728f * v[2];
  o[1] = -0.1246f * v[0] + 1.1329f * v[1] - 0.0083f * v[2];
  o[2] = -0.0182f * v[0] - 0.1006f * v[1] + 1.1187f * v[2];
}

typedef struct {
  unsigned char *b;
  size_t n, cap;
} mem_t;

static void wr(void *ctx, void *data, int size) {
  mem_t *m = ctx;
  if (m->n + size > m->cap) {
    size_t c = (m->cap ? m->cap * 2 : 65536);
    while (c < m->n + size)
      c *= 2;
    unsigned char *nb = realloc(m->b, c);
    if (!nb)
      return;
    m->b = nb;
    m->cap = c;
  }
  memcpy(m->b + m->n, data, size);
  m->n += size;
}

int jxr_to_jpeg(const char *path, int maxdim, int quality, unsigned char **out, size_t *outlen) {
  PKImageDecode *dec = 0;
  PKFormatConverter *conv = 0;
  unsigned char *raw = 0, *rgb = 0;
  int ret = -1;
  I32 w = 0, h = 0;

  jxr_last_err[0] = 0;
  if (PKCodecFactory_CreateDecoderFromFile(path, &dec) || !dec) {
    snprintf(jxr_last_err, sizeof jxr_last_err, "open/parse failed");
    return -1;
  }
  dec->WMP.wmiSCP.uAlphaMode = 0;
  PKPixelFormatGUID fmt;
  dec->GetPixelFormat(dec, &fmt);
  dec->GetSize(dec, &w, &h);
  char gs[64];
  fmtguid(gs, sizeof gs, &fmt);
  snprintf(jxr_last_err, sizeof jxr_last_err, "%dx%d fmt %s", (int)w, (int)h, gs);
  if (w <= 0 || h <= 0 || w > 16384 || h > 16384)
    goto done;

  PKPixelInfo pi;
  pi.pGUIDPixFmt = &fmt;
  if (PixelFormatLookup(&pi, LOOKUP_FORWARD)) {
    strcat(jxr_last_err, " unknown pixel format");
    goto done;
  }
  snprintf(jxr_last_err + strlen(jxr_last_err), 64, " bd=%d ch=%d bits=%u", (int)pi.bdBitDepth, (int)pi.cChannel, (unsigned)pi.cbitUnit);

  PKRect rc = {0, 0, w, h};
  ERR e;
  // 8-bit formats: let jxrlib convert to 24bpp RGB. Everything wider (PS5 HDR
  // captures are 48bppRGB uint16 holding 10-bit BT.2020 PQ, Windows/Xbox ones
  // are half/float scRGB) is copied natively and converted here.
  int kind = pi.bdBitDepth == BD_16 ? K_U16 : pi.bdBitDepth == BD_16S ? K_S16 : pi.bdBitDepth == BD_16F ? K_F16
           : pi.bdBitDepth == BD_32S ? K_S32 : pi.bdBitDepth == BD_32F ? K_F32
           : (pi.bdBitDepth == BD_10 && pi.cbitUnit == 32) ? K_1010102 : K_8;
  int fl = kind != K_8 && pi.cChannel >= 3;
  int bgr = !!(pi.grBit & PK_pixfmtBGR);
  int pq = kind == K_U16 || kind == K_1010102; // integer HDR = PQ (PS5)
  size_t bpp, stride;
  int sw8 = 0; // raw 8-bit pixels in BGR order
  if (fl) {
    bpp = pi.cbitUnit / 8;
    stride = ((size_t)w * bpp + 127) & ~(size_t)127;
    raw = malloc(stride * h);
    if (!raw) {
      strcat(jxr_last_err, " out of memory");
      goto done;
    }
    if ((e = dec->Copy(dec, &rc, raw, (U32)stride))) {
      snprintf(jxr_last_err + strlen(jxr_last_err), 48, " copy failed err=%d", (int)e);
      goto done;
    }
  } else {
    // 8-bit: try jxrlib's converter to RGB24, otherwise (no route, e.g. 32bppBGRA
    // SDR captures -> err -106) copy native pixels and swizzle here
    bpp = 3;
    stride = ((size_t)w * 3 + 127) & ~(size_t)127;
    raw = malloc(((size_t)w * 16 + 127) * h);
    if (!raw) {
      strcat(jxr_last_err, " out of memory");
      goto done;
    }
    int direct = pi.cChannel >= 3 && pi.cbitUnit >= 24 && pi.cbitUnit <= 32;
    e = -1;
    if (!direct && !(e = PKCodecFactory_CreateFormatConverter(&conv)) &&
        !(e = conv->Initialize(conv, dec, NULL, GUID_PKPixelFormat24bppRGB)))
      e = conv->Copy(conv, &rc, raw, (U32)stride);
    if (e) {
      if (pi.cChannel < 3 || pi.cbitUnit < 24 || pi.cbitUnit > 32) {
        snprintf(jxr_last_err + strlen(jxr_last_err), 48, " no converter to RGB24 err=%d", (int)e);
        goto done;
      }
      bpp = pi.cbitUnit / 8;
      stride = ((size_t)w * bpp + 127) & ~(size_t)127;
      if ((e = dec->Copy(dec, &rc, raw, (U32)stride))) {
        snprintf(jxr_last_err + strlen(jxr_last_err), 48, " copy failed err=%d", (int)e);
        goto done;
      }
      sw8 = bgr;
    }
  }

  // output size
  int ow = w, oh = h;
  if (maxdim > 0 && (w > maxdim || h > maxdim)) {
    if (w >= h) {
      ow = maxdim;
      oh = (int)((long long)h * maxdim / w);
    } else {
      oh = maxdim;
      ow = (int)((long long)w * maxdim / h);
    }
    if (ow < 1) ow = 1;
    if (oh < 1) oh = 1;
  }

  // HDR: find a white point (max of a sparse sample, clamped) for extended Reinhard
  float W = 1.f, v3[3];
  if (fl) {
    for (int y = 0; y < h; y += 7)
      for (int x = 0; x < w; x += 7) {
        px(kind, pq, bgr, raw + y * stride + x * bpp, v3);
        for (int c = 0; c < 3; c++)
          if (v3[c] > W) W = v3[c];
      }
    if (W > 12.5f) W = 12.5f; // cap white near 1000 nits
  }
  float iW2 = 1.f / (W * W);

  rgb = malloc((size_t)ow * oh * 3);
  if (!rgb) {
    strcat(jxr_last_err, " out of memory");
    goto done;
  }
  for (int oy = 0; oy < oh; oy++) {
    int y0 = (int)((long long)oy * h / oh), y1 = (int)((long long)(oy + 1) * h / oh);
    if (y1 <= y0) y1 = y0 + 1;
    for (int ox = 0; ox < ow; ox++) {
      int x0 = (int)((long long)ox * w / ow), x1 = (int)((long long)(ox + 1) * w / ow);
      if (x1 <= x0) x1 = x0 + 1;
      // box filter, but cap the sample count so big downscales stay fast
      int sy = (y1 - y0 + 3) / 4, sx = (x1 - x0 + 3) / 4;
      float acc[3] = {0, 0, 0};
      int n = 0;
      for (int y = y0; y < y1; y += sy)
        for (int x = x0; x < x1; x += sx) {
          const unsigned char *p = raw + y * stride + x * bpp;
          if (fl)
            px(kind, pq, bgr, p, v3);
          for (int c = 0; c < 3; c++) {
            float v;
            if (!fl)
              v = p[sw8 ? 2 - c : c];
            else {
              v = v3[c];
              if (v < 0) v = 0;
              v = v * (1.f + v * iW2) / (1.f + v); // extended Reinhard
            }
            acc[c] += v;
          }
          n++;
        }
      unsigned char *o = rgb + ((size_t)oy * ow + ox) * 3;
      for (int c = 0; c < 3; c++) {
        float v = acc[c] / n;
        o[c] = fl ? lin2srgb(v) : (unsigned char)(v + 0.5f);
      }
    }
  }

  mem_t m = {0};
  if (!stbi_write_jpg_to_func(wr, &m, ow, oh, 3, rgb, quality) || !m.b) {
    strcat(jxr_last_err, " jpeg encode failed");
    free(m.b);
    goto done;
  }
  *out = m.b;
  *outlen = m.n;
  ret = 0;

done:
  free(raw);
  free(rgb);
  if (conv)
    conv->Release(&conv);
  else if (dec)
    dec->Release(&dec);
  return ret;
}
