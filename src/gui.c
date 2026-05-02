/*
 * gui.c — Win32 GUI: progress bar, folder picker, worker thread.
 *
 * Threading model
 * ---------------
 * Two threads only:
 *
 *   UI thread     — owns every HWND. Created controls, dispatches
 *                   messages, reads/writes window text. Never blocks on
 *                   I/O.
 *   Worker thread — spawned per "Start" click. Runs http_get (in
 *                   DOWNLOAD builds) followed by unzip_buffer. Owns the
 *                   downloaded buffer and frees it before returning.
 *
 * The worker NEVER touches HWNDs directly. It communicates back to the
 * UI via PostMessageW with the WM_APP_* messages defined below; this is
 * thread-safe by design (PostMessageW marshals to the target's queue).
 *
 * Run-state machine
 * -----------------
 * STATE_IDLE   → user can click Start
 * STATE_RUNNING → buttons disabled while worker runs
 * STATE_DONE_OK → Start button is repurposed as "Close"
 * STATE_DONE_ERR → buttons re-enabled for retry
 *
 * MODE_DOWNLOAD vs MODE_EMBEDDED is selected at compile time and changes
 * only the source of the zip bytes (network vs #embed'd array).
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <objbase.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gui.h"
#include "unzip.h"

#ifdef MODE_DOWNLOAD
#include "http.h"
#endif

#ifdef MODE_EMBEDDED
const unsigned char *zbs_zip_data(void);
size_t                zbs_zip_size(void);
#endif

#define ID_URL      1001
#define ID_OUT      1002
#define ID_BROWSE   1003
#define ID_START    1004
#define ID_STATUS   1005
#define ID_PROGRESS 1006

/* Worker → UI messages. wParam/lParam conventions:
 *   WM_APP_PROGRESS  wParam = percent (0..100), lParam unused
 *   WM_APP_STATUS    lParam = strdup'd wchar_t* status (UI frees it)
 *   WM_APP_DONE      wParam = return code (0 on success) */
#define WM_APP_PROGRESS (WM_APP + 1)
#define WM_APP_STATUS   (WM_APP + 2)
#define WM_APP_DONE     (WM_APP + 3)

typedef struct {
    HWND hwnd;
    wchar_t url[2048];
    wchar_t out_dir[MAX_PATH];
} worker_args;

static void post_status(HWND h, const wchar_t *s) {
    PostMessageW(h, WM_APP_STATUS, 0, (LPARAM)_wcsdup(s));
}
static void post_progress(HWND h, int pct) {
    PostMessageW(h, WM_APP_PROGRESS, (WPARAM)pct, 0);
}

#ifdef MODE_DOWNLOAD
static void dl_progress(uint64_t cur, uint64_t total, void *u) {
    HWND h = (HWND)u;
    int pct = total ? (int)((cur * 100) / total) : 0;
    post_progress(h, pct);
}
#endif

static void unzip_progress(uint64_t cur, uint64_t total, void *u) {
    HWND h = (HWND)u;
    int pct = total ? (int)((cur * 100) / total) : 0;
    post_progress(h, pct);
}

/* Background pipeline: download (if DOWNLOAD build) → extract.
 * Runs on its own CreateThread() context; the UI thread stays responsive.
 * Talks back only via PostMessageW — no shared mutable state. */
static DWORD WINAPI worker_thread(LPVOID p) {
    worker_args *a = (worker_args *)p;
    HWND h = a->hwnd;
    int rc = 0;

    void *zip = NULL;
    size_t zip_len = 0;
    int free_zip = 0;

#ifdef MODE_DOWNLOAD
    post_status(h, L"Downloading...");
    post_progress(h, 0);
    rc = http_get(a->url, &zip, &zip_len, dl_progress, h);
    if (rc != 0) { post_status(h, L"Download failed."); goto end; }
    free_zip = 1;
#else
    (void)a->url;
    zip = (void *)zbs_zip_data();
    zip_len = zbs_zip_size();
#endif

    post_status(h, L"Extracting...");
    post_progress(h, 0);
    rc = unzip_buffer(zip, zip_len, a->out_dir, unzip_progress, h);
    if (rc != 0) { post_status(h, L"Extraction failed."); goto end; }

    post_status(h, L"Done.");
    post_progress(h, 100);

end:
    if (free_zip) free(zip);
    PostMessageW(h, WM_APP_DONE, (WPARAM)rc, 0);
    free(a);
    return 0;
}

/* Modern (Vista+) folder picker. Preferred over SHBrowseForFolderW for
 * resizing, breadcrumb navigation, and the address bar. The COM apartment
 * is initialised once per process in gui_run(). */
static void browse_for_folder(HWND owner, HWND edit) {
    IFileOpenDialog *dlg = NULL;
    if (FAILED(CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER,
                                &IID_IFileOpenDialog, (void **)&dlg))) return;
    DWORD opts = 0;
    dlg->lpVtbl->GetOptions(dlg, &opts);
    dlg->lpVtbl->SetOptions(dlg, opts | FOS_PICKFOLDERS | FOS_PATHMUSTEXIST | FOS_FORCEFILESYSTEM);
    if (SUCCEEDED(dlg->lpVtbl->Show(dlg, owner))) {
        IShellItem *it = NULL;
        if (SUCCEEDED(dlg->lpVtbl->GetResult(dlg, &it))) {
            PWSTR p = NULL;
            if (SUCCEEDED(it->lpVtbl->GetDisplayName(it, SIGDN_FILESYSPATH, &p))) {
                SetWindowTextW(edit, p);
                CoTaskMemFree(p);
            }
            it->lpVtbl->Release(it);
        }
    }
    dlg->lpVtbl->Release(dlg);
}

enum run_state { STATE_IDLE, STATE_RUNNING, STATE_DONE_OK, STATE_DONE_ERR };

static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    static HWND ed_url, ed_out, btn_browse, btn_start, lbl_status, prog;
    static enum run_state state = STATE_IDLE;
    switch (msg) {
    case WM_CREATE: {
        HFONT f = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        CreateWindowW(L"STATIC", L"URL:", WS_CHILD|WS_VISIBLE,
                      12, 14, 60, 20, h, NULL, NULL, NULL);
        ed_url = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                      WS_CHILD|WS_VISIBLE|ES_AUTOHSCROLL,
                      78, 12, 480, 22, h, (HMENU)(INT_PTR)ID_URL, NULL, NULL);
        CreateWindowW(L"STATIC", L"Extract to:", WS_CHILD|WS_VISIBLE,
                      12, 46, 70, 20, h, NULL, NULL, NULL);
        ed_out = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                      WS_CHILD|WS_VISIBLE|ES_AUTOHSCROLL,
                      78, 44, 400, 22, h, (HMENU)(INT_PTR)ID_OUT, NULL, NULL);
        btn_browse = CreateWindowW(L"BUTTON", L"Browse...",
                      WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
                      484, 43, 74, 24, h, (HMENU)(INT_PTR)ID_BROWSE, NULL, NULL);
        prog = CreateWindowW(PROGRESS_CLASSW, NULL,
                      WS_CHILD|WS_VISIBLE,
                      12, 78, 546, 18, h, (HMENU)(INT_PTR)ID_PROGRESS, NULL, NULL);
        SendMessageW(prog, PBM_SETRANGE32, 0, 100);
        lbl_status = CreateWindowW(L"STATIC", L"Ready.", WS_CHILD|WS_VISIBLE,
                      12, 102, 460, 20, h, (HMENU)(INT_PTR)ID_STATUS, NULL, NULL);
        btn_start = CreateWindowW(L"BUTTON", L"Start",
                      WS_CHILD|WS_VISIBLE|BS_DEFPUSHBUTTON,
                      484, 100, 74, 26, h, (HMENU)(INT_PTR)ID_START, NULL, NULL);

        HWND kids[] = { ed_url, ed_out, btn_browse, btn_start, lbl_status };
        for (size_t i = 0; i < sizeof kids / sizeof kids[0]; ++i)
            SendMessageW(kids[i], WM_SETFONT, (WPARAM)f, TRUE);

#ifdef MODE_EMBEDDED
        EnableWindow(ed_url, FALSE);
        SetWindowTextW(ed_url, L"(embedded payload)");
#endif
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(w) == ID_BROWSE) {
            browse_for_folder(h, ed_out);
        } else if (LOWORD(w) == ID_START) {
            if (state == STATE_DONE_OK) {
                PostMessageW(h, WM_CLOSE, 0, 0);
                return 0;
            }
            worker_args *a = (worker_args *)calloc(1, sizeof *a);
            if (!a) return 0;
            a->hwnd = h;
            GetWindowTextW(ed_url, a->url, (int)(sizeof a->url / sizeof a->url[0]));
            GetWindowTextW(ed_out, a->out_dir, (int)(sizeof a->out_dir / sizeof a->out_dir[0]));
            if (!a->out_dir[0]) {
                MessageBoxW(h, L"Please choose an output directory.", L"wistribute", MB_OK|MB_ICONWARNING);
                free(a); return 0;
            }
            state = STATE_RUNNING;
            EnableWindow(btn_start, FALSE);
            EnableWindow(btn_browse, FALSE);
            HANDLE t = CreateThread(NULL, 0, worker_thread, a, 0, NULL);
            if (t) CloseHandle(t);
        }
        return 0;
    case WM_APP_STATUS: {
        wchar_t *s = (wchar_t *)l;
        SetWindowTextW(lbl_status, s);
        free(s);
        return 0;
    }
    case WM_APP_PROGRESS:
        SendMessageW(prog, PBM_SETPOS, w, 0);
        return 0;
    case WM_APP_DONE:
        /* Worker has exited. On success we repurpose the Start button as
         * a Close button (and route the click through WM_CLOSE in the
         * ID_START handler above) so the user has an unambiguous "you
         * can leave now" signal without a modal popup. */
        if ((int)w == 0) {
            state = STATE_DONE_OK;
            SetWindowTextW(lbl_status, L"Done — you can close this window.");
            SetWindowTextW(btn_start, L"Close");
            EnableWindow(btn_start, TRUE);
            EnableWindow(btn_browse, FALSE);
            SetFocus(btn_start);
        } else {
            state = STATE_DONE_ERR;
            SetWindowTextW(btn_start, L"Start");
            EnableWindow(btn_start, TRUE);
            EnableWindow(btn_browse, TRUE);
        }
        return 0;
    case WM_CLOSE:
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, w, l);
}

int gui_run(const wchar_t *initial_url, const wchar_t *initial_out_dir) {
    INITCOMMONCONTROLSEX icc = { .dwSize = sizeof icc, .dwICC = ICC_PROGRESS_CLASS|ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    HINSTANCE hi = GetModuleHandleW(NULL);
    WNDCLASSEXW wc = {
        .cbSize = sizeof wc,
        .lpfnWndProc = wndproc,
        .hInstance = hi,
        .hCursor = LoadCursor(NULL, IDC_ARROW),
        .hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1),
        .lpszClassName = L"WistributeMain",
    };
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(0, L"WistributeMain",
                                L"wistribute - ZeroBraneStudio",
                                WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX,
                                CW_USEDEFAULT, CW_USEDEFAULT, 590, 170,
                                NULL, NULL, hi, NULL);
    if (!hwnd) return 1;

    if (initial_url)     SetWindowTextW(GetDlgItem(hwnd, ID_URL), initial_url);
    if (initial_out_dir) SetWindowTextW(GetDlgItem(hwnd, ID_OUT), initial_out_dir);

    ShowWindow(hwnd, SW_SHOWNORMAL);
    UpdateWindow(hwnd);

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    CoUninitialize();
    return (int)m.wParam;
}
