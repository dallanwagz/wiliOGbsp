/* DECK implementation. See fwog_ui.h for the contract and the v1 scope notes.
 *
 * Rendering is bespoke over st7789 + lcd_text, no framebuffer: screens are
 * const flash data and the RAM cost is this file's statics (~1 KB). Redraws
 * are band-granular — a cursor move repaints the content band, never the
 * chrome — which comfortably outruns the 20 Hz repeat rate at the panel's
 * SPI clock. */
#include "ui/fwog_ui.h"

#include <stdio.h>
#include <string.h>

#include "hardware/watchdog.h"
#include "input/buttons.h"
#include "lcd/lcd_text.h"
#include "lcd/st7789.h"
#include "leds/ws2812_driver.h"
#include "pico/stdlib.h"
#include "platform/board.h"
#include "power/power_poll.h"

/* ---- Geometry (spec §3) ---- */
#define BAND_STATUS_Y 0u
#define BAND_STATUS_H 20u
#define BAND_CONTENT_Y 20u
#define BAND_CONTENT_H 196u
#define BAND_HINT_Y 216u
#define BAND_HINT_H 24u
#define CHIP_W 64u
#define LIST_ROW_H 24u
#define LIST_ROWS 8u

/* ---- Timing (spec §2) ---- */
#define LONG_MS 600u
#define SYSCARD_MS 1500u
#define REPEAT_DELAY_MS 400u
#define REPEAT_SLOW_MS 125u /* 8 Hz  */
#define REPEAT_FAST_MS 50u  /* 20 Hz */
#define REPEAT_ACCEL_MS 2000u
#define TOAST_MS 2000u
#define LED_ECHO_MS 120u

/* ---- Palette ---- */
#define C_BG 0x0000u
#define C_FG 0xFFFFu
static uint16_t c_muted, c_chrome, c_accent;
static const uint8_t BTN_RGB[FWOG_BTN_COUNT][3] = {
    { 128, 128, 128 }, /* gray   */
    { 230, 194, 41 },  /* yellow */
    { 46, 158, 79 },   /* green  */
    { 47, 111, 224 },  /* blue   */
    { 194, 50, 55 },   /* red    */
};
static uint16_t btn565(fwog_btn_id_t b) {
    return st7789_rgb565(BTN_RGB[b][0], BTN_RGB[b][1], BTN_RGB[b][2]);
}

/* ---- State ---- */
typedef struct {
    unsigned id;
    unsigned cursor; /* list row / form field / detail page */
    unsigned top;    /* first visible list row               */
} nav_slot_t;

#define NAV_DEPTH 8u
#define MENU_MAX 4u

typedef enum { OV_NONE, OV_DIALOG, OV_SYSCARD, OV_EXIT } overlay_t;

static const fwog_ui_screen_t *g_screens;
static unsigned g_count;
static const char *g_app_name = "";
static nav_slot_t g_stack[NAV_DEPTH];
static unsigned g_depth; /* stack entries; >=1 while running */
static bool g_dirty_all, g_dirty_content, g_dirty_hints;

static overlay_t g_overlay = OV_NONE;
static const char *g_dialog_msg;
static void (*g_dialog_yes)(void);
static unsigned g_ov_cursor; /* row inside System/exit card */

static char g_toast[40];
static uint32_t g_toast_until;

static void (*g_on_tick)(uint32_t);
static void (*g_on_exit)(void);
static void (*g_on_home)(void);   /* RED at root -> here (launcher), if set */
static struct { const char *label; void (*fn)(void); } g_menu[MENU_MAX];
static unsigned g_menu_count;

/* Per-button hold bookkeeping for the grammar engine. The debouncing itself
 * lives in the BSP; this layer only times settled holds. */
static uint32_t g_down_at[FWOG_BTN_COUNT];
static uint32_t g_next_repeat[FWOG_BTN_COUNT];
static bool g_long_fired[FWOG_BTN_COUNT];
static bool g_red_consumed; /* System Card opened during this hold */

static uint32_t g_led_until[8];
static bool g_progress_owns_leds;
static unsigned g_last_pct = 101; /* force first paint */

/* ---- Small helpers ---- */
static const fwog_ui_screen_t *cur(void) { return &g_screens[g_stack[g_depth - 1].id]; }
static nav_slot_t *slot(void) { return &g_stack[g_depth - 1]; }

static void feedback_chime(unsigned which) {
    (void)which; /* ponytail: silent v1 — FeedbackMap hook, see header */
}

static void led_echo(fwog_btn_id_t b, uint32_t now) {
    /* LED per button: strip index 1..5 mirrors GRAY..RED positions. RED is
     * exempt — the power machine owns the strip during red holds. */
    if (b == FWOG_BTN_RED || !ws2812_ready()) return;
    unsigned idx = 1u + (unsigned)b;
    ws2812_set_color(idx, BTN_RGB[b][0] / 4, BTN_RGB[b][1] / 4, BTN_RGB[b][2] / 4);
    g_led_until[idx] = now + LED_ECHO_MS;
}

static void led_service(uint32_t now) {
    if (!ws2812_ready()) return;
    for (unsigned i = 0; i < 8; ++i)
        if (g_led_until[i] && (int32_t)(now - g_led_until[i]) >= 0) {
            g_led_until[i] = 0;
            if (!g_progress_owns_leds) ws2812_set_color(i, 0, 0, 0);
        }
    ws2812_process();
}

/* ---- Chrome ---- */
static void draw_status(void) {
    st7789_fill_rect(0, BAND_STATUS_Y, ST7789_W, BAND_STATUS_H, c_chrome);
    lcd_text_draw(4, 2, g_app_name, 2, C_FG, c_chrome);
    const char *t = cur()->title ? cur()->title : "";
    unsigned tw = lcd_text_width_px((unsigned)strlen(t), 2);
    lcd_text_draw((uint16_t)(ST7789_W / 2u - tw / 2u), 2, t, 2, c_accent, c_chrome);
    /* Breadcrumb depth dots, right-aligned: one per stack level. */
    for (unsigned i = 0; i < g_depth && i < 6; ++i)
        st7789_fill_rect((uint16_t)(ST7789_W - 10 - 8 * i), 8, 5, 5, C_FG);
}

typedef struct { const char *label; bool bound; } chip_t;

static void chips_for_screen(chip_t out[FWOG_BTN_COUNT]) {
    const fwog_ui_screen_t *s = cur();
    bool rocker = true, select = true;
    const char *sel_label = "SELECT";
    switch (s->kind) {
    case FWOG_UI_KIND_LIST: sel_label = "SELECT"; break;
    case FWOG_UI_KIND_DETAIL: sel_label = ""; select = false; break;
    case FWOG_UI_KIND_FORM: sel_label = "NEXT/OK"; break;
    case FWOG_UI_KIND_PROGRESS: rocker = false; select = false; break;
    case FWOG_UI_KIND_CANVAS: sel_label = s->u.canvas.on_btn ? "A" : ""; select = s->u.canvas.on_btn; rocker = s->u.canvas.on_btn; break;
    }
    out[FWOG_BTN_GRAY] = (chip_t){ rocker ? "PREV" : "", rocker };
    out[FWOG_BTN_YELLOW] = (chip_t){ rocker ? "NEXT" : "", rocker };
    out[FWOG_BTN_GREEN] = (chip_t){ select ? sel_label : "", select };
    out[FWOG_BTN_BLUE] = (chip_t){ s->blue.fn ? s->blue.hint : "", s->blue.fn != NULL };
    out[FWOG_BTN_RED] = (chip_t){ g_depth > 1 ? "BACK" : "EXIT", true };
}

static void draw_hints(void) {
    chip_t chips[FWOG_BTN_COUNT];
    if (g_overlay == OV_DIALOG) {
        chips[FWOG_BTN_GRAY] = (chip_t){ "", false };
        chips[FWOG_BTN_YELLOW] = (chip_t){ "", false };
        chips[FWOG_BTN_GREEN] = (chip_t){ "YES", true };
        chips[FWOG_BTN_BLUE] = (chip_t){ "", false };
        chips[FWOG_BTN_RED] = (chip_t){ "NO", true };
    } else if (g_overlay != OV_NONE) {
        chips[FWOG_BTN_GRAY] = (chip_t){ "PREV", true };
        chips[FWOG_BTN_YELLOW] = (chip_t){ "NEXT", true };
        chips[FWOG_BTN_GREEN] = (chip_t){ "SELECT", true };
        chips[FWOG_BTN_BLUE] = (chip_t){ "", false };
        chips[FWOG_BTN_RED] = (chip_t){ g_overlay == OV_SYSCARD ? "RESTART" : "YES", true };
    } else {
        chips_for_screen(chips);
    }
    for (unsigned i = 0; i < FWOG_BTN_COUNT; ++i) {
        uint16_t x = (uint16_t)(i * CHIP_W);
        bool bound = chips[i].bound && chips[i].label && chips[i].label[0];
        /* Color band on top: the button's own color when bound, dim when not
         * — silence is also labeled (spec: dim chip = "does nothing here"). */
        st7789_fill_rect(x, BAND_HINT_Y, CHIP_W, 4, bound ? btn565((fwog_btn_id_t)i) : c_muted);
        st7789_fill_rect(x, BAND_HINT_Y + 4, CHIP_W, BAND_HINT_H - 4, c_chrome);
        if (bound) {
            unsigned n = (unsigned)strlen(chips[i].label);
            if (n > 10) n = 10;
            uint16_t tx = (uint16_t)(x + CHIP_W / 2u - lcd_text_width_px(n, 1) / 2u);
            lcd_text_draw(tx, BAND_HINT_Y + 11, chips[i].label, 1, C_FG, c_chrome);
        } else {
            st7789_fill_rect((uint16_t)(x + CHIP_W / 2u - 2u), BAND_HINT_Y + 13, 4, 4, c_muted);
        }
        st7789_fill_rect((uint16_t)(x + CHIP_W - 1u), BAND_HINT_Y, 1, BAND_HINT_H, C_BG);
    }
}

/* ---- Content band per screen kind ---- */
static void field_value_text(const fwog_ui_field_t *f, char *out, size_t n) {
    if (f->is_bool)
        snprintf(out, n, "%s", *f->val ? "ON" : "OFF");
    else if (f->enum_labels)
        snprintf(out, n, "%s",
                 (*f->val >= 0 && (unsigned)*f->val < f->enum_count)
                     ? f->enum_labels[*f->val] : "?");
    else
        snprintf(out, n, "%ld", (long)*f->val);
}

static void draw_content(void) {
    const fwog_ui_screen_t *s = cur();
    nav_slot_t *n = slot();
    if (s->kind != FWOG_UI_KIND_CANVAS)
        st7789_fill_rect(0, BAND_CONTENT_Y, ST7789_W, BAND_CONTENT_H, C_BG);

    switch (s->kind) {
    case FWOG_UI_KIND_LIST: {
        const char *const *subs = s->u.list.subs;
        if (subs) {
            /* Master-detail ("blade") layout for the launcher: a name list in
             * the left column, and the ENTIRE right half a detail panel for the
             * highlighted app -- its name as a header over a big word-wrapped
             * description. Moving the cursor (GRAY/YELLOW) updates the panel. */
            const uint16_t split = 148u;                 /* left name-column width */
            const uint16_t px = (uint16_t)(split + 8u);  /* detail text x */
            const unsigned pcols = (unsigned)((ST7789_W - px - 6) / 12u);  /* scale-2 glyph = 12 px */
            /* LEFT: the name list, standard single-line rows. */
            if (n->cursor < n->top) n->top = n->cursor;
            if (n->cursor >= n->top + LIST_ROWS) n->top = n->cursor - LIST_ROWS + 1;
            for (unsigned r = 0; r < LIST_ROWS && n->top + r < s->u.list.count; ++r) {
                unsigned idx = n->top + r;
                bool cur = (idx == n->cursor);
                uint16_t y = (uint16_t)(BAND_CONTENT_Y + 2 + r * LIST_ROW_H);
                if (cur) st7789_fill_rect(0, y, split, LIST_ROW_H, c_accent);
                lcd_text_draw(8, (uint16_t)(y + 4), s->u.list.rows[idx], 2,
                              cur ? C_BG : C_FG, cur ? c_accent : C_BG);
            }
            if (s->u.list.count > LIST_ROWS) {   /* scroll tick, left column edge */
                unsigned h = BAND_CONTENT_H * LIST_ROWS / s->u.list.count;
                unsigned y0 = BAND_CONTENT_Y + BAND_CONTENT_H * n->top / s->u.list.count;
                st7789_fill_rect((uint16_t)(split - 4), (uint16_t)y0, 2, (uint16_t)(h ? h : 1), c_muted);
            }
            /* Divider. */
            st7789_fill_rect(split, BAND_CONTENT_Y, 1, BAND_CONTENT_H, c_muted);
            /* RIGHT: the selected app's detail. */
            lcd_text_draw(px, (uint16_t)(BAND_CONTENT_Y + 8), s->u.list.rows[n->cursor], 2, c_accent, C_BG);
            const char *p = subs[n->cursor] ? subs[n->cursor] : "";
            uint16_t ly = (uint16_t)(BAND_CONTENT_Y + 36);
            for (unsigned line = 0; line < 8u && *p; ++line) {
                unsigned take = 0, lastsp = 0;
                while (p[take] && take < pcols) { if (p[take] == ' ') lastsp = take; ++take; }
                unsigned cut = (p[take] && lastsp) ? lastsp : take;   /* break on a space if mid-word */
                char buf[24];
                unsigned c = cut < sizeof buf - 1 ? cut : (unsigned)(sizeof buf - 1);
                for (unsigned k = 0; k < c; ++k) buf[k] = p[k];
                buf[c] = '\0';
                lcd_text_draw(px, ly, buf, 2, C_FG, C_BG);   /* big, readable */
                ly = (uint16_t)(ly + 20);
                p += cut;
                while (*p == ' ') ++p;
            }
            break;
        }
        /* Plain single-line list (unchanged). */
        if (n->cursor < n->top) n->top = n->cursor;
        if (n->cursor >= n->top + LIST_ROWS) n->top = n->cursor - LIST_ROWS + 1;
        for (unsigned r = 0; r < LIST_ROWS && n->top + r < s->u.list.count; ++r) {
            bool cur_row = (n->top + r == n->cursor);
            uint16_t y = (uint16_t)(BAND_CONTENT_Y + 2 + r * LIST_ROW_H);
            if (cur_row) st7789_fill_rect(0, y, ST7789_W - 6, LIST_ROW_H, c_accent);
            lcd_text_draw(8, (uint16_t)(y + 4), s->u.list.rows[n->top + r], 2,
                          cur_row ? C_BG : C_FG, cur_row ? c_accent : C_BG);
        }
        if (s->u.list.count > LIST_ROWS) {
            unsigned h = BAND_CONTENT_H * LIST_ROWS / s->u.list.count;
            unsigned y0 = BAND_CONTENT_Y + BAND_CONTENT_H * n->top / s->u.list.count;
            st7789_fill_rect(ST7789_W - 4, (uint16_t)y0, 3, (uint16_t)(h ? h : 1), c_muted);
        }
        break;
    }
    case FWOG_UI_KIND_DETAIL: {
        const char *page = s->u.detail.pages[n->cursor];
        /* Pre-split pages; lines separated by '\n', 8 lines max. */
        char line[28];
        unsigned li = 0, ci = 0;
        for (const char *p = page; li < 8; ++p) {
            if (*p == '\n' || *p == '\0' || ci == sizeof line - 1) {
                line[ci] = '\0';
                lcd_text_draw(6, (uint16_t)(BAND_CONTENT_Y + 4 + li * 24), line, 2, C_FG, C_BG);
                ci = 0;
                ++li;
                if (*p == '\0') break;
            } else {
                line[ci++] = *p;
            }
        }
        char pos[8];
        snprintf(pos, sizeof pos, "%u/%u", n->cursor + 1, s->u.detail.count);
        lcd_text_draw((uint16_t)(ST7789_W - 8 - lcd_text_width_px((unsigned)strlen(pos), 1)),
                      BAND_CONTENT_Y + BAND_CONTENT_H - 12, pos, 1, c_muted, C_BG);
        break;
    }
    case FWOG_UI_KIND_FORM: {
        for (unsigned i = 0; i < s->u.form.count && i < LIST_ROWS; ++i) {
            const fwog_ui_field_t *f = &s->u.form.fields[i];
            bool cur_row = (i == n->cursor);
            uint16_t y = (uint16_t)(BAND_CONTENT_Y + 2 + i * LIST_ROW_H);
            if (cur_row) st7789_fill_rect(0, y, ST7789_W, LIST_ROW_H, c_chrome);
            lcd_text_draw(8, (uint16_t)(y + 4), f->label, 2, C_FG, cur_row ? c_chrome : C_BG);
            char v[16];
            field_value_text(f, v, sizeof v);
            uint16_t vx = (uint16_t)(ST7789_W - 12 - lcd_text_width_px((unsigned)strlen(v), 2));
            lcd_text_draw(vx, (uint16_t)(y + 4), v, 2, cur_row ? c_accent : c_muted,
                          cur_row ? c_chrome : C_BG);
        }
        break;
    }
    case FWOG_UI_KIND_PROGRESS: {
        unsigned pct = s->u.progress.get_pct ? s->u.progress.get_pct() : 0;
        if (pct > 100) pct = 100;
        g_last_pct = pct;
        if (s->u.progress.caption)
            lcd_text_draw(20, BAND_CONTENT_Y + 24, s->u.progress.caption, 2, C_FG, C_BG);
        char big[8];
        snprintf(big, sizeof big, "%u%%", pct);
        unsigned bw = lcd_text_width_px((unsigned)strlen(big), 4);
        lcd_text_draw((uint16_t)(ST7789_W / 2 - bw / 2), BAND_CONTENT_Y + 70, big, 4, c_accent, C_BG);
        st7789_fill_rect(20, BAND_CONTENT_Y + 130, 280, 16, c_chrome);
        st7789_fill_rect(20, BAND_CONTENT_Y + 130, (uint16_t)(280u * pct / 100u), 16, c_accent);
        /* Auto-mirror to the strip as a 7-step ring (spec §7). */
        if (ws2812_ready()) {
            g_progress_owns_leds = true;
            unsigned lit = pct * 7u / 100u;
            for (unsigned i = 0; i < 7; ++i)
                ws2812_set_color(i, 0, i < lit ? 30 : 0, 0);   /* green progress ring */
        }
        break;
    }
    case FWOG_UI_KIND_CANVAS:
        if (s->u.canvas.draw) s->u.canvas.draw(g_dirty_all || g_dirty_content);
        break;
    }
}

/* ---- Overlays (paint over the content band; chrome survives) ---- */
static void draw_overlay(void) {
    if (g_overlay == OV_NONE) return;
    const uint16_t x = 20, y = BAND_CONTENT_Y + 18, w = 280,
                   h = (g_overlay == OV_SYSCARD) ? 160 : 110;
    st7789_fill_rect(x, y, w, h, c_chrome);
    st7789_fill_rect(x, y, w, 2, c_accent);
    st7789_fill_rect(x, (uint16_t)(y + h - 2), w, 2, c_accent);

    if (g_overlay == OV_DIALOG) {
        lcd_text_draw(x + 10, y + 14, g_dialog_msg ? g_dialog_msg : "", 2, C_FG, c_chrome);
        lcd_text_draw(x + 10, (uint16_t)(y + h - 26), "GREEN yes   RED no", 1, c_muted, c_chrome);
        return;
    }

    /* System Card / exit card: legend + selectable rows. */
    const char *rows[3 + MENU_MAX];
    unsigned n = 0;
    rows[n++] = "Resume";
    if (g_overlay == OV_SYSCARD)
        for (unsigned i = 0; i < g_menu_count; ++i) rows[n++] = g_menu[i].label;
    rows[n++] = "Restart app";

    lcd_text_draw(x + 10, y + 8,
                  g_overlay == OV_SYSCARD ? "SYSTEM" : "Leave app?", 2, c_accent, c_chrome);
    if (g_overlay == OV_SYSCARD) {
        lcd_text_draw(x + 10, y + 30, "GRAY/YEL move  GREEN pick", 1, c_muted, c_chrome);
        lcd_text_draw(x + 10, y + 40, "RED restarts   hold=power", 1, c_muted, c_chrome);
    }
    uint16_t ry = (uint16_t)(y + (g_overlay == OV_SYSCARD ? 56 : 34));
    for (unsigned i = 0; i < n; ++i, ry += 22) {
        bool cur_row = (i == g_ov_cursor);
        if (cur_row) st7789_fill_rect((uint16_t)(x + 6), ry, (uint16_t)(w - 12), 20, c_accent);
        lcd_text_draw((uint16_t)(x + 12), (uint16_t)(ry + 2), rows[i], 2,
                      cur_row ? C_BG : C_FG, cur_row ? c_accent : c_chrome);
    }
}

static unsigned overlay_rows(void) {
    return (g_overlay == OV_SYSCARD) ? 2u + g_menu_count : 2u;
}

static void draw_toast(uint32_t now) {
    if (!g_toast[0]) return;
    if ((int32_t)(now - g_toast_until) >= 0) {
        g_toast[0] = '\0';
        g_dirty_content = true; /* wipe the banner */
        return;
    }
    st7789_fill_rect(10, BAND_HINT_Y - 30, 300, 28, c_accent);
    lcd_text_draw(18, BAND_HINT_Y - 24, g_toast, 2, C_BG, c_accent);
}

/* Power ladder annotation (spec §4): the BSP machine owns the LEDs and the
 * 6 s cutoff; this only paints the on-screen countdown so the hold is
 * legible. Drawn last, over everything — a fullscreen canvas included. */
static void draw_power_banner(const fwog_power_t *p) {
    static bool shown;      /* was the box on screen last frame? */
    if (p->armed && p->progress > 0) {
        /* Paint the box straight over whatever's underneath -- do NOT force a
         * content repaint. The old code set g_dirty_content every frame, which
         * repainted the base screen (and any open overlay) at full rate under
         * the box: the underlying window flashed through on each frame while
         * the box redrew on top -- the "shaky, several windows at once" bug.
         * Redrawing just this fixed box each frame is flicker-free because
         * nothing under it moves; only the %% text changes. */
        st7789_fill_rect(30, 100, 260, 44, btn565(FWOG_BTN_RED));
        char msg[48];
        snprintf(msg, sizeof msg, "POWER OFF %u%%  release=stay", p->progress);
        lcd_text_draw(38, 114, msg, 1, C_FG, btn565(FWOG_BTN_RED));
        /* Say how to come back: waking is a GRAY hold (or USB) -- see
         * power/ship_mode.h. Shown here because once the board is dark there is
         * no screen left to tell them. */
        lcd_text_draw(38, 128, "hold GRAY (or USB) to wake", 1, C_FG, btn565(FWOG_BTN_RED));
        shown = true;
    } else if (shown) {
        g_dirty_content = true; /* hold ended: repaint underneath ONCE to wipe the box */
        shown = false;
    }
}

/* ---- Actions ---- */
static void do_exit(void) {
    if (g_on_exit) g_on_exit();
    watchdog_reboot(0, 0, 0); /* clean restart; see the v1 note in fwog_ui.h */
    while (true) tight_loop_contents();
}

static void run_verb(const fwog_ui_verb_t *v);
static const fwog_ui_verb_t *g_pending_destructive;
static void destructive_yes(void) {
    if (g_pending_destructive && g_pending_destructive->fn) g_pending_destructive->fn();
    g_pending_destructive = NULL;
}
static void run_verb(const fwog_ui_verb_t *v) {
    if (!v->fn) return;
    if (!v->hint || !v->hint[0]) return; /* no label, no license: inert */
    if (v->destructive) {
        g_pending_destructive = v;
        fwog_ui_dialog(v->hint, destructive_yes);
        return;
    }
    v->fn();
}

static void overlay_select(void) {
    unsigned menu_rows = (g_overlay == OV_SYSCARD) ? g_menu_count : 0;
    if (g_ov_cursor == 0) { /* Resume */
        g_overlay = OV_NONE;
    } else if (g_ov_cursor <= menu_rows) {
        void (*fn)(void) = g_menu[g_ov_cursor - 1].fn;
        g_overlay = OV_NONE;
        if (fn) fn();
    } else {
        do_exit();
    }
    g_dirty_all = true;
}

static void rocker(int dir) {
    nav_slot_t *n = slot();
    const fwog_ui_screen_t *s = cur();
    if (g_overlay == OV_SYSCARD || g_overlay == OV_EXIT) {
        unsigned rows = overlay_rows();
        g_ov_cursor = (unsigned)(((int)g_ov_cursor + dir + (int)rows) % (int)rows);
        g_dirty_content = true;
        return;
    }
    if (g_overlay != OV_NONE) return;
    switch (s->kind) {
    case FWOG_UI_KIND_LIST:
        if (s->u.list.count)
            n->cursor = (unsigned)(((int)n->cursor + dir + (int)s->u.list.count)
                                   % (int)s->u.list.count);
        break;
    case FWOG_UI_KIND_DETAIL:
        if (dir > 0 && n->cursor + 1 < s->u.detail.count) n->cursor++;
        else if (dir < 0 && n->cursor > 0) n->cursor--;
        break;
    case FWOG_UI_KIND_FORM: {
        const fwog_ui_field_t *f = &s->u.form.fields[n->cursor];
        if (f->is_bool) *f->val = !*f->val;
        else if (f->enum_labels)
            *f->val = (int32_t)(((*f->val + dir) % (int32_t)f->enum_count
                                 + (int32_t)f->enum_count) % (int32_t)f->enum_count);
        else {
            int32_t v = *f->val + dir * f->step;
            if (v < f->lo) v = f->lo;
            if (v > f->hi) v = f->hi;
            *f->val = v;
        }
        break;
    }
    case FWOG_UI_KIND_CANVAS:
        if (s->u.canvas.on_btn)
            s->u.canvas.on_btn(dir < 0 ? FWOG_BTN_GRAY : FWOG_BTN_YELLOW, false);
        return; /* canvas repaints itself via dirty below */
    default: return;
    }
    g_dirty_content = true;
}

static void green_press(void) {
    nav_slot_t *n = slot();
    const fwog_ui_screen_t *s = cur();
    if (g_overlay == OV_DIALOG) {
        void (*yes)(void) = g_dialog_yes;
        g_overlay = OV_NONE;
        g_dirty_all = true;
        if (yes) yes();
        return;
    }
    if (g_overlay == OV_SYSCARD || g_overlay == OV_EXIT) { overlay_select(); return; }
    switch (s->kind) {
    case FWOG_UI_KIND_LIST:
        if (s->u.list.on_select && s->u.list.count) s->u.list.on_select(n->cursor);
        break;
    case FWOG_UI_KIND_FORM:
        if (n->cursor + 1 < s->u.form.count) { n->cursor++; g_dirty_content = true; }
        else {
            if (s->u.form.on_commit) s->u.form.on_commit();
            fwog_ui_toast("Saved");
            fwog_ui_pop();
        }
        break;
    case FWOG_UI_KIND_CANVAS:
        if (s->u.canvas.on_btn) s->u.canvas.on_btn(FWOG_BTN_GREEN, false);
        break;
    default: break;
    }
}

static void red_press(void) {
    /* RED = out of the current context, universally (spec §2). In the exit
     * and System cards it CONFIRMS leaving — a panic masher escapes, never
     * loops. In a plain dialog it dismisses. */
    switch (g_overlay) {
    case OV_DIALOG:
        g_overlay = OV_NONE;
        g_pending_destructive = NULL;
        g_dirty_all = true;
        return;
    case OV_SYSCARD:
    case OV_EXIT:
        do_exit();
        return;
    case OV_NONE: break;
    }
    if (g_depth > 1) { fwog_ui_pop(); return; }
    /* At the root of a sub-app (launcher model): RED goes HOME to the launcher
     * instead of exiting the whole binary. The launcher itself sets no home
     * hook, so RED there still raises the exit card. */
    if (g_on_home) { g_on_home(); return; }
    g_overlay = OV_EXIT;
    g_ov_cursor = 0;
    g_dirty_content = true;
    g_dirty_hints = true;
}

/* ---- Grammar engine ---- */
static void engine(const fwog_power_t *p, uint32_t now) {
    const fwog_buttons_t *b = &p->buttons;
    const fwog_ui_screen_t *s = cur();

    for (unsigned i = 0; i < FWOG_BTN_COUNT; ++i) {
        uint8_t bit = FWOG_BTN_BIT(i);
        if (b->pressed & bit) {
            g_down_at[i] = now;
            g_long_fired[i] = false;
            if (i == FWOG_BTN_RED) g_red_consumed = false;
            g_next_repeat[i] = now + REPEAT_DELAY_MS;
            led_echo((fwog_btn_id_t)i, now);
            feedback_chime(0);
        }
    }

    /* Rocker: act on press edge, then accelerating repeat while held. */
    const fwog_btn_id_t rockers[2] = { FWOG_BTN_GRAY, FWOG_BTN_YELLOW };
    for (unsigned r = 0; r < 2; ++r) {
        fwog_btn_id_t id = rockers[r];
        int dir = (id == FWOG_BTN_GRAY) ? -1 : 1;
        uint8_t bit = FWOG_BTN_BIT(id);
        if (b->pressed & bit) rocker(dir);
        else if ((b->down & bit) && (int32_t)(now - g_next_repeat[id]) >= 0) {
            rocker(dir);
            uint32_t held = now - g_down_at[id];
            g_next_repeat[id] = now + (held >= REPEAT_ACCEL_MS ? REPEAT_FAST_MS
                                                               : REPEAT_SLOW_MS);
        }
    }

    /* GREEN: press on edge unless a long is declared, then press-on-release
     * under the threshold and the long at the threshold (spec §2). */
    {
        uint8_t bit = FWOG_BTN_BIT(FWOG_BTN_GREEN);
        bool has_long = s->green_long.fn && g_overlay == OV_NONE;
        if (!has_long) {
            if (b->pressed & bit) green_press();
        } else {
            if ((b->down & bit) && !g_long_fired[FWOG_BTN_GREEN]
                && now - g_down_at[FWOG_BTN_GREEN] >= LONG_MS) {
                g_long_fired[FWOG_BTN_GREEN] = true;
                run_verb(&s->green_long);
            }
            if ((b->released & bit) && !g_long_fired[FWOG_BTN_GREEN]) green_press();
        }
    }

    /* BLUE: the app verb, press edge, overlays swallow it. */
    if ((b->pressed & FWOG_BTN_BIT(FWOG_BTN_BLUE)) && g_overlay == OV_NONE) {
        if (s->kind == FWOG_UI_KIND_CANVAS && s->u.canvas.on_btn && !s->blue.fn)
            s->u.canvas.on_btn(FWOG_BTN_BLUE, false);
        else
            run_verb(&s->blue);
    }

    /* RED: release = out (if the hold didn't escalate); 1.5 s = System Card.
     * The 6 s power path belongs to fwog_power_poll and is not touched. */
    {
        uint8_t bit = FWOG_BTN_BIT(FWOG_BTN_RED);
        if ((b->down & bit) && !g_red_consumed
            && now - g_down_at[FWOG_BTN_RED] >= SYSCARD_MS
            && g_overlay == OV_NONE) {
            g_red_consumed = true;
            g_overlay = OV_SYSCARD;
            g_ov_cursor = 0;
            g_dirty_content = true;
            g_dirty_hints = true;
        }
        /* Declutter the power-off: the System Card (Resume/Restart) opens at
         * 1.5 s, but once the 6 s power countdown is clearly underway (>=3 s)
         * the intent is plainly "power off" -- drop the card so only the red
         * POWER OFF bar shows. g_red_consumed stays set, so it won't reopen. */
        if (p->armed && p->progress >= 50u && g_overlay == OV_SYSCARD) {
            g_overlay = OV_NONE;
            g_dirty_all = true;
        }
        if ((b->released & bit) && !g_red_consumed) red_press();
        if (b->released & bit) g_red_consumed = false;
    }
}

/* ---- Public API ---- */
void fwog_ui_push(unsigned screen_id) {
    if (g_depth >= NAV_DEPTH || screen_id >= g_count) return;
    g_stack[g_depth++] = (nav_slot_t){ .id = screen_id };
    g_progress_owns_leds = false;
    g_dirty_all = true;
}

void fwog_ui_pop(void) {
    if (g_depth > 1) {
        g_depth--;
        g_progress_owns_leds = false;
        if (ws2812_ready())
            for (unsigned i = 0; i < 7; ++i) ws2812_set_color(i, 0, 0, 0);
        g_dirty_all = true;
    }
}

void fwog_ui_dirty(void) { g_dirty_content = true; }
unsigned fwog_ui_current(void) { return g_stack[g_depth ? g_depth - 1 : 0].id; }

void fwog_ui_toast(const char *msg) {
    snprintf(g_toast, sizeof g_toast, "%s", msg ? msg : "");
    g_toast_until = to_ms_since_boot(get_absolute_time()) + TOAST_MS;
}

void fwog_ui_dialog(const char *question, void (*on_yes)(void)) {
    g_dialog_msg = question;
    g_dialog_yes = on_yes;
    g_overlay = OV_DIALOG;
    g_dirty_content = true;
    g_dirty_hints = true;
}

void fwog_ui_on_tick(void (*fn)(uint32_t)) { g_on_tick = fn; }
void fwog_ui_on_exit(void (*fn)(void)) { g_on_exit = fn; }

void fwog_ui_menu_add(const char *label, void (*fn)(void)) {
    if (g_menu_count < MENU_MAX) {
        g_menu[g_menu_count].label = label;
        g_menu[g_menu_count].fn = fn;
        g_menu_count++;
    }
}

/* Point the framework at a screen table and reset the nav stack to its root.
 * Shared by fwog_ui_run (initial app) and fwog_ui_set_app (launcher swap). */
static void set_app(const char *app_name, const fwog_ui_screen_t *screens,
                    unsigned count, unsigned root_id) {
    g_app_name = app_name ? app_name : "";
    g_screens = screens;
    g_count = count;
    g_overlay = OV_NONE;
    g_depth = 0;
    fwog_ui_push(root_id < count ? root_id : 0);
    g_dirty_all = true;
}

/* Swap the active app at runtime (launcher model). Each app keeps its OWN
 * 0-based screen ids -- the table pointer is what changes -- so apps compose
 * without renumbering. Set the app's on_tick / on_home around this call. */
void fwog_ui_set_app(const char *app_name, const fwog_ui_screen_t *screens,
                     unsigned count, unsigned root_id) {
    set_app(app_name, screens, count, root_id);
}

void fwog_ui_on_home(void (*fn)(void)) { g_on_home = fn; }

void fwog_ui_run(const char *app_name, const fwog_ui_screen_t *screens,
                 unsigned count, unsigned root_id) {
    set_app(app_name, screens, count, root_id);

    c_muted = st7789_rgb565(90, 82, 80);
    c_chrome = st7789_rgb565(34, 28, 27);
    c_accent = st7789_rgb565(194, 50, 55);

    st7789_init_begin();
    absolute_time_t lcd_deadline = make_timeout_time_ms(600);
    while (!st7789_ready() && !time_reached(lcd_deadline)) {
        st7789_init_step();
        sleep_ms(1);
    }
    st7789_clear(C_BG);
    board_backlight(255);

    uint32_t last_prog_paint = 0;
    while (true) {
        uint32_t now = to_ms_since_boot(get_absolute_time());
        fwog_power_t p = fwog_power_poll(now); /* the ONE poll; see header */
        engine(&p, now);
        if (g_on_tick) g_on_tick(now);

        /* Progress screens repaint on a clock (value changes), others on
         * demand. */
        if (cur()->kind == FWOG_UI_KIND_PROGRESS && g_overlay == OV_NONE) {
            unsigned pct = cur()->u.progress.get_pct ? cur()->u.progress.get_pct() : 0;
            if (pct != g_last_pct && now - last_prog_paint >= 100) {
                g_dirty_content = true;
                last_prog_paint = now;
            }
            if (pct >= 100 && cur()->u.progress.on_done) {
                void (*done)(void) = cur()->u.progress.on_done;
                g_dirty_content = true;
                done();
            }
        }

        if (g_dirty_all) {
            st7789_clear(C_BG);
            draw_status();
            draw_content();
            draw_overlay();
            draw_hints();
            g_dirty_all = g_dirty_content = g_dirty_hints = false;
        } else {
            if (g_dirty_content) {
                draw_content();
                draw_overlay();
                g_dirty_content = false;
            }
            if (g_dirty_hints) {
                draw_hints();
                g_dirty_hints = false;
            }
        }
        draw_toast(now);
        draw_power_banner(&p);
        led_service(now);
        sleep_ms(2);
    }
}
