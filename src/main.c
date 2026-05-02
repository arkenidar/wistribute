/*
 * main.c — entry point, argv parsing, CLI/GUI dispatch.
 *
 * Subsystem is GUI (-mwindows) so a double-click never flashes a console
 * window; when the user launches us from cmd/PowerShell we re-attach to
 * the parent console via AttachConsole(ATTACH_PARENT_PROCESS) so stdio
 * still reaches the shell that started us.
 *
 * MODE_DOWNLOAD vs MODE_EMBEDDED is selected at compile time; this file
 * is shared by both builds and conditionally pulls in http.c symbols.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "common.h"
#include "unzip.h"
#include "gui.h"

#ifdef MODE_DOWNLOAD
#include "http.h"
#endif

#ifdef MODE_EMBEDDED
const unsigned char *zbs_zip_data(void);
size_t                zbs_zip_size(void);
#endif

static void cli_progress(uint64_t cur, uint64_t total, void *u) {
    const wchar_t *label = (const wchar_t *)u;
    if (total)
        fwprintf(stderr, L"\r%ls %3llu%%  (%llu / %llu)",
                 label, (unsigned long long)((cur * 100) / total),
                 (unsigned long long)cur, (unsigned long long)total);
    else
        fwprintf(stderr, L"\r%ls %llu bytes", label, (unsigned long long)cur);
    fflush(stderr);
}

static int run_cli(const wchar_t *url, const wchar_t *out_dir) {
    void *zip = NULL;
    size_t zip_len = 0;
    int free_zip = 0;
    int rc = 0;

#ifdef MODE_DOWNLOAD
    rc = http_get(url, &zip, &zip_len, cli_progress, (void *)L"download");
    fputwc(L'\n', stderr);
    if (rc != 0) {
        fwprintf(stderr, L"download failed: error %d\n", rc);
        return rc;
    }
    free_zip = 1;
#else
    (void)url;
    zip = (void *)zbs_zip_data();
    zip_len = zbs_zip_size();
    fwprintf(stderr, L"using embedded payload (%zu bytes)\n", zip_len);
#endif

    rc = unzip_buffer(zip, zip_len, out_dir, cli_progress, (void *)L"extract ");
    fputwc(L'\n', stderr);
    if (free_zip) free(zip);

    if (rc == 0) fwprintf(stderr, L"done. extracted to %ls\n", out_dir);
    else         fwprintf(stderr, L"extraction failed: %d\n", rc);
    return rc;
}

static void usage(const wchar_t *argv0) {
    fwprintf(stderr,
        L"usage: %ls [--cli] [--gui] [--out DIR] "
#ifdef MODE_DOWNLOAD
        L"[--url URL] "
#endif
        L"[-h]\n", argv0);
}

int wmain(int argc, wchar_t **argv) {
    /* A -mwindows process has no stdio attached. Re-bind to the parent's
     * console so `wistribute.exe --cli` from cmd/PowerShell prints inline
     * instead of silently dropping output. AttachConsole returns FALSE
     * (and we leave stdio detached) when launched from Explorer. */
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE *fp;
        freopen_s(&fp, "CONOUT$", "w", stdout);
        freopen_s(&fp, "CONOUT$", "w", stderr);
        freopen_s(&fp, "CONIN$",  "r", stdin);
    }

    int force_cli = 0, force_gui = 0;
    const wchar_t *url = DEFAULT_URL;
    wchar_t out_buf[MAX_PATH];
    GetCurrentDirectoryW(MAX_PATH, out_buf);
    const wchar_t *out_dir = out_buf;

    for (int i = 1; i < argc; ++i) {
        if      (!wcscmp(argv[i], L"--cli")) force_cli = 1;
        else if (!wcscmp(argv[i], L"--gui")) force_gui = 1;
        else if (!wcscmp(argv[i], L"--out") && i + 1 < argc) out_dir = argv[++i];
        else if (!wcscmp(argv[i], L"--url") && i + 1 < argc) url = argv[++i];
        else if (!wcscmp(argv[i], L"-h") || !wcscmp(argv[i], L"--help")) {
            usage(argv[0]); return 0;
        } else {
            fwprintf(stderr, L"unknown argument: %ls\n", argv[i]);
            usage(argv[0]); return 2;
        }
    }

    /* Auto-mode rule: explicit flags win; otherwise GUI iff no console.
     * GetConsoleWindow() returns NULL when launched from Explorer. */
    int has_console = GetConsoleWindow() != NULL;
    int gui = force_gui || (!force_cli && !has_console);

    if (gui) return gui_run(url, out_dir);
    return run_cli(url, out_dir);
}
