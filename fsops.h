// FileDen - file operations used by the payload's web API
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stddef.h>

enum { JOB_COPY, JOB_MOVE, JOB_DELETE, JOB_ZIP, JOB_7Z, JOB_EXTRACT, JOB_HASH, JOB_SIZE, JOB_CHMOD, JOB_TAR };
#define FS_ESYSTEM 10001  // a PS5 system file or folder: FileDen won't change it
#define FS_EARCHIVE 10002 // not an archive FileDen can read
#define FS_EMETHOD 10003  // the archive uses a method or encryption FileDen doesn't support
#define FS_EPASSWORD 10004 // the archive is encrypted and no password was given
#define FS_EBADPW 10005   // the password given doesn't open it
#define FS_MODE 0777      // everything FileDen writes: the console only starts apps whose files are 0777

// absolute, no empty/"."/".." components, shorter than PATH_MAX
int fs_path_ok(const char *p);
// a single name for a new or renamed entry: not empty, no '/', not "." or ".."
int fs_name_ok(const char *n);
// PS5 system paths. fs_guard_entry: may p itself be deleted, moved, renamed or chmodded?
// fs_guard_write: may something be created or changed inside dir? Both: 0 or -FS_ESYSTEM
int fs_guard_entry(const char *p);
int fs_guard_write(const char *dir);
int fs_is_system(const char *p); // 1 if inside a locked system tree (shown as a lock in the UIs)
void fs_system_msg(char *o, size_t n); // "\"app.db\" is a PS5 system file..." for the last refusal

// listing: JSON {"path":..,"items":[{"n":..,"d":0|1,"l":0|1,"s":size,"m":mtime}]} or
// TSV lines "d\tsize\tmtime\tlocked\tname" (tab, newline and backslash in names escaped)
// dirs first, then by name ignoring case. Returns malloc'd text, 0 on error (errno set).
char *fs_list(const char *path, int hidden, int tsv);
// storage places that exist on this console, JSON array or TSV "path\tlabel\tfree\ttotal"
char *fs_places(int tsv);
// one entry: type, size, mode, owner, created / modified / accessed times, link target, system flag (JSON)
char *fs_info(const char *p);

int fs_mkdir(const char *p);                 // 0 or -errno
int fs_newfile(const char *p);               // creates an empty file, fails if it exists
int fs_rename(const char *p, const char *name, char *out, size_t on); // in the same folder
int fs_chmod(const char *p, int mode);       // just this entry; recursive is a job
// writes data to p through a temporary file in the same folder, then renames it over p
int fs_save(const char *p, const void *data, size_t n);
// picks "name", "name (2).ext", "name (3).ext", ... in dir that doesn't exist yet
int fs_unique(const char *dir, const char *name, char *out, size_t on);
int fs_mkdirs(const char *p);                // mkdir -p, every new folder 0777
int fs_create(const char *p, int excl);      // open for writing, 0777 whatever the umask; fd or -errno

// background job (one at a time): paths is a newline separated list.
// dest: target folder (copy, move), new archive path (zip, 7z), folder to extract into;
// arg: the mode for chmod, the compression (TF_*) for tar. The final path (archive, extract folder) or SHA-256 is the job's result.
int fs_job_start(int op, const char *paths, const char *dest, int arg); // 0, -EBUSY or -errno
int fs_job_start_pw(int op, const char *paths, const char *dest, int arg, const char *pw); // with an archive password (zip, 7z, extract)
void fs_job_cancel(void);
void fs_job_cancel_all(void); // the job and everything queued after it
// Undo: the last rename or move, put back. fs_undo_info: a description (0 if there's nothing to undo);
// fs_undo: 0 done, 1 a job is moving things back from another drive, <0 an error
int fs_undo_info(char *o, size_t n);
int fs_undo(void);
void fs_job_json(char *o, size_t n);
int fs_job_running(void);

// for archive.c: progress, cancel and totals of the running job
int job_tick(long long bytes, long long items, const char *cur); // -ECANCELED once cancelled
void job_add_total(long long bytes, long long items);
void job_finishing(void); // all input read: the compressor is still writing (the progress bar says so)
void job_fail(const char *what, const char *p, int rc);
void job_fail_text(const char *what, const char *p, const char *why); // a reason in words
int job_cancelled(void);
const char *job_password(void); // "" when none

const char *fs_err(int rc); // message for a -errno / -FS_E* result
void fs_json_str(char *o, size_t n, const char *s); // JSON string with quotes, escaped
