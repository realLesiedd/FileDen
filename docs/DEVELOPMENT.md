# FileDen: notes for developers

## Protected system paths

FileDen never changes anything inside these folders: no delete, move, rename, permission change, new
file, save, upload, extract or paste. Copying files *out* of them still works.

| Path | What it holds |
|---|---|
| `/system`, `/system_ex`, `/preinst`, `/preinst2`, `/update` | Firmware, system apps, the update image |
| `/system_data` | The home screen's app databases (`priv/mms/app.db`, `appinfo.db`, `addcontent.db`, `av_content_bg.db`) and the registry (`settings/system.*`) |
| `/user/home`, `/user/license`, `/user/settings`, `/user/system`, `/user/priv`, `/user/sshared` | Accounts, saves, licences, the account registry |
| `/dev` | Device nodes, including the flash (`sflash0`) |
| `/system_tmp`, `/adm`, `/eap_user`, `/eap_vsh`, `/host`, `/hostapp`, `/mnt/sandbox`, `/mnt/disc`, `/mnt/proc` | Other system mounts and running apps' sandboxes |

Folders that stay put but whose contents you manage:
`/`, `/data`, `/user`, `/mnt`, `/mnt/usb0`–`7`, `/mnt/ext0`, `/mnt/ext1`, `/user/app`, `/user/patch`,
`/user/addcont`, `/user/appmeta`, `/user/av_contents`, `/user/download`, `/user/trophy`, `/user/data` and
`/data/fileden` can't be deleted, moved or renamed themselves. Nothing new can be created directly in
`/`, `/mnt` or `/user`.

Sources: [PS5 DB backup payload](https://github.com/jeroendev-one/ps5-db-backup),
[PS5-TabShifter](https://github.com/xvvulcanvx/PS5-TabShifter),
[ps5-pkg-manager](https://github.com/itsPLK/ps5-pkg-manager/blob/main/ARCHITECTURE.md),
[PS4 registry write-up](https://oct0xor.github.io/2018/02/07/ps4_registry/),
[ConsoleMods PS4 file list](https://consolemods.org/wiki/PS4:Files_and_Directories) (shared layout).

## Build details

Run `./embed.sh` after editing `web/index.html` or `assets/`. `tools/fetch-deps.sh` fetches xz, zstd, bzip2,
zlib, OpenSSL and libwebp (PacBrew ports).

`app/fself/build.sh` fetches ProsperoStore's native app tooling (pinned commit) into `$WORK`
(default `/tmp/fileden-fself`) and writes the release zip with every entry stored as 0777.
`tools/check-app-imports.sh` then lists any function the app calls that the PS5 app runtime lacks.
GitHub Actions does all of this on every push and attaches the zip to each published release.

## Run and test on a PC

```sh
LIBS="third_party/miniz/miniz.c $(ls third_party/lzma/*.c | tr '\n' ' ')"
DEFS="-DZ7_ST -DZ7_PPMD_SUPPORT -Ithird_party/lzma"
LA="$(tools/host-libarchive.sh)"                                                              # vendored libarchive (needs libssl-dev, libzstd-dev, liblzma-dev, libbz2-dev)

cc -DHOST $DEFS -O2 main.c fsops.c archive.c rarcrypt.c title.c image.c $LIBS $LA -lm -pthread -o fileden-host && ./fileden-host  # :9923
cc -DHOST_PREVIEW -O2 app/app.c -lm -o preview                                                # TV app (needs fileden-host)
FD_SCRIPT="wait:20;snap:browse;down;x;wait:10;snap:folder" ./preview                           # writes browse.png, folder.png
cc $DEFS tests/test_fsops.c fsops.c archive.c rarcrypt.c title.c $LIBS $LA -pthread -o test_fsops && ./test_fsops
cc $DEFS tests/test_archives.c fsops.c archive.c rarcrypt.c title.c $LIBS $LA -pthread -o test_archives && ./test_archives tests/fixtures
```

## API

Paths are absolute. POSTs are form-encoded and need `X-FileDen: 1`. FileDen answers only to its IP address,
`localhost` or a local name (one word, `.local`, `.lan`, `.home`, `.internal`), so other websites can't reach it
through their own domain names. Archive passwords go with the request (`pw`) and are dropped when the job ends.

| Call | What it does |
|---|---|
| `GET /api/ls?path=` · `/api/places` · `/api/info?path=` | List a folder, the drives, one item's details |
| `GET /api/file?path=[&dl=1]` | Read or download a file (Range supported) |
| `GET /api/job` · `/api/settings` | Progress and result of the background job, settings |
| `POST /api/mkdir` · `newfile` · `rename` | Create and rename |
| `GET /api/undo` · `POST /api/undo` | What Undo would put back (the last rename or move), and do it |
| `POST /api/copy` · `move` · `delete` · `cancel` | Background file jobs |
| `POST /api/zip` · `7z` · `tar` (paths, dest, name, pw; tar: z = gz, xz, bz2, zst or empty) · `extract` (path, dest, pw) | Archives; `pw` is an optional password (ZIP / 7z AES-256). A locked archive fails with `need_pw` 1 in `/api/job` (2: wrong password) |
| `GET /api/image?path=&w=&h=` | Any picture FileDen reads as a JPEG that fits w × h (`X-Width` / `X-Height`: its own size) |
| `GET /api/favs` · `POST /api/fav` (path, on) | Favourites |
| `POST /api/hash` · `size` (path) · `chmod` (path, mode, recursive) | SHA-256, folder size, permissions |
| `POST /api/upload?path=` · `save?path=` | Raw file body: upload, editor save |
| `POST /api/settings` · `quit` (force=1 stops it with the web server on) | Change settings (web, hidden, confirm, theme, grid, sound, intro), stop the payload |

One background job runs at a time. Settings and favourites live in `/data/fileden`, and start-up problems of the
app go to `/data/fileden/app.log`.

## Title ID and port

Title ID `PPSA99923`, port `9923`: neither clashes with known homebrew or payloads (checked 3 Nov 2026).
The app's title ID, name and version are in [`sce_sys/param.json`](../sce_sys/param.json).

## Releasing

Every push builds the app zip, `fileden.elf` and `SHA256SUMS.txt`; they are on the run's page under **Artifacts**.
Set the version in `version.h` and raise `contentVersion` in `sce_sys/param.json` (ProsperoStore reads it at the
release tag to offer updates), then draft a release on GitHub with tag `v<version>` and attach those three files.
Keep the zip's name the same between releases.
