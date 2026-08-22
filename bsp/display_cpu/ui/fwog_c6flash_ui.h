#pragma once
/* Reusable DECK UI for the on-device C6 flasher (fwog_c6flash).
 *
 * Two CANVAS screens an app registers and points at these draw/btn functions:
 *   - a CHOOSER: build names down the left, the highlighted build's
 *     description + "when to use it" on the right; GREEN picks.
 *   - a PROGRESS screen: which image, percent, bytes, DO NOT UNPLUG.
 *
 * The app owns the screen ids and the linkui wiring; this module owns the
 * drawing, the chooser cursor, and parsing the "S:C6*" status lines the
 * fwog_c6flash engine emits. Fed the generated fwog_c6_catalog_meta[] table.
 */
#include <stdbool.h>
#include "ui/fwog_ui.h"   /* fwog_btn_id_t */

/* Per-build metadata the chooser renders (generated: fwog_c6_catalog_meta[]).
 * Bytes live only in MAIN; this is the DISPLAY side's view of a build. */
typedef struct { const char *name; const char *description; const char *when; } fwog_c6_meta_t;

/* Register the catalog and the pick callback (called with the chosen build
 * index when the operator presses GREEN in the chooser). Call once at start. */
void fwog_c6flash_ui_init(const fwog_c6_meta_t *builds, unsigned count,
                          void (*on_pick)(unsigned idx));

/* CHOOSER screen: FWOG_UI_CANVAS(title, fwog_c6flash_chooser_draw,
 *                                fwog_c6flash_chooser_btn). */
void fwog_c6flash_chooser_draw(bool full);
void fwog_c6flash_chooser_btn(fwog_btn_id_t b, bool long_press);

/* PROGRESS screen: FWOG_UI_CANVAS(title, fwog_c6flash_progress_draw, NULL).
 * Call fwog_c6flash_progress_reset() when (re)entering it. */
void fwog_c6flash_progress_draw(bool full);
void fwog_c6flash_progress_reset(void);

/* Current progress percent (0..100), for an app that mirrors it elsewhere
 * (e.g. an LED strip). */
unsigned fwog_c6flash_progress_pct(void);

/* Feed each status line's BODY (the text after "S:"). Returns:
 *   0 not a C6 line   1 progress updated (repaint)
 *   2 finished OK      3 errored (app should toast the line and pop) */
int fwog_c6flash_ui_on_status(const char *body);
