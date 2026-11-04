# Changelog

## 0.0.1

First release.

**Files**
- Browse every drive with a controller, or from a phone or PC at `http://<PS5-IP>:9923/`
- Favourites tab and Favourites row, shared by the app and the web UI
- Copy, cut, paste, Copy to / Move to, rename, delete, new folder, new text file, multi-select (hold △ to select everything in a folder)
- Tabs only for the drives that are plugged in
- Open any file as text from the menu; files over 4 MB (8 MB in the browser) get a clear message instead
- Big jobs run in the background service with progress, speed, time left and Cancel; Minimise keeps them going
  while you browse, rename, view and edit, with a small progress card beside the status
- A job queue: start more while one runs; the progress card shows a write-speed graph, time left and run time;
  the Touchpad brings a minimised job back
- Undo for the last rename or move (TV menu, web UI button or Ctrl+Z)
- PS5 system files and folders locked against changes, with an error naming the item

**Details and permissions**
- Info: size, SHA-256, created / modified / last opened, owner, link target
- Permissions: read / write / run for owner, group and everyone, set user ID / set group ID / sticky,
  or any mode typed as a number from 0000 to 7777; folders can apply it to everything inside

**Archives**
- Extract ZIP, ZIPX, 7z, RAR and RAR5 (multi-part sets too), TAR and its gz / bz2 / xz / zst / lzma / Z forms,
  CAB, LHA, CPIO, ISO, AR / DEB, split `.001` sets and single compressed files
- Compress to ZIP, 7z, TAR, TAR.GZ, TAR.XZ, TAR.BZ2 or TAR.ZST
- Passwords: ZIP and 7z with AES-256 (7z hides the names too); extract ZIP, 7z, RAR5 and RAR 3.x / 4.x
  archives with a password, with a prompt that says when it's wrong

**View and edit**
- Image viewer with full screen and details: JPEG, PNG, GIF, BMP, TGA, PSD, HDR, WebP
  and PS5 HDR `.jxr` screenshots
- Grid view with picture previews (L3), sort by name, size, date or type (R3), Refresh in the menu
- Game, app and package icons and names in lists and grids (sce_sys/icon0.png, param.json / param.sfo, .pkg),
  installed apps too (/user/app and /user/appmeta); icons are kept, so a folder opened again shows them at once
- Text editor in the app and in the browser

**App and settings**
- The app never freezes waiting for the background service: requests and picture previews run on their own
  threads, with a small "Working" sign when one takes a moment
- A controls card on first start (gone once you press X, back any time from Settings › About and controls)
- Web server switch, menu sounds (on / off), show hidden files, ask before deleting
- Five themes (Montane Mint, Classic, Slate, Ember, Ocean), shared by the app and the web UI
- Everything FileDen writes, and every file in the release zip, is 0777

**Safety**
- Archives can't write outside their folder ("../" and absolute names stay inside, links are never made)
- The web server answers only to the console's IP address or a local name (no DNS rebinding), keeps the
  app's own connection open when many others connect, and drops clients that stall
- The web UI can't be put inside another site's frame, and files opened in the browser can't run scripts
- An archive password goes with its own job and is cleared when the job ends
