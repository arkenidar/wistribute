/*
 * unzip.h — extract a ZIP archive held in memory to a directory on disk.
 *
 * Backed by miniz (vendored at build time). Used by both the CLI and
 * the GUI worker thread.
 */
#ifndef WISTRIBUTE_UNZIP_H
#define WISTRIBUTE_UNZIP_H

#include "common.h"

/**
 * unzip_buffer — extract a zip-in-memory tree under @p out_dir.
 *
 * Walks the central directory and writes each regular entry through
 * CreateFileW so Unicode filenames round-trip cleanly. Directory entries
 * are materialised with SHCreateDirectoryExW. The destination directory
 * is created if missing.
 *
 * Path-traversal guard: any entry whose name begins with a separator,
 * contains a Windows drive letter, or includes a "..\" component causes
 * the function to abort before opening that file.
 *
 * @param zip          Pointer to the in-memory zip bytes.
 * @param zip_len      Size of @p zip in bytes.
 * @param out_dir      UTF-16 destination directory.
 * @param on_progress  Optional. cur = entries written, total = entry count.
 * @param user         Opaque pointer passed back to on_progress.
 *
 * @return 0 on success.
 *         -1 = zip is unreadable, -2 = file_stat failed,
 *         -3 = path-traversal rejected, -4 = OOM,
 *         -5 = CreateFileW failed, -6 = miniz extract failed,
 *         -7 = WriteFile short/failed.
 */
int unzip_buffer(const void *zip, size_t zip_len, const wchar_t *out_dir,
                 progress_fn on_progress, void *user);

#endif
