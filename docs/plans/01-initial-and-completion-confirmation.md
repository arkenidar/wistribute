# Plan — Self-contained Win64 ZBS fetcher/extractor

## Context
Build a single Win64 `.exe` (mingw-w64 GCC, C23) that downloads
`https://github.com/pkulchenko/ZeroBraneStudio/archive/refs/heads/master.zip`
and extracts it locally — no DLLs, no installer, no sidecar files. The exe
must support both a headless CLI and an optional Win32 GUI (progress bar +
extract-path picker). A second build mode embeds the zip directly into the
exe via C23 `#embed` for fully offline use.

Goal: smallest possible, truly self-contained binary.

## Decisions (confirmed)
- **HTTP**: WinHTTP (`winhttp.dll`, ships with Windows) — no libcurl.
- **TLS**: Schannel via WinHTTP (transparent).
- **Unzip**: `miniz` (single-file, public domain) — statically compiled in.
- **Payload modes** (build-time switch): `DOWNLOAD` and `EMBEDDED` (both built).
- **UI**: One exe with both CLI and GUI. GUI shown by default on
  double-click; `--cli` forces headless. Built as `-mwindows` subsystem,
  with manual `AttachConsole(ATTACH_PARENT_PROCESS)` so CLI still prints.
- **Compiler**: mingw-w64 GCC ≥ 15 (for C23 `#embed`). `-std=c23`.

## Project layout
```
wistribute/
├── src/
│   ├── main.c          # entry, arg parse, mode dispatch
│   ├── http.c/.h       # WinHTTP download (with progress callback)
│   ├── unzip.c/.h      # miniz wrapper: extract zip buffer/file to dir
│   ├── gui.c/.h        # Win32 GUI: progress bar + folder picker
│   ├── miniz.c/.h      # vendored single-file lib
│   └── embed.c         # contains: static const unsigned char ZBS_ZIP[] = {
│                       #   #embed "../payload/zbs.zip"
│                       # };  (only compiled in EMBEDDED build)
├── payload/zbs.zip     # fetched at build time for EMBEDDED mode
├── Makefile
└── README.md
```

## Build
Two targets in the Makefile:

- `wistribute.exe` (DOWNLOAD mode, default)
  ```
  gcc -std=c23 -O2 -s -municode -mwindows \
      -DMODE_DOWNLOAD \
      src/main.c src/http.c src/unzip.c src/gui.c src/miniz.c \
      -o wistribute.exe \
      -lwinhttp -lcomctl32 -lshell32 -lole32 -luser32 -lgdi32
  ```
- `wistribute-offline.exe` (EMBEDDED mode)
  ```
  curl -L -o payload/zbs.zip <ZBS master.zip URL>
  gcc -std=c23 -O2 -s -municode -mwindows \
      -DMODE_EMBEDDED \
      src/main.c src/embed.c src/unzip.c src/gui.c src/miniz.c \
      -o wistribute-offline.exe \
      -lcomctl32 -lshell32 -lole32 -luser32 -lgdi32
  ```

Both link only against Windows system DLLs → no redistributables.
`-s` strips; expected sizes: download build ~120-180 KB, embedded build
~120 KB + zip size.

## Implementation notes — critical files

### `src/main.c`
- `wmain` (UNICODE) — parse args:
  - `--cli` → force CLI even if no console attached
  - `--out <dir>` → extraction destination (default: cwd)
  - `--url <url>` → override download URL (DOWNLOAD only)
  - `-h/--help`
- Decide UI: if `--cli` or stdout is redirected → CLI path; else GUI path.
- Call `AttachConsole(ATTACH_PARENT_PROCESS)` early so prints reach the
  parent shell when launched from cmd/powershell.

### `src/http.c` (DOWNLOAD only)
Single function:
```c
int http_get(const wchar_t *url,
             void **out_buf, size_t *out_len,
             void (*on_progress)(uint64_t cur, uint64_t total, void *u),
             void *u);
```
- `WinHttpOpen` → `WinHttpCrackUrl` → `WinHttpConnect` →
  `WinHttpOpenRequest(... WINHTTP_FLAG_SECURE)` → `WinHttpSendRequest` →
  `WinHttpReceiveResponse`.
- Read `Content-Length` header (may be absent for chunked); grow a
  `realloc`'d buffer in 64 KB chunks via `WinHttpReadData`. Invoke
  `on_progress` after each chunk.
- Follow redirects automatically (WinHTTP default — GitHub redirects
  master.zip to codeload.github.com).

### `src/unzip.c` (miniz)
```c
int unzip_buffer(const void *zip, size_t zip_len, const wchar_t *out_dir,
                 void (*on_progress)(uint32_t cur, uint32_t total, void *u),
                 void *u);
```
- `mz_zip_reader_init_mem`, iterate entries with
  `mz_zip_reader_get_num_files` / `mz_zip_reader_file_stat`.
- For each entry: build full UTF-16 path under `out_dir`, create
  intermediate dirs (`SHCreateDirectoryExW`), then either skip (dir) or
  `mz_zip_reader_extract_to_callback` writing via `CreateFileW` /
  `WriteFile`. Path-traversal guard: reject entries containing `..` or
  absolute paths.

### `src/gui.c`
- `InitCommonControlsEx` with `ICC_PROGRESS_CLASS`.
- Main dialog created with `CreateWindowExW` (no resource file — keeps
  build single-command). Children: status `STATIC`, `msctls_progress32`,
  "Browse…" button, "Start" button, path `EDIT`.
- Browse → `SHBrowseForFolderW` (or `IFileDialog` via `CoCreateInstance`
  for the modern picker — pick `SHBrowseForFolderW` for fewer COM deps).
- Start → spawn worker thread (`CreateThread`) running download → unzip;
  posts `WM_APP+1`/`WM_APP+2` progress messages back to the UI thread.

### `src/embed.c` (EMBEDDED only)
Uses C23 `#embed`:
```c
static const unsigned char ZBS_ZIP[] = {
    #embed "../payload/zbs.zip"
};
const unsigned char *zbs_zip_data(void) { return ZBS_ZIP; }
size_t zbs_zip_size(void) { return sizeof ZBS_ZIP; }
```

### Manifest (linked via `windres`)
Embed an application manifest enabling:
- `requestedExecutionLevel level="asInvoker"` (no UAC prompt)
- common-controls v6 (themed progress bar)
- UTF-8 active code page (so narrow APIs handle filenames correctly)

## Addendum — completion-confirmation in GUI

**Why**: After a successful download+extract the GUI currently shows
"Done." and idles with a disabled Start button — the user has no clear
signal they can close, and the Browse button stays disabled.

**Approach** (selected by user): repurpose the Start button as a Close
button on success, no modal popups.

Changes confined to `src/gui.c`:

1. Track run state in the window via a static enum:
   `STATE_IDLE`, `STATE_RUNNING`, `STATE_DONE_OK`, `STATE_DONE_ERR`.
2. In `WM_APP_DONE` (received with `wParam = rc`):
   - On `rc == 0`:
     - status text → `"Done — you can close this window."`
     - relabel `btn_start` to `"Close"` via `SetWindowTextW`
     - keep `btn_browse` disabled (no point browsing now)
     - leave `btn_start` enabled; clicking it sends `WM_CLOSE`
   - On `rc != 0`:
     - keep `"Failed."` status
     - re-enable `btn_browse` and `btn_start` (label restored to `"Start"`)
3. In `WM_COMMAND` for `ID_START`, branch on state: if `STATE_DONE_OK`,
   `PostMessageW(h, WM_CLOSE, 0, 0)`; else run the existing start path
   and set state to `STATE_RUNNING`.
4. Reset to `STATE_IDLE` when the user edits the path or starts again
   after an error (covered by the existing re-enable path).

No new threads, messages, or controls. Roughly 20 lines.

## Verification
1. **Build**:
   `make` produces both exes; `dumpbin /dependents` (or `objdump -p`)
   shows imports only from `kernel32, user32, gdi32, comctl32, shell32,
   ole32, winhttp` — no third-party DLLs.
2. **CLI download**:
   `wistribute.exe --cli --out C:\tmp\zbs` → exits 0, `C:\tmp\zbs\
   ZeroBraneStudio-master\zbstudio.exe` exists.
3. **GUI**: double-click → window appears, Browse picks dir, Start shows
   progressing bar for download then extraction, completes without error.
4. **Offline build**: disconnect network, run `wistribute-offline.exe
   --cli --out C:\tmp\zbs2` → succeeds.
5. **Path-traversal test**: craft a zip with `../evil.txt`, confirm
   extractor rejects it.
6. **Size**: `ls -l` — download exe < 200 KB, offline exe ≈ 120 KB +
   payload size.
