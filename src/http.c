/*
 * http.c — single-shot HTTPS GET via WinHTTP into a malloc'd buffer.
 *
 * Threading: synchronous; called from whichever thread owns the
 * download (CLI main thread or GUI worker thread). Holds no globals.
 *
 * TLS: delegated to Schannel through WinHTTP — no third-party TLS lib,
 * no DLLs beyond winhttp.dll itself.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#include <stdlib.h>
#include <string.h>
#include "http.h"

static int read_uint64_header(HINTERNET req, DWORD info, uint64_t *out) {
    wchar_t buf[64];
    DWORD sz = sizeof buf;
    if (!WinHttpQueryHeaders(req, info, WINHTTP_HEADER_NAME_BY_INDEX,
                             buf, &sz, WINHTTP_NO_HEADER_INDEX))
        return 0;
    *out = (uint64_t)_wcstoui64(buf, NULL, 10);
    return 1;
}

int http_get(const wchar_t *url,
             void **out_buf, size_t *out_len,
             progress_fn on_progress, void *user) {
    int rc = 0;
    HINTERNET hSession = NULL, hConnect = NULL, hRequest = NULL;
    URL_COMPONENTS uc = { .dwStructSize = sizeof uc };
    wchar_t host[256] = {0}, path[2048] = {0};
    uc.lpszHostName = host;     uc.dwHostNameLength = (DWORD)(sizeof host / sizeof host[0]);
    uc.lpszUrlPath = path;      uc.dwUrlPathLength = (DWORD)(sizeof path / sizeof path[0]);

    if (!WinHttpCrackUrl(url, 0, 0, &uc)) { rc = (int)GetLastError(); goto done; }

    hSession = WinHttpOpen(L"wistribute/1.0",
                           WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                           WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) { rc = (int)GetLastError(); goto done; }

    hConnect = WinHttpConnect(hSession, host, uc.nPort, 0);
    if (!hConnect) { rc = (int)GetLastError(); goto done; }

    DWORD flags = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
    hRequest = WinHttpOpenRequest(hConnect, L"GET", path, NULL,
                                  WINHTTP_NO_REFERER,
                                  WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hRequest) { rc = (int)GetLastError(); goto done; }

    /* Follow redirects to any host (GitHub -> codeload). */
    DWORD redir = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption(hRequest, WINHTTP_OPTION_REDIRECT_POLICY, &redir, sizeof redir);

    if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        rc = (int)GetLastError(); goto done;
    }
    if (!WinHttpReceiveResponse(hRequest, NULL)) { rc = (int)GetLastError(); goto done; }

    DWORD status = 0, sz = sizeof status;
    WinHttpQueryHeaders(hRequest,
                        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz,
                        WINHTTP_NO_HEADER_INDEX);
    if (status / 100 != 2) { rc = (int)status ? (int)status : -1; goto done; }

    uint64_t total = 0;
    read_uint64_header(hRequest, WINHTTP_QUERY_CONTENT_LENGTH, &total);

    /* Pre-size to Content-Length when known so chunked responses are the
     * only path that reallocates. 256 KiB initial guess otherwise. */
    size_t cap = total ? (size_t)total : (size_t)(256 * 1024);
    size_t len = 0;
    unsigned char *buf = (unsigned char *)malloc(cap ? cap : 1);
    if (!buf) { rc = ERROR_OUTOFMEMORY; goto done; }

    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(hRequest, &avail)) { rc = (int)GetLastError(); free(buf); goto done; }
        if (avail == 0) break;
        if (len + avail > cap) {
            /* Geometric growth (1.5x + 64 KiB floor) keeps the realloc
             * count O(log N) without overshooting on small payloads. */
            size_t ncap = cap;
            while (ncap < len + avail) ncap = ncap + (ncap >> 1) + 64 * 1024;
            unsigned char *nb = (unsigned char *)realloc(buf, ncap);
            if (!nb) { free(buf); rc = ERROR_OUTOFMEMORY; goto done; }
            buf = nb; cap = ncap;
        }
        DWORD got = 0;
        if (!WinHttpReadData(hRequest, buf + len, avail, &got)) {
            rc = (int)GetLastError(); free(buf); goto done;
        }
        if (got == 0) break;
        len += got;
        if (on_progress) on_progress((uint64_t)len, total, user);
    }

    *out_buf = buf;
    *out_len = len;
    if (on_progress && total == 0) on_progress((uint64_t)len, (uint64_t)len, user);

done:
    if (hRequest) WinHttpCloseHandle(hRequest);
    if (hConnect) WinHttpCloseHandle(hConnect);
    if (hSession) WinHttpCloseHandle(hSession);
    return rc;
}
