// FileDen: RAR with a password. See rarcrypt.h.
//
// A volume is copied block by block. Headers are read (decrypted first when the archive hides its
// file names), file blocks lose their encryption record, and the file data behind them is decrypted
// with AES-CBC on the way through. What comes out is an ordinary RAR that libarchive reads.
//
// RAR5: PBKDF2-HMAC-SHA256 key, AES-256-CBC, a password check value, checksums turned into a MAC.
// RAR 3.x/4.x: the RAR3 SHA-1 key derivation (2^18 rounds), AES-128-CBC.
#include "rarcrypt.h"
#include "fsops.h"
#include <errno.h>
#include <fcntl.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <zlib.h>

#define RX_OUT (1 << 20)        // data passed on per read
#define RX_HDR_MAX (2u << 20)   // RAR5's own limit on a header
#define RX_KDF_MAX 24           // 2^24 rounds: more is a damaged header, not a real archive
static const unsigned char SIG5[8] = {'R', 'a', 'r', '!', 0x1a, 7, 1, 0}, SIG4[7] = {'R', 'a', 'r', '!', 0x1a, 7, 0};

typedef struct {
  int ver; // 5 or 3
  unsigned lg;
  unsigned char salt[16];
  int salted;
  unsigned char key[32], iv[16], hk[32], check[8]; // RAR5: key, MAC key, password check; RAR3: key, iv
} key_t5;

struct rx_shared {
  char pw[256];
  int err, used, verified; // verified: a check value said the password is right
  key_t5 k[8];
  int nk;
  struct mac { char *name; uint32_t crc; unsigned char hk[32], b2[32]; int has_crc, has_b2; } *mac;
  int nmac, cmac;
  struct left { char *name; uint64_t n; } *left; // stored files split across volumes: bytes still to come
  int nleft, cleft;
  struct { // walking a RAR5 compressed stream to where its last block ends (see r5_stream_end)
    char name[1024];
    uint64_t skip; // from the start of the next part's data to the next block header
    unsigned char hb[8];
    int hn;
  } w;
};

struct rx_vol {
  rx_shared_t *s;
  int fd, ver, state, henc; // state 0 signature, 1 headers, 2 file data, 3 the end
  unsigned char hkey[32];   // RAR5: the header key
  int hchecked;             // the header password was checked
  unsigned char *out;       // ready to hand out
  size_t on, oat, ocap;
  uint64_t dleft, oleft;    // file data still to read, and how much of it is passed on (no padding)
  EVP_CIPHER_CTX *c;        // set while the data is encrypted
  unsigned char *ib;
};

static int set_err(rx_shared_t *s, int e) {
  if (!s->err) s->err = e;
  return -1;
}
static uint32_t le32(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static unsigned le16(const unsigned char *p) { return p[0] | p[1] << 8; }
static void put_le32(unsigned char *p, uint32_t v) { p[0] = v, p[1] = v >> 8, p[2] = v >> 16, p[3] = v >> 24; }
static void put_le16(unsigned char *p, unsigned v) { p[0] = v, p[1] = v >> 8; }
static int rd(int fd, void *p, size_t n) { // exactly n bytes
  for (size_t got = 0; got < n;) {
    ssize_t k = read(fd, (char *)p + got, n - got);
    if (k < 0 && errno == EINTR) continue;
    if (k <= 0) return k < 0 ? -errno : -1;
    got += (size_t)k;
  }
  return 0;
}
static int vint(const unsigned char *p, size_t n, size_t *at, uint64_t *v) {
  uint64_t r = 0;
  for (int sh = 0; sh < 70; sh += 7) {
    if (*at >= n) return -1;
    unsigned char b = p[(*at)++];
    r |= (uint64_t)(b & 0x7f) << sh;
    if (!(b & 0x80)) return *v = r, 0;
  }
  return -1;
}

// ---- output buffer ----
typedef struct {
  unsigned char *p;
  size_t n, cap;
  int bad;
} buf_t;
static void bput(buf_t *b, const void *d, size_t n) {
  if (b->bad || !n) return;
  if (b->n + n > b->cap) {
    size_t c = b->cap ? b->cap : 256;
    while (c < b->n + n) c *= 2;
    unsigned char *q = realloc(b->p, c);
    if (!q) {
      b->bad = 1;
      return;
    }
    b->p = q, b->cap = c;
  }
  memcpy(b->p + b->n, d, n);
  b->n += n;
}
static void bvint(buf_t *b, uint64_t v) {
  unsigned char t[10];
  int n = 0;
  do t[n++] = (unsigned char)((v & 0x7f) | (v > 0x7f ? 0x80 : 0)), v >>= 7;
  while (v);
  bput(b, t, n);
}
static void emit(rx_vol_t *v, const void *d, size_t n) { // queue output
  if (v->oat == v->on) v->oat = v->on = 0;
  if (v->on + n > v->ocap) {
    size_t c = v->ocap ? v->ocap : 4096;
    while (c < v->on + n) c *= 2;
    unsigned char *q = realloc(v->out, c);
    if (!q) {
      set_err(v->s, -ENOMEM);
      return;
    }
    v->out = q, v->ocap = c;
  }
  memcpy(v->out + v->on, d, n);
  v->on += n;
}

// ---- AES-CBC ----
static EVP_CIPHER_CTX *cbc(const unsigned char *key, int bits, const unsigned char *iv) {
  EVP_CIPHER_CTX *c = EVP_CIPHER_CTX_new();
  if (c && EVP_DecryptInit_ex(c, bits == 256 ? EVP_aes_256_cbc() : EVP_aes_128_cbc(), 0, key, iv) == 1) {
    EVP_CIPHER_CTX_set_padding(c, 0);
    return c;
  }
  EVP_CIPHER_CTX_free(c);
  return 0;
}
static int dec(EVP_CIPHER_CTX *c, unsigned char *d, size_t n) { // in place; n is whole blocks
  for (size_t off = 0; off < n;) {
    int k = n - off > (1u << 30) ? (1 << 30) : (int)(n - off), w = 0;
    if (EVP_DecryptUpdate(c, d + off, &w, d + off, k) != 1 || w != k) return -1;
    off += (size_t)k;
  }
  return 0;
}

// ---- keys ----
// RAR5: PBKDF2-HMAC-SHA256 over the UTF-8 password. The key is the value after 2^lg rounds; 16 and
// 32 rounds more give the checksum MAC key and the password check (folded to 8 bytes).
static key_t5 *key5(rx_shared_t *s, unsigned lg, const unsigned char salt[16]) {
  for (int i = 0; i < s->nk; i++)
    if (s->k[i].ver == 5 && s->k[i].lg == lg && !memcmp(s->k[i].salt, salt, 16)) return &s->k[i];
  if (lg > RX_KDF_MAX) return set_err(s, -FS_EARCHIVE), (key_t5 *)0;
  key_t5 *k = &s->k[s->nk < 8 ? s->nk++ : 7];
  memset(k, 0, sizeof *k);
  k->ver = 5, k->lg = lg, k->salted = 1;
  memcpy(k->salt, salt, 16);
  unsigned char v2[32];
  int n = (int)strlen(s->pw), c = 1 << lg;
  if (PKCS5_PBKDF2_HMAC(s->pw, n, salt, 16, c, EVP_sha256(), 32, k->key) != 1 ||
      PKCS5_PBKDF2_HMAC(s->pw, n, salt, 16, c + 16, EVP_sha256(), 32, k->hk) != 1 ||
      PKCS5_PBKDF2_HMAC(s->pw, n, salt, 16, c + 32, EVP_sha256(), 32, v2) != 1) {
    k->ver = 0;
    return set_err(s, -ENOMEM), (key_t5 *)0;
  }
  for (int i = 0; i < 32; i++) k->check[i & 7] ^= v2[i];
  return k;
}
// the 12-byte check value: 8 bytes, then the start of their SHA-256 (so damage isn't taken for a wrong password)
static int check5(rx_shared_t *s, const key_t5 *k, const unsigned char cv[12]) {
  unsigned char h[32];
  unsigned hn = 0;
  if (EVP_Digest(cv, 8, h, &hn, EVP_sha256(), 0) != 1 || memcmp(h, cv + 8, 4)) return 0; // can't tell
  if (memcmp(k->check, cv, 8)) return set_err(s, -FS_EBADPW);
  return s->verified = 1;
}
// RAR 3.x: 2^18 rounds of SHA-1 over UTF-16LE password + salt + a 3-byte counter. RAR's SHA-1 wrote
// its message schedule back into the buffer it was hashing, so a long password changes as it goes:
// whole 64-byte blocks of it that SHA-1 read straight from the buffer are replaced (rarfile's
// rar3_corrupt_block does the same).
static void rar3_mangle(unsigned char *p) {
  uint32_t w[16];
  for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4 * i] << 24 | p[4 * i + 1] << 16 | p[4 * i + 2] << 8 | p[4 * i + 3];
  for (int r = 0; r < 4; r++)
    for (int i = 0; i < 16; i++) {
      uint32_t x = w[(i + 13) & 15] ^ w[(i + 8) & 15] ^ w[(i + 2) & 15] ^ w[i];
      w[i] = x << 1 | x >> 31;
    }
  for (int i = 0; i < 16; i++) put_le32(p + 4 * i, w[i]);
}
static size_t utf16le(const char *s, unsigned char *o, size_t max_units) {
  size_t n = 0;
  const unsigned char *p = (const unsigned char *)s;
  while (*p && n < max_units) {
    uint32_t c = *p++;
    int more = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;
    if (more) c &= 0x3f >> more;
    while (more-- && (*p & 0xc0) == 0x80) c = c << 6 | (*p++ & 0x3f);
    if (c >= 0x10000 && n + 2 <= max_units) {
      c -= 0x10000;
      put_le16(o + 2 * n++, 0xd800 | (c >> 10));
      put_le16(o + 2 * n++, 0xdc00 | (c & 0x3ff));
    } else if (c < 0x10000) put_le16(o + 2 * n++, c);
    else break;
  }
  return n * 2;
}
static key_t5 *key3(rx_shared_t *s, const unsigned char *salt) {
  for (int i = 0; i < s->nk; i++)
    if (s->k[i].ver == 3 && s->k[i].salted == (salt != 0) && (!salt || !memcmp(s->k[i].salt, salt, 8))) return &s->k[i];
  key_t5 *k = &s->k[s->nk < 8 ? s->nk++ : 7];
  memset(k, 0, sizeof *k);
  k->ver = 3, k->salted = salt != 0;
  if (salt) memcpy(k->salt, salt, 8);
  unsigned char seed[2 * 127 + 8 + 3];
  size_t sl = utf16le(s->pw, seed, 127);
  if (salt) memcpy(seed + sl, salt, 8), sl += 8;
  EVP_MD_CTX *m = EVP_MD_CTX_new(), *t = EVP_MD_CTX_new();
  int ok = m && t && EVP_DigestInit_ex(m, EVP_sha1(), 0) == 1;
  unsigned long long nbytes = 0;
  unsigned char d[20];
  unsigned dn;
  for (uint32_t i = 0; ok && i < 0x40000; i++) {
    seed[sl] = (unsigned char)i, seed[sl + 1] = (unsigned char)(i >> 8), seed[sl + 2] = (unsigned char)(i >> 16);
    size_t pos = nbytes & 63;
    nbytes += sl + 3;
    ok = EVP_DigestUpdate(m, seed, sl + 3) == 1;
    if (sl > 64)
      for (size_t dp = 64 - pos; dp + 64 <= sl; dp += 64) rar3_mangle(seed + dp);
    if (ok && !(i & 0x3fff)) { // one byte of the IV every 2^14 rounds
      ok = EVP_MD_CTX_copy_ex(t, m) == 1 && EVP_DigestFinal_ex(t, d, &dn) == 1;
      k->iv[i >> 14] = d[19];
    }
  }
  if (ok) ok = EVP_DigestFinal_ex(m, d, &dn) == 1;
  EVP_MD_CTX_free(m);
  EVP_MD_CTX_free(t);
  if (!ok) {
    k->ver = 0;
    return set_err(s, -ENOMEM), (key_t5 *)0;
  }
  for (int i = 0; i < 4; i++)
    for (int j = 0; j < 4; j++) k->key[i * 4 + j] = d[i * 4 + 3 - j];
  return k;
}

// stored files split across volumes: what's left to pass on
static uint64_t *left_of(rx_shared_t *s, const char *name, size_t nl, int make) {
  for (int i = 0; i < s->nleft; i++)
    if (strlen(s->left[i].name) == nl && !memcmp(s->left[i].name, name, nl)) return &s->left[i].n;
  if (!make) return 0;
  if (s->nleft == s->cleft) {
    void *q = realloc(s->left, (s->cleft = s->cleft ? s->cleft * 2 : 8) * sizeof *s->left);
    if (!q) return 0;
    s->left = q;
  }
  char *nm = malloc(nl + 1);
  if (!nm) return 0;
  memcpy(nm, name, nl), nm[nl] = 0;
  s->left[s->nleft].name = nm, s->left[s->nleft].n = 0;
  return &s->left[s->nleft++].n;
}
static uint64_t pass_on(rx_shared_t *s, const char *name, size_t nl, int cont_before, int cont_after, uint64_t size,
                        uint64_t data) {
  uint64_t *l = left_of(s, name, nl, 1), have = cont_before && l ? *l : size, out = data < have ? data : have;
  if (l) *l = cont_after ? have - out : 0;
  return out;
}
static struct mac *mac_of(rx_shared_t *s, const char *name, size_t nl, const unsigned char hk[32]) {
  struct mac *m = 0;
  for (int i = 0; i < s->nmac && !m; i++) // a split file: the last part's values count
    if (strlen(s->mac[i].name) == nl && !memcmp(s->mac[i].name, name, nl)) m = &s->mac[i];
  if (!m) {
    if (s->nmac == s->cmac) {
      void *q = realloc(s->mac, (s->cmac = s->cmac ? s->cmac * 2 : 16) * sizeof *s->mac);
      if (!q) return 0;
      s->mac = q;
    }
    char *nm = malloc(nl + 1);
    if (!nm) return 0;
    memcpy(nm, name, nl), nm[nl] = 0;
    m = &s->mac[s->nmac++];
    memset(m, 0, sizeof *m);
    m->name = nm;
  }
  memcpy(m->hk, hk, 32);
  return m;
}
static void add_mac(rx_shared_t *s, const char *name, size_t nl, uint32_t crc, const unsigned char hk[32]) {
  struct mac *m = mac_of(s, name, nl, hk);
  if (m) m->crc = crc, m->has_crc = 1;
}
static void add_b2(rx_shared_t *s, const char *name, size_t nl, const unsigned char b2[32], const unsigned char hk[32]) {
  struct mac *m = mac_of(s, name, nl, hk);
  if (m) memcpy(m->b2, b2, 32), m->has_b2 = 1;
}
int rx_wants_blake2(const rx_shared_t *s, const char *name) {
  for (int i = 0; s && i < s->nmac; i++)
    if (s->mac[i].has_b2 && !strcmp(s->mac[i].name, name)) return 1;
  return 0;
}
int rx_check_blake2(rx_shared_t *s, const char *name, const unsigned char digest[32]) {
  for (int i = 0; s && i < s->nmac; i++)
    if (s->mac[i].has_b2 && !strcmp(s->mac[i].name, name)) {
      unsigned char d[32];
      unsigned dn = 0;
      if (!HMAC(EVP_sha256(), s->mac[i].hk, 32, digest, 32, d, &dn)) return 0;
      return memcmp(d, s->mac[i].b2, 32) ? -1 : 1;
    }
  return 0;
}
int rx_check_crc(rx_shared_t *s, const char *name, uint32_t crc) {
  for (int i = 0; s && i < s->nmac; i++)
    if (s->mac[i].has_crc && !strcmp(s->mac[i].name, name)) {
      unsigned char raw[4], d[32];
      unsigned dn = 0;
      put_le32(raw, crc);
      if (!HMAC(EVP_sha256(), s->mac[i].hk, 32, raw, 4, d, &dn)) return 0;
      uint32_t m = 0;
      for (int j = 0; j < 32; j++) m ^= (uint32_t)d[j] << ((j & 3) * 8);
      return m == s->mac[i].crc ? 1 : -1;
    }
  return 0;
}

// ---- RAR5 ----
// Encrypted data is padded to whole AES blocks, and libarchive's RAR5 reader takes those padding
// bytes for another compressed block. So the real end is found first: the stream is a chain of blocks
// (flags, checksum, size), and with CBC any 16 bytes can be decrypted from the 16 before them, so
// only the block headers are read and decrypted.
static int plain_at(rx_vol_t *v, off_t base, const unsigned char *key, const unsigned char *iv0, uint64_t off, unsigned char *out,
                    size_t n) {
  uint64_t a = off & ~(uint64_t)15, e = (off + n + 15) & ~(uint64_t)15;
  unsigned char iv[16], b[48];
  if (e - a > sizeof b) return -1;
  if (a == 0) memcpy(iv, iv0, 16);
  else if (pread(v->fd, iv, 16, base + (off_t)a - 16) != 16) return -1;
  if (pread(v->fd, b, (size_t)(e - a), base + (off_t)a) != (ssize_t)(e - a)) return -1;
  EVP_CIPHER_CTX *c = cbc(key, 256, iv);
  int r = !c || dec(c, b, (size_t)(e - a)) ? -1 : 0;
  EVP_CIPHER_CTX_free(c);
  if (!r) memcpy(out, b + (off - a), n);
  return r;
}
static uint64_t r5_stream_end(rx_vol_t *v, const unsigned char *key, const unsigned char *iv, uint64_t ds, const char *name, size_t nl,
                              int cont) {
  rx_shared_t *s = v->s;
  off_t base = lseek(v->fd, 0, SEEK_CUR);
  if (base < 0) return ds;
  if (!cont || nl >= sizeof s->w.name || strlen(s->w.name) != nl || memcmp(s->w.name, name, nl)) { // a new stream
    memset(&s->w, 0, sizeof s->w);
    if (nl < sizeof s->w.name) memcpy(s->w.name, name, nl);
  }
  uint64_t pos = s->w.skip;
  for (int guard = 0; guard < (1 << 24); guard++) {
    if (!s->w.hn) { // the flags byte says how long the header is
      if (pos >= ds) return s->w.skip = pos - ds, ds;
      if (plain_at(v, base, key, iv, pos, s->w.hb, 1)) return ds;
      s->w.hn = 1, pos++;
    }
    int need = 2 + ((s->w.hb[0] >> 3) & 7) + 1;
    if (need > 5) return ds; // not a block header: let libarchive say so
    if (s->w.hn < need) {
      size_t k = (size_t)(need - s->w.hn);
      if (k > ds - pos) k = (size_t)(ds - pos);
      if (k && plain_at(v, base, key, iv, pos, s->w.hb + s->w.hn, k)) return ds;
      s->w.hn += (int)k, pos += k;
      if (s->w.hn < need) return s->w.skip = 0, ds; // the header carries on in the next volume
    }
    uint64_t size = 0;
    unsigned char ck = 0x5a ^ s->w.hb[0];
    for (int i = 2; i < need; i++) size |= (uint64_t)s->w.hb[i] << (8 * (i - 2)), ck ^= s->w.hb[i];
    if (ck != s->w.hb[1]) return s->w.hn = 0, ds; // garbage: a wrong password, or damage
    int last = s->w.hb[0] & 0x40;
    s->w.hn = 0, pos += size;
    if (last) {
      s->w.name[0] = 0;
      return pos < ds ? pos : ds;
    }
  }
  return ds;
}

static int r5_read_header(rx_vol_t *v, unsigned char **out, size_t *len) {
  rx_shared_t *s = v->s;
  unsigned char st[16];
  EVP_CIPHER_CTX *c = 0;
  size_t got;
  if (v->henc) { // a 16-byte IV, then the header in whole AES blocks
    unsigned char iv[16];
    int r = rd(v->fd, iv, 16);
    if (r) return r == -1 ? 1 : set_err(s, r);
    if ((r = rd(v->fd, st, 16))) return set_err(s, r == -1 ? -FS_EARCHIVE : r);
    if (!(c = cbc(v->hkey, 256, iv)) || dec(c, st, 16)) return EVP_CIPHER_CTX_free(c), set_err(s, -ENOMEM);
    got = 16;
  } else {
    int r = rd(v->fd, st, 5);
    if (r) return r == -1 ? 1 : set_err(s, r);
    got = 5;
    while (got < 7 && (st[got - 1] & 0x80)) {
      if ((r = rd(v->fd, st + got, 1))) return set_err(s, r == -1 ? -FS_EARCHIVE : r);
      got++;
    }
  }
  size_t at = 4;
  uint64_t hs;
  if (vint(st, got, &at, &hs) || hs > RX_HDR_MAX || !hs) {
    EVP_CIPHER_CTX_free(c);
    return set_err(s, v->henc && !v->hchecked ? -FS_EBADPW : -FS_EARCHIVE);
  }
  size_t total = at + (size_t)hs, full = v->henc ? (total + 15) & ~(size_t)15 : total;
  unsigned char *h = malloc(full > got ? full : got);
  if (!h) return EVP_CIPHER_CTX_free(c), set_err(s, -ENOMEM);
  memcpy(h, st, got);
  int r = full > got ? rd(v->fd, h + got, full - got) : 0;
  if (!r && c && full > got && dec(c, h + got, full - got)) r = -ENOMEM;
  EVP_CIPHER_CTX_free(c);
  if (r) return free(h), set_err(s, r == -1 ? -FS_EARCHIVE : r);
  if ((uint32_t)crc32(0, h + 4, (uInt)(total - 4)) != le32(h)) {
    free(h);
    return set_err(s, v->henc && !v->hchecked ? -FS_EBADPW : -FS_EARCHIVE);
  }
  *out = h, *len = total;
  return 0;
}
static int r5_block(rx_vol_t *v) {
  rx_shared_t *s = v->s;
  unsigned char *h;
  size_t n, at = 4;
  int r = r5_read_header(v, &h, &n);
  if (r) return r > 0 ? (v->state = 3, 0) : -1; // end of the file
  uint64_t hs, type, fl, xs = 0, ds = 0;
  vint(h, n, &at, &hs);
  if (vint(h, n, &at, &type) || vint(h, n, &at, &fl) || ((fl & 1) && vint(h, n, &at, &xs)) || ((fl & 2) && vint(h, n, &at, &ds)) ||
      xs > n - at) {
    free(h);
    return set_err(s, -FS_EARCHIVE);
  }
  size_t spec = at, xa = n - (size_t)xs;
  if (type == 4) { // archive encryption: every header after this one is encrypted
    uint64_t ver, ef;
    if (vint(h, n, &at, &ver) || vint(h, n, &at, &ef) || at + 17 > n || ((ef & 1) && at + 29 > n)) return free(h), set_err(s, -FS_EARCHIVE);
    if (ver != 0) return free(h), set_err(s, -FS_EMETHOD);
    if (!s->pw[0]) return free(h), set_err(s, -FS_EPASSWORD);
    key_t5 *k = key5(s, h[at], h + at + 1);
    if (!k) return free(h), -1;
    if ((ef & 1) && (r = check5(s, k, h + at + 17)) < 0) return free(h), -1;
    v->hchecked = (ef & 1) && r > 0;
    memcpy(v->hkey, k->key, 32);
    v->henc = 1, s->used = 1;
    free(h);
    return 0;
  }
  if (type == 2 || type == 3) { // a file, or a service block (comment, ...): look for an encryption record
    uint64_t ff, unp, attr, comp, host, nl;
    size_t p = spec, crc_at = 0;
    if (vint(h, xa, &p, &ff) || vint(h, xa, &p, &unp) || vint(h, xa, &p, &attr)) goto plain;
    if (ff & 2) p += 4;
    if (ff & 4) crc_at = p, p += 4;
    if (p > xa || vint(h, xa, &p, &comp) || vint(h, xa, &p, &host) || vint(h, xa, &p, &nl) || nl > xa - p) goto plain;
    const char *name = (const char *)h + p;
    size_t name_end = p + (size_t)nl, enc = 0, enc_end = 0, b2 = 0;
    for (size_t q = xa; q < n;) { // extra records: size, type, data
      uint64_t rs, rt;
      size_t q0;
      if (vint(h, n, &q, &rs) || rs > n - q) goto plain;
      q0 = q;
      if (vint(h, n, &q, &rt)) goto plain;
      if (rt == 1) enc = q, enc_end = q0 + (size_t)rs;
      if (rt == 2) { // a hash record: type 0 is BLAKE2sp, 32 bytes
        uint64_t ht;
        size_t hq = q;
        if (!vint(h, q0 + (size_t)rs, &hq, &ht) && ht == 0 && hq + 32 <= q0 + (size_t)rs) b2 = hq;
      }
      q = q0 + (size_t)rs;
    }
    if (!enc) goto plain;
    // the encryption record: version, flags, KDF count, salt, IV, check value
    uint64_t ver, ef;
    size_t e = enc;
    if (vint(h, enc_end, &e, &ver) || vint(h, enc_end, &e, &ef) || e + 33 > enc_end || ((ef & 1) && e + 45 > enc_end)) {
      free(h);
      return set_err(s, -FS_EARCHIVE);
    }
    if (ver != 0) return free(h), set_err(s, -FS_EMETHOD);
    if (!s->pw[0]) return free(h), set_err(s, -FS_EPASSWORD);
    key_t5 *k = key5(s, h[e], h + e + 1);
    if (!k || ((ef & 1) && check5(s, k, h + e + 33) < 0)) return free(h), -1;
    if (ds & 15) return free(h), set_err(s, -FS_EARCHIVE);
    int tweaked = (ef & 2) != 0, stored = ((comp >> 7) & 7) == 0;
    // the new header: no encryption record; with a MAC, no checksum (FileDen checks the MAC itself)
    uint64_t nff = ff, out = ds;
    if (tweaked && crc_at) {
      nff &= ~(uint64_t)4;
      if (type == 2) add_mac(s, name, (size_t)nl, le32(h + crc_at), k->hk);
    }
    if (tweaked && b2 && type == 2) add_b2(s, name, (size_t)nl, h + b2, k->hk);
    if (stored && !(ff & (1 | 8))) out = pass_on(s, name, (size_t)nl, (fl & 8) != 0, (fl & 16) != 0, unp, ds);
    else if (!stored && ds) out = r5_stream_end(v, k->key, h + e + 17, ds, name, (size_t)nl, (fl & 8) != 0);
    buf_t sp = {0}, xb = {0}, b = {0};
    bvint(&sp, nff), bvint(&sp, unp), bvint(&sp, attr);
    { // copy mtime (and the CRC unless it's a MAC) as they were
      size_t q = spec;
      uint64_t t;
      vint(h, xa, &q, &t), vint(h, xa, &q, &t), vint(h, xa, &q, &t);
      if (ff & 2) bput(&sp, h + q, 4), q += 4;
      if (ff & 4) {
        if (nff & 4) bput(&sp, h + q, 4);
        q += 4;
      }
      bput(&sp, h + q, name_end - q); // compression, host OS, name
    }
    for (size_t q = xa; q < n;) {
      uint64_t rs, rt;
      size_t q0 = q;
      vint(h, n, &q, &rs);
      size_t q1 = q;
      vint(h, n, &q, &rt);
      size_t end = q1 + (size_t)rs;
      if (!(rt == 1 || (rt == 2 && tweaked))) bput(&xb, h + q0, end - q0);
      q = end;
    }
    uint64_t nfl = (fl & ~(uint64_t)1) | (xb.n ? 1 : 0);
    bvint(&b, type), bvint(&b, nfl);
    if (xb.n) bvint(&b, xb.n);
    if (fl & 2) bvint(&b, out);
    bput(&b, sp.p, sp.n), bput(&b, xb.p, xb.n);
    buf_t hd = {0};
    unsigned char crc[4] = {0};
    bput(&hd, crc, 4), bvint(&hd, b.n), bput(&hd, b.p, b.n);
    int bad = sp.bad || xb.bad || b.bad || hd.bad;
    if (!bad) put_le32(hd.p, (uint32_t)crc32(0, hd.p + 4, (uInt)(hd.n - 4))), emit(v, hd.p, hd.n);
    free(sp.p), free(xb.p), free(b.p), free(hd.p);
    if (bad) return free(h), set_err(s, -ENOMEM);
    v->dleft = ds, v->oleft = out, s->used = 1;
    if (ds && !(v->c = cbc(k->key, 256, h + e + 17))) return free(h), set_err(s, -ENOMEM);
    free(h);
    if (ds) v->state = 2;
    return 0;
  }
plain: // anything else goes through as it is (decrypted, if the headers were)
  emit(v, h, n);
  free(h);
  if (type == 5) v->state = 3;
  else if (ds) v->dleft = v->oleft = ds, v->state = 2;
  return s->err ? -1 : 0;
}

// ---- RAR 3.x / 4.x ----
static int r4_block(rx_vol_t *v) {
  rx_shared_t *s = v->s;
  unsigned char st[16], *h;
  size_t size;
  if (v->henc) { // an 8-byte salt, then the header in whole AES blocks
    unsigned char salt[8];
    int r = rd(v->fd, salt, 8);
    if (r) return r == -1 ? (v->state = 3, 0) : set_err(s, r);
    if (!s->pw[0]) return set_err(s, -FS_EPASSWORD);
    key_t5 *k = key3(s, salt);
    if (!k) return -1;
    if ((r = rd(v->fd, st, 16))) return set_err(s, r == -1 ? -FS_EARCHIVE : r);
    EVP_CIPHER_CTX *c = cbc(k->key, 128, k->iv);
    if (!c || dec(c, st, 16)) return EVP_CIPHER_CTX_free(c), set_err(s, -ENOMEM);
    size = le16(st + 5); // RAR 3.x has no password check: a wrong one shows as a header that makes no sense
    size_t full = (size + 15) & ~(size_t)15;
    if (size < 7 || !(h = malloc(full))) return EVP_CIPHER_CTX_free(c), set_err(s, size < 7 ? -FS_EBADPW : -ENOMEM);
    memcpy(h, st, 16);
    r = full > 16 ? rd(v->fd, h + 16, full - 16) : 0;
    if (!r && full > 16 && dec(c, h + 16, full - 16)) r = -ENOMEM;
    EVP_CIPHER_CTX_free(c);
    if (r) return free(h), set_err(s, r == -1 ? -FS_EBADPW : r);
    if ((crc32(0, h + 2, (uInt)(size - 2)) & 0xffff) != le16(h)) return free(h), set_err(s, -FS_EBADPW);
    s->used = 1;
  } else {
    int r = rd(v->fd, st, 7);
    if (r) return r == -1 ? (v->state = 3, 0) : set_err(s, r);
    size = le16(st + 5);
    if (size < 7 || !(h = malloc(size))) return set_err(s, size < 7 ? -FS_EARCHIVE : -ENOMEM);
    memcpy(h, st, 7);
    if (size > 7 && (r = rd(v->fd, h + 7, size - 7))) return free(h), set_err(s, r == -1 ? -FS_EARCHIVE : r);
  }
  unsigned type = h[2], fl = le16(h + 3);
  uint64_t ds = (fl & 0x8000) && size >= 11 ? le32(h + 7) : 0;
  if (type == 0x73) { // main header: headers after it are encrypted
    if (fl & 0x0080) {
      if (!s->pw[0]) return free(h), set_err(s, -FS_EPASSWORD);
      v->henc = 1;
      put_le16(h + 3, fl & ~0x0080u);
      put_le16(h, crc32(0, h + 2, (uInt)(size - 2)) & 0xffff);
    }
    emit(v, h, size);
    free(h);
    return s->err ? -1 : 0;
  }
  if ((type == 0x74 || type == 0x7a) && size >= 32) {
    size_t off = (fl & 0x0100) ? 40 : 32, nl = le16(h + 26);
    if ((fl & 0x0100) && size >= 40) ds |= (uint64_t)le32(h + 32) << 32;
    if ((fl & 0x0004) && off + nl + ((fl & 0x0400) ? 8 : 0) <= size) { // an encrypted file
      unsigned ver = h[24], method = h[25];
      if (ver < 29) return free(h), set_err(s, -FS_EMETHOD); // RAR 1.5 / 2.0 encryption
      if (!s->pw[0]) return free(h), set_err(s, -FS_EPASSWORD);
      const unsigned char *salt = (fl & 0x0400) ? h + off + nl : 0;
      key_t5 *k = key3(s, salt);
      if (!k) return free(h), -1;
      if (ds & 15) return free(h), set_err(s, -FS_EARCHIVE);
      uint64_t unp = le32(h + 11) | ((fl & 0x0100) && size >= 40 ? (uint64_t)le32(h + 36) << 32 : 0), out = ds;
      int dir = (fl & 0xe0) == 0xe0;
      if (method == 0x30 && !dir) out = pass_on(s, (const char *)h + off, nl, fl & 1, (fl & 2) != 0, unp, ds);
      // the new header: no password flag, no salt, the real size of stored data
      size_t cut = (fl & 0x0400) ? 8 : 0, ns = size - cut;
      unsigned char *g = malloc(ns);
      if (!g) return free(h), set_err(s, -ENOMEM);
      memcpy(g, h, off + nl);
      memcpy(g + off + nl, h + off + nl + cut, size - off - nl - cut);
      put_le16(g + 3, fl & ~0x0404u);
      put_le16(g + 5, (unsigned)ns);
      put_le32(g + 7, (uint32_t)out);
      if (fl & 0x0100) put_le32(g + 32, (uint32_t)(out >> 32));
      put_le16(g, crc32(0, g + 2, (uInt)(ns - 2)) & 0xffff);
      emit(v, g, ns);
      free(g);
      v->dleft = ds, v->oleft = out, s->used = 1;
      if (ds && !(v->c = cbc(k->key, 128, k->iv))) return free(h), set_err(s, -ENOMEM);
      free(h);
      if (ds) v->state = 2;
      return s->err ? -1 : 0;
    }
  }
  if (v->henc) put_le16(h, crc32(0, h + 2, (uInt)(size - 2)) & 0xffff); // unchanged, but now in the clear
  emit(v, h, size);
  free(h);
  if (type == 0x7b) v->state = 3;
  else if (ds) v->dleft = v->oleft = ds, v->state = 2;
  return s->err ? -1 : 0;
}

// ---- data ----
static int pump_data(rx_vol_t *v) {
  rx_shared_t *s = v->s;
  size_t k = v->dleft > RX_OUT ? RX_OUT : (size_t)v->dleft;
  if (!v->ib && !(v->ib = malloc(RX_OUT))) return set_err(s, -ENOMEM);
  int r = rd(v->fd, v->ib, k);
  if (r) return set_err(s, r == -1 ? -FS_EARCHIVE : r);
  if (v->c && dec(v->c, v->ib, k)) return set_err(s, -ENOMEM);
  v->dleft -= k;
  size_t pass = v->oleft < k ? (size_t)v->oleft : k;
  v->oleft -= pass;
  if (pass) emit(v, v->ib, pass);
  if (!v->dleft) {
    EVP_CIPHER_CTX_free(v->c);
    v->c = 0, v->state = 1;
  }
  return s->err ? -1 : 0;
}

long rx_read(rx_vol_t *v, const void **buf) {
  while (v->oat == v->on) {
    if (v->s->err) return -1;
    if (v->state == 3) return 0;
    v->oat = v->on = 0;
    int r;
    if (v->state == 0) { // the signature, as it is
      unsigned char sig[8];
      size_t n = v->ver == 5 ? 8 : 7;
      if ((r = rd(v->fd, sig, n)) || memcmp(sig, v->ver == 5 ? SIG5 : SIG4, n)) return set_err(v->s, r && r != -1 ? r : -FS_EARCHIVE);
      emit(v, sig, n);
      v->state = 1;
      continue;
    }
    r = v->state == 2 ? pump_data(v) : v->ver == 5 ? r5_block(v) : r4_block(v);
    if (r) return -1;
  }
  size_t k = v->on - v->oat;
  if (k > RX_OUT) k = RX_OUT;
  *buf = v->out + v->oat;
  v->oat += k;
  return (long)k;
}

static int sig_of(int fd) {
  unsigned char b[8];
  if (rd(fd, b, 8)) return 0;
  if (!memcmp(b, SIG5, 8)) return 5;
  if (!memcmp(b, SIG4, 7)) {
    lseek(fd, 7, SEEK_SET);
    return 4;
  }
  return 0;
}
rx_vol_t *rx_open(rx_shared_t *s, const char *path) {
  rx_vol_t *v = calloc(1, sizeof *v);
  if (!v) return set_err(s, -ENOMEM), (rx_vol_t *)0;
  v->s = s;
  if ((v->fd = open(path, O_RDONLY)) < 0) {
    set_err(s, -errno);
    free(v);
    return 0;
  }
  v->ver = sig_of(v->fd);
  lseek(v->fd, 0, SEEK_SET);
  if (!v->ver) {
    set_err(s, -FS_EARCHIVE);
    rx_close(v);
    return 0;
  }
  return v;
}
void rx_close(rx_vol_t *v) {
  if (!v) return;
  if (v->fd >= 0) close(v->fd);
  EVP_CIPHER_CTX_free(v->c);
  free(v->out), free(v->ib), free(v);
}
rx_shared_t *rx_shared_new(const char *password) {
  rx_shared_t *s = calloc(1, sizeof *s);
  if (s) snprintf(s->pw, sizeof s->pw, "%s", password ? password : "");
  return s;
}
void rx_shared_free(rx_shared_t *s) {
  if (!s) return;
  for (int i = 0; i < s->nmac; i++) free(s->mac[i].name);
  for (int i = 0; i < s->nleft; i++) free(s->left[i].name);
  free(s->mac), free(s->left);
  OPENSSL_cleanse(s, sizeof *s);
  free(s);
}
int rx_error(const rx_shared_t *s) { return s ? s->err : 0; }
int rx_decrypted(const rx_shared_t *s) { return s && s->used; }
int rx_verified(const rx_shared_t *s) { return s && s->verified; }

// does this RAR use a password? Walks the headers that are in the clear (seeking past the data).
int rx_detect(const char *path) {
  int fd = open(path, O_RDONLY), ver = fd < 0 ? 0 : sig_of(fd), found = 0;
  unsigned char h[64];
  for (int i = 0; ver && !found && i < 4096; i++) {
    if (ver == 5) {
      if (rd(fd, h, 5)) break;
      size_t got = 5, at = 4;
      while (got < 7 && (h[got - 1] & 0x80) && !rd(fd, h + got, 1)) got++;
      uint64_t hs, type, fl, xs = 0, ds = 0;
      if (vint(h, got, &at, &hs) || hs > RX_HDR_MAX) break;
      unsigned char *b = malloc((size_t)hs);
      if (!b || rd(fd, b, (size_t)hs)) {
        free(b);
        break;
      }
      size_t p = 0;
      if (vint(b, hs, &p, &type) || vint(b, hs, &p, &fl) || ((fl & 1) && vint(b, hs, &p, &xs)) || ((fl & 2) && vint(b, hs, &p, &ds)) ||
          xs > hs) {
        free(b);
        break;
      }
      if (type == 4) found = 1;
      for (size_t q = (size_t)(hs - xs); (type == 2 || type == 3) && q < hs && !found;) {
        uint64_t rs, rt;
        if (vint(b, hs, &q, &rs) || rs > hs - q) break;
        size_t q0 = q;
        if (vint(b, hs, &q, &rt)) break;
        if (rt == 1) found = 1;
        q = q0 + (size_t)rs;
      }
      free(b);
      if (type == 5 || lseek(fd, (off_t)ds, SEEK_CUR) < 0) break;
    } else {
      if (rd(fd, h, 7)) break;
      unsigned type = h[2], fl = le16(h + 3), size = le16(h + 5);
      if (size < 7) break;
      uint64_t ds = 0;
      if (size >= 11 && (fl & 0x8000)) {
        if (rd(fd, h + 7, 4)) break;
        ds = le32(h + 7);
        if (lseek(fd, size - 11, SEEK_CUR) < 0) break;
      } else if (lseek(fd, size - 7, SEEK_CUR) < 0) break;
      if ((type == 0x73 && (fl & 0x0080)) || ((type == 0x74 || type == 0x7a) && (fl & 0x0004))) found = 1;
      if (type == 0x7b || lseek(fd, (off_t)ds, SEEK_CUR) < 0) break;
    }
  }
  if (fd >= 0) close(fd);
  return found ? ver : 0;
}
