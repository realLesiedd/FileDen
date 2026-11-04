// FileDen - host tests for archive.c: TAR round trips, single compressed files, split sets and
// archives made elsewhere (RAR, RAR5, multi-part RAR, CAB, LHA, ZIPX, 7z with zstd, .Z, ISO).
//   test_archives tests/fixtures      (see the README for the compile line)
// The fixtures come from libarchive's own test suite (BSD-2-Clause).
// SPDX-License-Identifier: GPL-3.0-or-later
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../archive.h"
#include "../fsops.h"
// a password for the next job started here, used once (as the apps send one with each request)
static char tpw[256];
#define fs_job_password(p) snprintf(tpw, sizeof tpw, "%s", (p))
static int start_job(int op, const char *paths, const char *dest, int arg) {
  int rc = fs_job_start_pw(op, paths, dest, arg, tpw);
  tpw[0] = 0;
  return rc;
}
#define fs_job_start start_job

static int fails;
#define CHECK(c)                                                                     \
  do {                                                                               \
    if (!(c)) fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c), fails++; \
  } while (0)

static char root[] = "/tmp/fileden-arc-XXXXXX", fix[1024];
static void put(const char *p, const char *s) {
  FILE *f = fopen(p, "w");
  if (f) fputs(s, f), fclose(f);
}
static long long fsize(const char *p) {
  struct stat st;
  return stat(p, &st) || !S_ISREG(st.st_mode) ? -1 : (long long)st.st_size;
}
static int mode_of(const char *p) {
  struct stat st;
  return stat(p, &st) ? -1 : (int)(st.st_mode & 07777);
}
static void wait_job(void) {
  for (int i = 0; i < 1000 && fs_job_running(); i++) usleep(10000);
}
static void copy_fix(const char *name) { // fixtures are copied: extracting writes next to the archive
  char cmd[4096];
  snprintf(cmd, sizeof cmd, "cp '%s/%s' '%s/'", fix, name, root);
  if (system(cmd)) fprintf(stderr, "can't copy %s\n", name);
}
// extracts root/name; result: where it went ("" on failure), err: the job's message
static int extract(const char *name, char *result, size_t rn, char *err, size_t en) {
  char a[1200], o[4096];
  snprintf(a, sizeof a, "%s/%s", root, name);
  char out[600], dest[1900];
  if (ar_out_name(a, out, sizeof out)) return -1;
  snprintf(dest, sizeof dest, "%s/%s", root, out);
  int rc = fs_job_start(JOB_EXTRACT, a, dest, 0);
  if (rc) return snprintf(err, en, "start: %s", fs_err(rc)), rc;
  wait_job();
  fs_job_json(o, sizeof o);
  char *r = strstr(o, "\"result\":\""), *e = strstr(o, "\"err\":\"");
  snprintf(result, rn, "%.*s", r ? (int)strcspn(r + 10, "\"") : 0, r ? r + 10 : "");
  snprintf(err, en, "%.*s", e ? (int)strcspn(e + 7, "\"") : 0, e ? e + 7 : "");
  return strstr(o, "\"state\":\"done\"") ? 0 : 1;
}
static void name_is(const char *in, const char *want) {
  char o[600] = "";
  ar_out_name(in, o, sizeof o);
  if (strcmp(o, want)) fprintf(stderr, "FAIL name %s -> %s (want %s)\n", in, o, want), fails++;
}

int main(int argc, char **argv) {
  snprintf(fix, sizeof fix, "%s", argc > 1 ? argv[1] : "tests/fixtures");
  if (fix[0] != '/') { // absolute, for cp
    char cwd[512];
    if (getcwd(cwd, sizeof cwd)) {
      char t[1024];
      snprintf(t, sizeof t, "%s/%s", cwd, fix);
      snprintf(fix, sizeof fix, "%s", t);
    }
  }
  if (!mkdtemp(root)) return 1;
  char p[1200], q[1200], res[1100], err[400];

  // what archives unpack to
  name_is("/d/game.tar.gz", "game");
  name_is("/d/Game.TAR.XZ", "Game");
  name_is("/d/game.tgz", "game");
  name_is("/d/notes.txt.gz", "notes.txt");
  name_is("/d/game.part01.rar", "game");
  name_is("/d/game.part2.rar", "game");
  name_is("/d/game.7z.001", "game");
  name_is("/d/game.zip.002", "game");
  name_is("/d/dump.001", "dump");
  name_is("/d/a.zip", "a");
  name_is("/d/.zip", ".zip"); // a hidden name, not an extension
  name_is("/d/README", "README");

  // a source tree
  snprintf(p, sizeof p, "%s/src", root);
  fs_mkdir(p);
  snprintf(p, sizeof p, "%s/src/a.txt", root);
  put(p, "hello");
  snprintf(p, sizeof p, "%s/src/sub", root);
  fs_mkdir(p);
  snprintf(p, sizeof p, "%s/src/sub/b.txt", root);
  put(p, "world, a little longer");

  // TAR round trips, every compression
  static const char *const ext[] = {"tar", "tar.gz", "tar.xz", "tar.bz2", "tar.zst"};
  for (int f = TF_NONE; f <= TF_ZST; f++) {
    snprintf(p, sizeof p, "%s/src", root);
    snprintf(q, sizeof q, "%s/t%d.%s", root, f, ext[f]);
    CHECK(!fs_job_start(JOB_TAR, p, q, f));
    wait_job();
    CHECK(fsize(q) > 0 && mode_of(q) == 0777);
    snprintf(p, sizeof p, "t%d.%s", f, ext[f]);
    CHECK(ar_kind(q) == AR_MULTI);
    int rc = extract(p, res, sizeof res, err, sizeof err);
    if (rc) fprintf(stderr, "  %s: %s\n", p, err);
    CHECK(!rc);
    snprintf(p, sizeof p, "%s/src/sub/b.txt", res);
    CHECK(fsize(p) == 22 && mode_of(p) == 0777);
  }
  CHECK(fs_job_start(JOB_TAR, "/tmp", "/tmp/x.tar", 9) < 0); // unknown compression

  // single compressed files become a file next to them
  static const char *const tool[] = {"gzip", "xz", "bzip2"}, *const sfx[] = {"gz", "xz", "bz2"};
  for (int i = 0; i < 3; i++) {
    char cmd[2600];
    snprintf(p, sizeof p, "%s/one%d.txt", root, i);
    put(p, "single file contents\n");
    snprintf(cmd, sizeof cmd, "%s -q '%s'", tool[i], p);
    if (system(cmd)) {
      fprintf(stderr, "skip: no %s\n", tool[i]);
      continue;
    }
    snprintf(q, sizeof q, "%s.%s", p, sfx[i]);
    CHECK(ar_kind(q) == AR_SINGLE);
    snprintf(q, sizeof q, "one%d.txt.%s", i, sfx[i]);
    CHECK(!extract(q, res, sizeof res, err, sizeof err));
    CHECK(fsize(res) == 21 && strstr(res, "/one") && mode_of(res) == 0777);
  }
  snprintf(p, sizeof p, "%s/plain.txt", root);
  put(p, "not an archive at all, just words");
  CHECK(ar_kind(p) == AR_NONE);
  CHECK(fs_job_start(JOB_EXTRACT, p, p, 0) == -FS_EARCHIVE);

  // a 7z split into .001 / .002 / .003; any part can be picked
  snprintf(p, sizeof p, "%s/src", root);
  snprintf(q, sizeof q, "%s/split.7z", root);
  CHECK(!fs_job_start(JOB_7Z, p, q, 0));
  wait_job();
  {
    char cmd[2600];
    snprintf(cmd, sizeof cmd, "cd '%s' && split -b 60 -d -a 3 --numeric-suffixes=1 split.7z split.7z. && rm split.7z", root);
    CHECK(!system(cmd));
    CHECK(!extract("split.7z.002", res, sizeof res, err, sizeof err));
    snprintf(p, sizeof p, "%s/src/a.txt", res);
    CHECK(fsize(p) == 5);
  }

  // made elsewhere
  struct {
    const char *arc, *file;
    long long size;
  } t[] = {
      {"rar.rar", "testdir/test.txt", 20},
      {"rar5_stored.rar", "helloworld.txt", 29},
      {"cab_1.cab", "dir2/file2", 78},
      {"lha_lh6.lzh", "file2", 78},
      {"7zip_zstd.7z", "dir1/file1", 13},     // 7z method FileDen's reader lacks: libarchive
      {"zip_zstd.zipx", "vimrc", 912},         // ZIP signature, zstd method: libarchive
      {"rar_multivolume_multiple_files.part3.rar", "LibarchiveAddingTest.html", 20111},
      {"rar5_multiarchive.part05.rar", "home/antek/temp/build/unrar5/libarchive/bin/bsdcat_test", 144608},
  };
  for (size_t i = 0; i < sizeof t / sizeof *t; i++) {
    if (strstr(t[i].arc, ".part")) { // every part
      char cmd[4096], stem[200];
      snprintf(stem, sizeof stem, "%.*s", (int)(strstr(t[i].arc, ".part") - t[i].arc), t[i].arc);
      snprintf(cmd, sizeof cmd, "cp '%s/%s'.part*.rar '%s/'", fix, stem, root);
      CHECK(!system(cmd));
    } else copy_fix(t[i].arc);
    int rc = extract(t[i].arc, res, sizeof res, err, sizeof err);
    if (rc) fprintf(stderr, "  %s: %s\n", t[i].arc, err);
    CHECK(!rc);
    snprintf(p, sizeof p, "%s/%s", res, t[i].file);
    long long s = fsize(p);
    if (s != t[i].size) fprintf(stderr, "FAIL %s: %s is %lld bytes\n", t[i].arc, t[i].file, s), fails++;
  }
  // symbolic links inside are left out
  snprintf(p, sizeof p, "%s/rar/testlink", root);
  struct stat st;
  CHECK(lstat(p, &st) != 0);
  // an ISO inside .Z compression: read straight through, like a .tar.gz
  copy_fix("iso.iso.Z");
  CHECK(!extract("iso.iso.Z", res, sizeof res, err, sizeof err) && mode_of(res) == 0777 && fsize(res) < 0);
  // RAR with a password: RAR5 (data only, and names hidden too), RAR 3.x (data, names, some files)
  {
    struct {
      const char *arc, *pw, *file;
      long long size;
    } r[] = {{"rar5-psw.rar", "password", "stest1.txt", 2048},          {"rar5-psw.rar", "password", "stest2.txt", 2048},
             {"rar5-hpsw.rar", "password", "stest1.txt", 2048},         {"rar5-psw-blake.rar", "password", "stest2.txt", 2048},         {"rar_encryption_data.rar", "12345678", "foo.txt", 16},
             {"rar_encryption_header.rar", "12345678", "bar.txt", 16},  {"rar_encryption_partially.rar", "12345678", "foo.txt", 16}};
    for (size_t i = 0; i < sizeof r / sizeof *r; i++) {
      char o[4096];
      copy_fix(r[i].arc);
      fs_job_password("");
      CHECK(extract(r[i].arc, res, sizeof res, err, sizeof err) == 1);
      fs_job_json(o, sizeof o);
      if (!strstr(o, "\"need_pw\":1")) fprintf(stderr, "FAIL %s without a password: %s\n", r[i].arc, err), fails++;
      fs_job_password("not it");
      CHECK(extract(r[i].arc, res, sizeof res, err, sizeof err) == 1);
      fs_job_json(o, sizeof o);
      if (!strstr(o, "\"need_pw\":2")) fprintf(stderr, "FAIL %s with a wrong password: %s\n", r[i].arc, err), fails++;
      fs_job_password(r[i].pw);
      int rc = extract(r[i].arc, res, sizeof res, err, sizeof err);
      if (rc) fprintf(stderr, "  %s: %s\n", r[i].arc, err);
      CHECK(!rc);
      snprintf(p, sizeof p, "%s/%s", res, r[i].file);
      if (fsize(p) != r[i].size) fprintf(stderr, "FAIL %s: %s is %lld bytes\n", r[i].arc, r[i].file, fsize(p)), fails++;
    }
    // damage inside an encrypted file, with the right password: the checksum (a MAC) catches it
    char cmd[2600];
    snprintf(cmd, sizeof cmd, "cd '%s' && cp rar5-psw.rar bad5.rar && printf 'Z' | dd of=bad5.rar bs=1 seek=2300 conv=notrunc 2>/dev/null", root);
    CHECK(!system(cmd));
    fs_job_password("password");
    CHECK(extract("bad5.rar", res, sizeof res, err, sizeof err) == 1);
    CHECK(strstr(err, "damaged") != 0);
    // the same with WinRAR's BLAKE2sp checksum
    snprintf(cmd, sizeof cmd, "cd '%s' && cp rar5-psw-blake.rar badb.rar && printf 'Z' | dd of=badb.rar bs=1 seek=2380 conv=notrunc 2>/dev/null", root);
    CHECK(!system(cmd));
    fs_job_password("password");
    CHECK(extract("badb.rar", res, sizeof res, err, sizeof err) == 1);
    if (!strstr(err, "damaged")) fprintf(stderr, "FAIL badb.rar: %s\n", err), fails++;
  }

  // ZIP with a password (AES-256): asks for it, refuses a wrong one, opens with the right one
  {
    char o[4096];
    snprintf(p, sizeof p, "%s/src", root);
    snprintf(q, sizeof q, "%s/secret.zip", root);
    fs_job_password("pa55 word");
    CHECK(!fs_job_start(JOB_ZIP, p, q, 0));
    wait_job();
    FILE *f = fopen(q, "rb");
    char z[4096];
    size_t zn = f ? fread(z, 1, sizeof z, f) : 0;
    if (f) fclose(f);
    CHECK(zn > 30 && memmem(z, zn, "\x01\x99", 2) != 0); // the WinZip AES extra field
    CHECK(extract("secret.zip", res, sizeof res, err, sizeof err) == 1);
    fs_job_json(o, sizeof o);
    CHECK(strstr(o, "\"need_pw\":1") != 0);
    fs_job_password("wrong");
    CHECK(extract("secret.zip", res, sizeof res, err, sizeof err) == 1);
    fs_job_json(o, sizeof o);
    CHECK(strstr(o, "\"need_pw\":2") != 0);
    fs_job_password("pa55 word");
    CHECK(!extract("secret.zip", res, sizeof res, err, sizeof err));
    snprintf(p, sizeof p, "%s/src/sub/b.txt", res);
    CHECK(fsize(p) == 22);
  }

  // 7z with a password (AES-256, names hidden too): the same three answers
  {
    char o[4096], cmd[2600];
    snprintf(p, sizeof p, "%s/src/big.bin", root); // bigger than one buffer, streamed through AES
    FILE *f = fopen(p, "wb");
    for (int i = 0; f && i < 3 << 20; i++) fputc((i * 7 + i / 977) & 0xff, f);
    if (f) fclose(f);
    snprintf(p, sizeof p, "%s/src", root);
    snprintf(q, sizeof q, "%s/secret.7z", root);
    fs_job_password("pa55 word");
    CHECK(!fs_job_start(JOB_7Z, p, q, 0));
    wait_job();
    f = fopen(q, "rb");
    static char z[4 << 20];
    size_t zn = f ? fread(z, 1, sizeof z, f) : 0;
    if (f) fclose(f);
    CHECK(zn > 32 && !memmem(z, zn, "b\0.\0t\0x\0t", 8)); // no file names in the clear
    CHECK(extract("secret.7z", res, sizeof res, err, sizeof err) == 1);
    fs_job_json(o, sizeof o);
    CHECK(strstr(o, "\"need_pw\":1") != 0);
    fs_job_password("wrong");
    CHECK(extract("secret.7z", res, sizeof res, err, sizeof err) == 1);
    fs_job_json(o, sizeof o);
    CHECK(strstr(o, "\"need_pw\":2") != 0);
    fs_job_password("pa55 word");
    int rc = extract("secret.7z", res, sizeof res, err, sizeof err);
    if (rc) fprintf(stderr, "  secret.7z: %s\n", err);
    CHECK(!rc);
    snprintf(p, sizeof p, "%s/src/sub/b.txt", res);
    CHECK(fsize(p) == 22);
    snprintf(cmd, sizeof cmd, "cmp -s '%s/src/big.bin' '%s/src/big.bin'", root, res);
    CHECK(!system(cmd));
    // 7-Zip itself agrees, when it's here (FD_7ZZ=/path/to/7zz)
    const char *zz = getenv("FD_7ZZ");
    if (zz) {
      snprintf(cmd, sizeof cmd, "'%s' t -p'pa55 word' '%s' >/dev/null", zz, q);
      CHECK(!system(cmd));
    }
  }
  // made by 7-Zip with a password and hidden names (an x86 file inside gets the BCJ filter too)
  copy_fix("7zip_aes.7z");
  fs_job_password("");
  CHECK(extract("7zip_aes.7z", res, sizeof res, err, sizeof err) == 1);
  fs_job_password("FileDen");
  {
    int rc = extract("7zip_aes.7z", res, sizeof res, err, sizeof err);
    if (rc) fprintf(stderr, "  7zip_aes.7z: %s\n", err);
    CHECK(!rc);
    snprintf(p, sizeof p, "%s/notes.txt", res);
    CHECK(fsize(p) == 6000);
  }

  // hostile names: nothing may land outside the folder ("/x" lands inside it), and links are never made
  {
    char cmd[3000];
    snprintf(cmd, sizeof cmd,
             "cd '%s' && python3 -c \"import zipfile,tarfile,io\n"
             "z=zipfile.ZipFile('evil.zip','w')\n"
             "for n in ['../evil1.txt','/evil2.txt','a/../../evil3.txt','..\\\\\\\\evil4.txt','ok/./good.txt']: z.writestr(n,'x')\n"
             "z.close()\n"
             "t=tarfile.open('evil.tar','w')\n"
             "l=tarfile.TarInfo('link'); l.type=tarfile.SYMTYPE; l.linkname='..'; t.addfile(l)\n"
             "for n in ['link/evil5.txt','../evil6.txt','ok/good.txt']:\n"
             " i=tarfile.TarInfo(n); i.size=1; t.addfile(i,io.BytesIO(b'x'))\n"
             "t.close()\"",
             root);
    if (!system(cmd)) {
      static const char *const arc[] = {"evil.zip", "evil.tar"};
      for (int i = 0; i < 2; i++) {
        int rc = extract(arc[i], res, sizeof res, err, sizeof err);
        if (rc) fprintf(stderr, "  %s: %s\n", arc[i], err);
        CHECK(!rc);
        snprintf(p, sizeof p, "%s/ok/good.txt", res);
        CHECK(fsize(p) == 1);
        snprintf(p, sizeof p, "%s/link", res);
        struct stat st;
        CHECK(lstat(p, &st) || !S_ISLNK(st.st_mode)); // no symlink made
      }
      static const char *const out[] = {"evil1.txt", "evil2.txt", "evil3.txt", "evil4.txt", "evil5.txt", "evil6.txt", 0};
      for (int i = 0; out[i]; i++) {
        snprintf(p, sizeof p, "%s/%s", root, out[i]);
        CHECK(fsize(p) < 0);
        snprintf(p, sizeof p, "/%s", out[i]);
        CHECK(fsize(p) < 0);
      }
    } else fprintf(stderr, "  (no python3: hostile-name test skipped)\n");
  }

  if (getenv("FD_KEEP")) printf("kept %s\n", root);
  else {
    CHECK(!fs_job_start(JOB_DELETE, root, 0, 0));
    wait_job();
  }
  printf(fails ? "%d FAILED\n" : "all archive tests passed\n", fails);
  return fails ? 1 : 0;
}
