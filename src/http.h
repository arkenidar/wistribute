/*
 * http.h — DOWNLOAD-build HTTPS fetch via WinHTTP.
 *
 * Compiled and linked only when MODE_DOWNLOAD is defined; the EMBEDDED
 * build skips this entirely and reads bytes from embed.c instead.
 */
#ifndef WISTRIBUTE_HTTP_H
#define WISTRIBUTE_HTTP_H

#include "common.h"

/**
 * http_get — fetch a URL into memory.
 *
 * Performs a GET against @p url, follows redirects to any host
 * (GitHub release URLs redirect to codeload.github.com), and stores the
 * full response body in a malloc'd buffer that the caller must free().
 *
 * @param url          UTF-16 URL; http or https. TLS is handled by
 *                     Schannel via WinHTTP — no OpenSSL involvement.
 * @param out_buf      [out] receives a malloc'd buffer (free with free()).
 *                     Untouched on failure.
 * @param out_len      [out] receives the byte length written to *out_buf.
 * @param on_progress  Optional. Invoked after each chunk read; total is
 *                     0 when Content-Length is unknown.
 * @param user         Opaque pointer passed back to on_progress.
 *
 * @return 0 on success.
 *         Non-2xx HTTP status codes are returned as-is.
 *         Win32/WinHTTP errors are returned as their GetLastError() code.
 *         ERROR_OUTOFMEMORY on allocation failure.
 */
int http_get(const wchar_t *url,
             void **out_buf, size_t *out_len,
             progress_fn on_progress, void *user);

#endif
