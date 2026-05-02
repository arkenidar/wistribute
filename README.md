# wistribute

A single self-contained Win64 `.exe` that downloads
[ZeroBraneStudio](https://github.com/pkulchenko/ZeroBraneStudio)'s
`master.zip` and extracts it locally. Optional Win32 GUI with progress
bar and folder picker. No DLLs, no installer, no sidecar files.

Two build modes:

| Exe                       | Mode      | What it does                                         |
| ------------------------- | --------- | ---------------------------------------------------- |
| `wistribute.exe`          | DOWNLOAD  | Fetches the zip at runtime via WinHTTP, then unzips. |
| `wistribute-offline.exe`  | EMBEDDED  | Zip is baked into the exe via C23 `#embed`. Offline. |

## Stack
- **C23** (mingw-w64 GCC ≥ 15 — needed for `#embed`)
- **WinHTTP** for HTTPS (TLS via Schannel) — no libcurl, no OpenSSL
- **miniz** (vendored at build time) for ZIP extraction
- **Win32** for the GUI (`msctls_progress32` + `IFileOpenDialog`)

## Build (CMake — recommended)

```sh
cmake -S . -B build -G Ninja
cmake --build build
```

Outputs end up in `build/wistribute.exe` and `build/wistribute-offline.exe`.

To skip one mode:

```sh
cmake -S . -B build -DWISTRIBUTE_BUILD_EMBEDDED=OFF
```

To override the embedded URL:

```sh
cmake -S . -B build -DZBS_URL=https://example.com/your.zip
```

## Build (plain GNU make)

Mirrors the CMake build for convenience (msys2 / mingw shell):

```sh
make            # builds both
make download   # only wistribute.exe
make embedded   # only wistribute-offline.exe
```

## Usage

```text
wistribute.exe [--cli] [--gui] [--out DIR] [--url URL]
```

- Double-click → GUI (default when no console parent).
- From `cmd.exe` / PowerShell → CLI by default; pass `--gui` to force the window.
- `--out DIR` chooses the extraction directory (default: cwd).
- `--url URL` overrides the download URL (DOWNLOAD build only).

## Verify it's truly self-contained

```sh
objdump -p build/wistribute.exe | grep -i "DLL Name"
```

Should list only Windows system DLLs:
`KERNEL32.dll, USER32.dll, GDI32.dll, COMCTL32.dll, SHELL32.dll, ole32.dll,
WINHTTP.dll` (and `winhttp` is omitted from the offline build).

## Layout
```
src/         C sources (main, http, unzip, gui, embed) + headers
res/         Win32 manifest + .rc
payload/     Embedded-mode zip (downloaded by Makefile; CMake stages its own copy)
docs/        Static HTML documentation (open docs/index.html)
docs/plans/  Preserved planning history (markdown)
```

## Documentation
Open [docs/index.html](docs/index.html) in a browser for the full docs:
overview, build, usage, architecture, and the plans archive.
