// FileDen: names and icons of games, apps and packages (sce_sys/param.json or param.sfo, icon0.png).
#pragma once
#include <stddef.h>

typedef struct {
  char id[16];    // PPSA01234 / CUSA12345, if known
  char name[160]; // the title's name
  int icon;       // an icon0.png is there
} title_t;

int title_id_like(const char *name);
// 0 with t filled for a game / app folder or a .pkg; -1 for anything else
int title_of(const char *path, int isdir, title_t *t);
// a title folder's icon0.png (sce_sys/, the folder itself, or /user/appmeta/<ID>): 0 with its path in o
int title_icon_path(const char *dir, char *o, size_t n);
// the icon0.png inside a package (malloc'd), or 0
unsigned char *title_pkg_icon(const char *path, size_t *n);
