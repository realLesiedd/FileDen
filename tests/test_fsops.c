// FileDen - host tests for fsops.c (path checks, names, copy / move / delete jobs)
//   see the README (Build, on a PC) for the full compile line
// SPDX-License-Identifier: GPL-3.0-or-later
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../fsops.h"
#include "../title.h"

static int fails;
#define CHECK(c)                                                  \
  do {                                                            \
    if (!(c)) fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c), fails++; \
  } while (0)

static void put(const char *p, const char *s) {
  FILE *f = fopen(p, "w");
  if (f) fputs(s, f), fclose(f);
}
static int isfile(const char *p) {
  struct stat st;
  return !stat(p, &st) && S_ISREG(st.st_mode);
}
static int isdirp(const char *p) {
  struct stat st;
  return !stat(p, &st) && S_ISDIR(st.st_mode);
}
static void wait_job(void) {
  for (int i = 0; i < 500 && fs_job_running(); i++) usleep(10000);
}

int main(void) {
  // path and name rules
  CHECK(fs_path_ok("/"));
  CHECK(fs_path_ok("/data/a b/c.txt"));
  CHECK(!fs_path_ok("data"));
  CHECK(!fs_path_ok("/data/../etc"));
  CHECK(!fs_path_ok("/data//x"));
  CHECK(!fs_path_ok("/data/./x"));
  CHECK(!fs_path_ok("/data/"));
  CHECK(fs_name_ok("a.txt") && fs_name_ok(".hidden"));
  CHECK(!fs_name_ok("") && !fs_name_ok("a/b") && !fs_name_ok("..") && !fs_name_ok("."));
  // PS5 system paths
  CHECK(fs_guard_entry("/") && fs_guard_entry("/data") && fs_guard_entry("/mnt/usb0") && !fs_guard_entry("/mnt/usb0/x"));
  CHECK(fs_guard_entry("/system_data/priv/mms/app.db") == -FS_ESYSTEM && fs_guard_entry("/user/home/1000/savedata") == -FS_ESYSTEM);
  CHECK(fs_guard_entry("/user/app") && !fs_guard_entry("/user/app/PPSA01234"));
  CHECK(fs_guard_write("/system") && fs_guard_write("/dev") && fs_guard_write("/mnt") && !fs_guard_write("/data"));

  char root[] = "/tmp/fileden-test-XXXXXX", p[1100], q[1100], o[4096];
  if (!mkdtemp(root)) return 1;
  snprintf(p, sizeof p, "%s/src", root);
  CHECK(!fs_mkdir(p));
  CHECK(fs_mkdir(p) < 0); // already there
  snprintf(p, sizeof p, "%s/src/a.txt", root);
  put(p, "hello");
  snprintf(p, sizeof p, "%s/src/sub", root);
  fs_mkdir(p);
  snprintf(p, sizeof p, "%s/src/sub/b.txt", root);
  put(p, "world");
  snprintf(p, sizeof p, "%s/dst", root);
  fs_mkdir(p);

  // unique names
  snprintf(p, sizeof p, "%s/src", root);
  CHECK(!fs_unique(p, "a.txt", q, sizeof q) && strstr(q, "/a (2).txt"));
  CHECK(!fs_unique(p, "new.txt", q, sizeof q) && strstr(q, "/new.txt"));

  // new file, save, rename
  snprintf(p, sizeof p, "%s/src/n.txt", root);
  CHECK(!fs_newfile(p));
  CHECK(fs_newfile(p) < 0);
  CHECK(!fs_save(p, "line\n", 5));
  CHECK(!fs_rename(p, "m.txt", q, sizeof q) && isfile(q));
  CHECK(fs_rename(q, "../x", o, sizeof o) < 0);

  // listing: folders first, hidden skipped unless asked
  snprintf(p, sizeof p, "%s/src/.dot", root);
  put(p, "x");
  snprintf(p, sizeof p, "%s/src", root);
  char *l = fs_list(p, 0, 1);
  CHECK(l && !strncmp(l, "d\t", 2) && !strstr(l, ".dot"));
  free(l);
  l = fs_list(p, 1, 0);
  CHECK(l && strstr(l, "\".dot\""));
  free(l);

  // copy a folder into another, then into itself (refused)
  snprintf(o, sizeof o, "%s/src", root);
  snprintf(q, sizeof q, "%s/dst", root);
  CHECK(!fs_job_start(JOB_COPY, o, q, 0));
  wait_job();
  snprintf(p, sizeof p, "%s/dst/src/sub/b.txt", root);
  CHECK(isfile(p));
  snprintf(q, sizeof q, "%s/src/sub", root);
  CHECK(fs_job_start(JOB_COPY, o, q, 0) < 0);
  // copy into the same folder: "src (2)"
  CHECK(!fs_job_start(JOB_COPY, o, root, 0));
  wait_job();
  snprintf(p, sizeof p, "%s/src (2)/a.txt", root);
  CHECK(isfile(p));

  // move two items
  snprintf(o, sizeof o, "%s/src (2)/a.txt\n%s/src (2)/sub", root, root);
  snprintf(q, sizeof q, "%s/dst", root);
  CHECK(!fs_job_start(JOB_MOVE, o, q, 0));
  wait_job();
  snprintf(p, sizeof p, "%s/dst/a.txt", root);
  CHECK(isfile(p));
  snprintf(p, sizeof p, "%s/dst/sub/b.txt", root);
  CHECK(isfile(p));
  snprintf(p, sizeof p, "%s/src (2)/a.txt", root);
  CHECK(!isfile(p));
  fs_job_json(o, sizeof o);
  CHECK(strstr(o, "\"state\":\"done\"") != 0);

  // protected paths can't be deleted or moved
  CHECK(fs_job_start(JOB_DELETE, "/data", 0, 0) < 0);
  CHECK(fs_job_start(JOB_MOVE, "/", root, 0) < 0);

  // zip and 7z round trips, extracted as 0777
  for (int kind = 0; kind < 2; kind++) {
    snprintf(o, sizeof o, "%s/dst", root);
    snprintf(q, sizeof q, "%s/t.%s", root, kind ? "7z" : "zip");
    CHECK(!fs_job_start(kind ? JOB_7Z : JOB_ZIP, o, q, 0));
    wait_job();
    CHECK(!fs_job_start(JOB_EXTRACT, q, q, 0)); // unpacks into "t (2).zip" style folder next to it
    wait_job();
    fs_job_json(o, sizeof o);
    CHECK(strstr(o, "\"state\":\"done\"") != 0);
    char *r = strstr(o, "\"result\":\"");
    char out[1100] = "";
    if (r) snprintf(out, sizeof out, "%.*s", (int)strcspn(r + 10, "\""), r + 10);
    snprintf(p, sizeof p, "%s/dst/sub/b.txt", out);
    struct stat st;
    CHECK(!stat(p, &st) && (st.st_mode & 0777) == 0777);
  }
  // SHA-256 of "hello" (a.txt)
  snprintf(p, sizeof p, "%s/dst/a.txt", root);
  CHECK(!fs_job_start(JOB_HASH, p, "", 0));
  wait_job();
  fs_job_json(o, sizeof o);
  CHECK(strstr(o, "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824") != 0);

  // Undo: a rename, then a move, put back
  {
    char u1[1100], u2[1100], out[1300], w[400];
    snprintf(u1, sizeof u1, "%s/undo-a.txt", root);
    put(u1, "x");
    CHECK(!fs_rename(u1, "undo-b.txt", out, sizeof out));
    CHECK(fs_undo_info(w, sizeof w) == 1 && strstr(w, "undo-b.txt") && strstr(w, "undo-a.txt"));
    CHECK(!fs_undo() && isfile(u1));
    CHECK(fs_undo_info(w, sizeof w) == 0 && fs_undo() == -ENOENT); // once
    snprintf(u2, sizeof u2, "%s/undo-dir", root);
    fs_mkdir(u2);
    CHECK(!fs_job_start(JOB_MOVE, u1, u2, 0));
    wait_job();
    snprintf(out, sizeof out, "%s/undo-a.txt", u2);
    CHECK(isfile(out) && !isfile(u1));
    CHECK(fs_undo_info(w, sizeof w) == 2);
    CHECK(!fs_undo() && isfile(u1) && !isfile(out));
    // put back over something that took its place: refused
    CHECK(!fs_rename(u1, "undo-c.txt", out, sizeof out));
    put(u1, "new");
    CHECK(fs_undo() == -EEXIST);
  }

  // games, apps and packages: their names (and icons) in the listing
  {
    char hb[1100], q2[1300];
    snprintf(hb, sizeof hb, "%s/homebrew", root);
    fs_mkdir(hb);
    snprintf(q2, sizeof q2, "%s/MyApp", hb), fs_mkdir(q2);
    snprintf(q2, sizeof q2, "%s/MyApp/sce_sys", hb), fs_mkdir(q2);
    snprintf(q2, sizeof q2, "%s/MyApp/sce_sys/param.json", hb);
    put(q2, "{\"titleId\":\"PPSA01234\",\"localizedParameters\":{\"defaultLanguage\":\"en-US\",\"en-US\":{\"titleName\":\"My \\u00c9 App\"}}}");
    snprintf(q2, sizeof q2, "%s/Plain", hb), fs_mkdir(q2);
    // a PS4-style package with a param.sfo entry (TITLE)
    snprintf(q2, sizeof q2, "%s/game.pkg", hb);
    FILE *f = fopen(q2, "wb");
    unsigned char h[0x200] = {0x7f, 'C', 'N', 'T'};
    h[0x13] = 1, h[0x18] = 0, h[0x19] = 0, h[0x1a] = 0x01, h[0x1b] = 0x00; // 1 entry, table at 0x100
    memcpy(h + 0x40, "UP0000-CUSA00042_00-TEST000000000000", 36);
    unsigned char sfo[64] = {0, 'P', 'S', 'F', 1, 1, 0, 0, 36, 0, 0, 0, 44, 0, 0, 0, 1, 0, 0, 0};
    // one index entry: key 0, utf8, len 5, max 8, data 0; keys "TITLE\0"; data "Game\0"
    unsigned char ix[16] = {0, 0, 4, 2, 5, 0, 0, 0, 8, 0, 0, 0, 0, 0, 0, 0};
    memcpy(sfo + 20, ix, 16), memcpy(sfo + 36, "TITLE\0\0\0", 8), memcpy(sfo + 44, "Game\0", 5);
    unsigned char e[32] = {0, 0, 0x10, 0}; // id 0x1000 (param.sfo) at 0x180, 64 bytes
    e[18] = 0x01, e[19] = 0x80, e[23] = 64;
    memcpy(h + 0x100, e, 32), memcpy(h + 0x180, sfo, 64);
    if (f) fwrite(h, 1, sizeof h, f), fclose(f);
    char *l = fs_list(hb, 0, 1);
    CHECK(l && strstr(l, "\tMyApp\tPPSA01234\tMy \xc3\x89 App\t0\n"));
    CHECK(l && strstr(l, "\tPlain\n"));
    CHECK(l && strstr(l, "\tgame.pkg\tCUSA00042\tGame\t0\n"));
    if (l && !strstr(l, "game.pkg\tCUSA00042")) fprintf(stderr, "%s\n", l);
    free(l);
    // kept between listings, and read again once the title changes (the editor saves through a rename)
    snprintf(q2, sizeof q2, "%s/MyApp/sce_sys/param.json.new", hb);
    put(q2, "{\"titleId\":\"PPSA01234\",\"titleName\":\"Renamed\"}");
    char q3[1300];
    snprintf(q3, sizeof q3, "%s/MyApp/sce_sys/param.json", hb);
    rename(q2, q3);
    l = fs_list(hb, 0, 1);
    CHECK(l && strstr(l, "\tMyApp\tPPSA01234\tRenamed\t0\n"));
    free(l);
    // an /user/appmeta-style folder: icon0.png and param.json right inside it
    snprintf(q2, sizeof q2, "%s/PPSA05678", hb), fs_mkdir(q2);
    snprintf(q2, sizeof q2, "%s/PPSA05678/param.json", hb);
    put(q2, "{\"titleId\":\"PPSA05678\",\"titleName\":\"Meta\"}");
    snprintf(q2, sizeof q2, "%s/PPSA05678/icon0.png", hb);
    put(q2, "png");
    l = fs_list(hb, 0, 1);
    CHECK(l && strstr(l, "\tPPSA05678\tPPSA05678\tMeta\t1\n"));
    free(l);
    snprintf(q2, sizeof q2, "%s/PPSA05678", hb);
    CHECK(!title_icon_path(q2, q3, sizeof q3) && strstr(q3, "PPSA05678/icon0.png"));
  }

  // delete everything
  CHECK(!fs_job_start(JOB_DELETE, root, 0, 0));
  wait_job();
  CHECK(!isdirp(root));

  printf(fails ? "%d FAILED\n" : "all fsops tests passed\n", fails);
  return fails ? 1 : 0;
}
