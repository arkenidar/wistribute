/*
 * gui.h — Win32 progress GUI entry point.
 *
 * The GUI is a single in-code dialog (no resource template) running its
 * own message loop. All download/extract work happens on a worker thread
 * that posts WM_APP_* messages back to the UI thread.
 */
#ifndef WISTRIBUTE_GUI_H
#define WISTRIBUTE_GUI_H

#include "common.h"

/**
 * gui_run — show the main window and pump messages until it closes.
 *
 * Pre-fills the URL and output-directory fields when the corresponding
 * argument is non-NULL. In EMBEDDED builds the URL field is disabled
 * regardless and shows "(embedded payload)".
 *
 * @param initial_url      Initial URL (DOWNLOAD builds), or NULL.
 * @param initial_out_dir  Initial extract path, or NULL.
 *
 * @return Process exit code (WM_QUIT wParam, normally 0).
 */
int gui_run(const wchar_t *initial_url, const wchar_t *initial_out_dir);

#endif
