/*
 * embed.c — EMBEDDED-build only. Bakes the source zip into the binary
 * via the C23 #embed directive so the offline exe needs no network and
 * no sidecar files. Requires GCC >= 15.
 *
 * The path is relative to this file; the build system stages a copy of
 * payload/zbs.zip next to a generated copy of embed.c so both in-tree
 * and out-of-tree builds resolve the include identically.
 */
#include <stddef.h>

#ifdef MODE_EMBEDDED

static const unsigned char ZBS_ZIP[] = {
#embed "../payload/zbs.zip"
};

const unsigned char *zbs_zip_data(void) { return ZBS_ZIP; }
size_t                zbs_zip_size(void) { return sizeof ZBS_ZIP; }

#endif
