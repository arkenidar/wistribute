/*
 * common.h — shared types and the default download URL.
 *
 * Pulled in by every translation unit; keep it tiny.
 */
#ifndef WISTRIBUTE_COMMON_H
#define WISTRIBUTE_COMMON_H

#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

/** Default URL fetched when no --url override is given. */
#define DEFAULT_URL L"https://github.com/pkulchenko/ZeroBraneStudio/archive/refs/heads/master.zip"

/**
 * progress_fn — invoked from the I/O loop with the current byte/entry
 * counter and the known total (or 0 if the total is unknown, e.g. when
 * Content-Length is missing on a chunked response).
 *
 * Called on the same thread that drives the work (worker thread for the
 * GUI, main thread for the CLI). Implementations must be re-entrant
 * across calls but never recursive.
 */
typedef void (*progress_fn)(uint64_t cur, uint64_t total, void *user);

#endif
