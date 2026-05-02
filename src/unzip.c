/*
 * unzip.c — extract an in-memory ZIP to disk via miniz.
 *
 * Threading: synchronous; safe to call from any thread. Holds no globals.
 *
 * Filename safety: zip entries are user-controlled bytes. path_is_safe()
 * runs before any filesystem call and rejects absolute paths, drive
 * letters, and ".." components — closing the classic Zip-Slip vector.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#include <stdlib.h>
#include <string.h>
#include "unzip.h"
#include "miniz.h"

static wchar_t *utf8_to_wide(const char *s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 0) return NULL;
    wchar_t *w = (wchar_t *)malloc((size_t)n * sizeof(wchar_t));
    if (!w) return NULL;
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}

/* Reject empty, absolute, or traversal paths. */
static int path_is_safe(const char *p) {
    if (!p || !*p) return 0;
    if (p[0] == '/' || p[0] == '\\') return 0;
    if (p[1] == ':') return 0;
    for (const char *q = p; *q; ++q) {
        if ((q == p || q[-1] == '/' || q[-1] == '\\') &&
            q[0] == '.' && q[1] == '.' &&
            (q[2] == 0 || q[2] == '/' || q[2] == '\\'))
            return 0;
    }
    return 1;
}

static void normalize_slashes(wchar_t *p) {
    for (; *p; ++p) if (*p == L'/') *p = L'\\';
}

static int ensure_parent_dir(const wchar_t *full_path) {
    wchar_t *copy = _wcsdup(full_path);
    if (!copy) return 0;
    wchar_t *slash = wcsrchr(copy, L'\\');
    if (slash) {
        *slash = 0;
        SHCreateDirectoryExW(NULL, copy, NULL);
    }
    free(copy);
    return 1;
}

int unzip_buffer(const void *zip, size_t zip_len, const wchar_t *out_dir,
                 progress_fn on_progress, void *user) {
    mz_zip_archive za;
    memset(&za, 0, sizeof za);
    if (!mz_zip_reader_init_mem(&za, zip, zip_len, 0)) return -1;

    int rc = 0;
    SHCreateDirectoryExW(NULL, out_dir, NULL);

    mz_uint n = mz_zip_reader_get_num_files(&za);
    for (mz_uint i = 0; i < n; ++i) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&za, i, &st)) { rc = -2; break; }
        if (!path_is_safe(st.m_filename))         { rc = -3; break; }

        wchar_t *rel = utf8_to_wide(st.m_filename);
        if (!rel) { rc = -4; break; }
        normalize_slashes(rel);

        size_t need = wcslen(out_dir) + 1 + wcslen(rel) + 1;
        wchar_t *full = (wchar_t *)malloc(need * sizeof(wchar_t));
        if (!full) { free(rel); rc = -4; break; }
        _snwprintf(full, need, L"%ls\\%ls", out_dir, rel);

        if (mz_zip_reader_is_file_a_directory(&za, i)) {
            SHCreateDirectoryExW(NULL, full, NULL);
        } else {
            ensure_parent_dir(full);
            HANDLE h = CreateFileW(full, GENERIC_WRITE, 0, NULL,
                                   CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            if (h == INVALID_HANDLE_VALUE) { free(rel); free(full); rc = -5; break; }

            size_t out_size = 0;
            void *data = mz_zip_reader_extract_to_heap(&za, i, &out_size, 0);
            if (!data) { CloseHandle(h); free(rel); free(full); rc = -6; break; }
            DWORD wrote = 0;
            BOOL ok = WriteFile(h, data, (DWORD)out_size, &wrote, NULL);
            mz_free(data);
            CloseHandle(h);
            if (!ok || wrote != out_size) { free(rel); free(full); rc = -7; break; }
        }
        free(rel); free(full);
        if (on_progress) on_progress((uint64_t)(i + 1), (uint64_t)n, user);
    }

    mz_zip_reader_end(&za);
    return rc;
}
