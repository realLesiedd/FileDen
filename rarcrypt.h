// FileDen: RAR archives with a password (RAR5 AES-256, RAR 3.x/4.x AES-128).
// libarchive reads RAR but can't decrypt it, so a volume is read through rx_read(), which hands
// libarchive the same archive with the headers and file data decrypted and the encryption records
// taken out. Written from the RAR 5.0 format notes; no unRAR code.
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct rx_shared rx_shared_t; // one per extract: the password, keys, files split across volumes
typedef struct rx_vol rx_vol_t;       // one per volume being read

int rx_detect(const char *path); // 5: RAR5 with encryption, 4: RAR 3.x/4.x with encryption, 0: neither
rx_shared_t *rx_shared_new(const char *password);
void rx_shared_free(rx_shared_t *s);
rx_vol_t *rx_open(rx_shared_t *s, const char *path);
long rx_read(rx_vol_t *v, const void **buf); // bytes ready, 0 at the end, -1 on error (rx_error says why)
void rx_close(rx_vol_t *v);
int rx_error(const rx_shared_t *s);      // -FS_EPASSWORD, -FS_EBADPW, -FS_EMETHOD, -FS_EARCHIVE or -errno; 0 if none
int rx_decrypted(const rx_shared_t *s);  // something was decrypted
int rx_verified(const rx_shared_t *s);   // the archive's check value confirmed the password (RAR5)
// a RAR5 file whose checksum the archive keeps as a MAC (WinRAR's default with a password):
// 1 it matches, 0 nothing to check, -1 it doesn't
int rx_check_crc(rx_shared_t *s, const char *name, uint32_t crc);
// the same for WinRAR's optional BLAKE2sp checksum (a MAC too)
int rx_wants_blake2(const rx_shared_t *s, const char *name);
int rx_check_blake2(rx_shared_t *s, const char *name, const unsigned char digest[32]);
