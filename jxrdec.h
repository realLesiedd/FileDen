#pragma once
#include <stddef.h>
// decode a .jxr (JPEG XR) file and encode it as JPEG.
// maxdim > 0 downsizes so the longest side is <= maxdim. HDR (half/float) is tone-mapped to SDR.
// returns 0 on success, *out must be free()d.
int jxr_to_jpeg(const char *path, int maxdim, int quality, unsigned char **out, size_t *outlen);
// details of the last decode (size, pixel format GUID, failure reason)
extern char jxr_last_err[256];
