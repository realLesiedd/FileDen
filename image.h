// FileDen - any image FileDen can read, as a JPEG that fits a box (for the TV app and the web UI)
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stddef.h>

int img_supported(const char *path); // by extension
// a JPEG no bigger than mw x mh (never enlarged); *w, *h: the picture's own size. 0 or -errno
int img_to_jpeg(const char *path, int mw, int mh, unsigned char **out, size_t *len, int *w, int *h);
