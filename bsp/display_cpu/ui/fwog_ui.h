/* DECK — Declarative Escalating Color Kit. The FreeWili OG UI framework.
 *
 * Spec: fwOGAppExplorer repo, docs/deck/deck-ui-spec.md. The one-sentence
 * manual it exists to make true on every screen of every app:
 *
 *     "Red always gets you out; the bottom of the screen tells you the rest."
 *
 * Grammar (fixed; apps get exactly one button):
 *   GRAY    PREV  — row up / page back / decrement; accelerating auto-repeat
 *   YELLOW  NEXT  — row down / page fwd / increment; accelerating auto-repeat
 *   GREEN   SELECT / commit; optional declared long-press (labeled or inert)
 *   BLUE    the screen's ONE app verb; label mandatory — the chip is the
 *           license to bind
 *   RED     OUT — press pops a screen (root: exit card); hold 1.5 s raises
 *           the System Card from ANY depth; the 6 s hardware power-off
 *           (fwog_power_poll) is never touched, only annotated on screen.
 *           NOTHING is ever bindable on RED.
 *
 * Screens are const data in flash; the framework owns navigation, chrome
 * (status bar + hint chips), the exit ladder, and LED echo. Apps draw only
 * the content band (y 20..215) and only when the framework asks.
 *
 * V1 SCOPE, stated plainly (each a spec item deferred, not forgotten):
 *   ponytail: chimes are stubbed silent — no chime asset pipeline yet; the
 *             FeedbackMap hook exists so themes can add them without API churn.
 *   ponytail: Watchdog component absent — the loop is cooperative; a
 *             busy-looping app today blocks its own chrome. Add when any
 *             real app needs long compute.
 *   ponytail: "Exit to App Explorer" is v1 "Restart app" (watchdog reboot) —
 *             the device has no launcher yet; the ladder shape is identical.
 *   ponytail: Detail takes pre-split pages, no auto-wrap — wrap when a real
 *             app has dynamic text.
 */
#ifndef FWOG_UI_H
#define FWOG_UI_H

#include <stdbool.h>
#include <stdint.h>
#include "input/buttons.h"

/* ---- Declarations an app writes ---- */

typedef struct {
    const char *hint;        /* chip label, <=10 chars; REQUIRED if fn set  */
    void (*fn)(void);
    bool destructive;        /* true = force-routed through a confirm card  */
} fwog_ui_verb_t;

typedef struct {
    const char *label;                 /* <=12 chars                        */
    int32_t    *val;                   /* pointer-bound to the app variable */
    int32_t     lo, hi, step;          /* INT range; ignored for bool/enum  */
    bool        is_bool;
    const char *const *enum_labels;    /* non-NULL => enum field            */
    unsigned    enum_count;
} fwog_ui_field_t;

typedef enum {
    FWOG_UI_KIND_LIST,
    FWOG_UI_KIND_DETAIL,
    FWOG_UI_KIND_FORM,
    FWOG_UI_KIND_PROGRESS,
    FWOG_UI_KIND_CANVAS,
} fwog_ui_kind_t;

typedef struct fwog_ui_screen {
    fwog_ui_kind_t kind;
    const char    *title;              /* status bar, <=12 chars            */
    fwog_ui_verb_t blue;               /* the app verb (optional)           */
    fwog_ui_verb_t green_long;         /* declared GREEN long (optional)    */
    union {
        struct { const char *const *rows; unsigned count;
                 void (*on_select)(unsigned idx); } list;
        struct { const char *const *pages; unsigned count; } detail;
        struct { const fwog_ui_field_t *fields; unsigned count;
                 void (*on_commit)(void); } form;
        struct { unsigned (*get_pct)(void); void (*on_done)(void);
                 const char *caption; } progress;
        struct { void (*draw)(bool full);
                 void (*on_btn)(fwog_btn_id_t b, bool long_press); } canvas;
    } u;
} fwog_ui_screen_t;

/* Designated-init sugar: a screen is one line of const data. */
#define FWOG_UI_LIST(t, r, n, sel, ...) \
    { .kind = FWOG_UI_KIND_LIST, .title = (t), \
      .u.list = { .rows = (r), .count = (n), .on_select = (sel) }, __VA_ARGS__ }
#define FWOG_UI_DETAIL(t, p, n, ...) \
    { .kind = FWOG_UI_KIND_DETAIL, .title = (t), \
      .u.detail = { .pages = (p), .count = (n) }, __VA_ARGS__ }
#define FWOG_UI_FORM(t, f, n, commit, ...) \
    { .kind = FWOG_UI_KIND_FORM, .title = (t), \
      .u.form = { .fields = (f), .count = (n), .on_commit = (commit) }, __VA_ARGS__ }
#define FWOG_UI_PROGRESS(t, pct, done, cap, ...) \
    { .kind = FWOG_UI_KIND_PROGRESS, .title = (t), \
      .u.progress = { .get_pct = (pct), .on_done = (done), .caption = (cap) }, __VA_ARGS__ }
#define FWOG_UI_CANVAS(t, drawfn, btnfn, ...) \
    { .kind = FWOG_UI_KIND_CANVAS, .title = (t), \
      .u.canvas = { .draw = (drawfn), .on_btn = (btnfn) }, __VA_ARGS__ }

#define FWOG_FIELD_INT(l, p, lo_, hi_, step_) \
    { .label = (l), .val = (p), .lo = (lo_), .hi = (hi_), .step = (step_) }
#define FWOG_FIELD_BOOL(l, p) \
    { .label = (l), .val = (p), .is_bool = true }
#define FWOG_FIELD_ENUM(l, p, labels, n) \
    { .label = (l), .val = (p), .enum_labels = (labels), .enum_count = (n) }

/* ---- The framework ---- */

/* Run the app. Never returns: the exit ladder's terminal action is a clean
 * restart (see the v1 note above). Calls fwog_power_poll() itself — the app
 * must NOT poll power or buttons; that is now the framework's job. */
void fwog_ui_run(const char *app_name, const fwog_ui_screen_t *screens,
                 unsigned count, unsigned root_id);

void fwog_ui_push(unsigned screen_id);
void fwog_ui_pop(void);                       /* what a RED press calls     */
void fwog_ui_dirty(void);                     /* app data changed: repaint  */
unsigned fwog_ui_current(void);               /* id of the visible screen   */
void fwog_ui_toast(const char *msg);          /* 2 s banner above the chips */

/* Async confirm: paints a card, returns immediately; on GREEN runs on_yes,
 * on RED dismisses. The app's on_tick keeps running underneath — a modal
 * must never starve audio/LED pumps (judge-mandated). */
void fwog_ui_dialog(const char *question, void (*on_yes)(void));

void fwog_ui_on_tick(void (*fn)(uint32_t now_ms));   /* ~50 Hz              */
void fwog_ui_on_exit(void (*save_state)(void));      /* before ANY exit     */
void fwog_ui_menu_add(const char *label, void (*fn)(void)); /* System Card  */

/* ---- Launcher model (multiple apps in one binary) ----
 * Swap the active screen table at runtime; each app keeps its own 0-based
 * screen ids. fwog_ui_on_home() sets what RED does at an app's root: a
 * sub-app points it back to the launcher (which itself leaves it NULL, so RED
 * there still raises the exit card). Set the app's on_tick/on_home around the
 * swap. See og-deck's evilog launcher for the pattern. */
void fwog_ui_set_app(const char *app_name, const fwog_ui_screen_t *screens,
                     unsigned count, unsigned root_id);
void fwog_ui_on_home(void (*fn)(void));

#endif /* FWOG_UI_H */
