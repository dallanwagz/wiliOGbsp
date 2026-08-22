/* Reusable DECK UI for the C6 flasher -- see fwog_c6flash_ui.h. */
#include "ui/fwog_c6flash_ui.h"
#include "ui/fwog_ui.h"
#include "lcd/lcd_text.h"
#include "lcd/st7789.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define C_BG     0x0000
#define C_FG     0xFFFF
#define C_GREEN  0x07E0
#define C_RED    0xF800
#define C_YELLOW 0xFFE0
#define C_GRAY   0x7BEF
#define C_DIM    0x2124

/* ---- registered catalog ---- */
static const fwog_c6_meta_t *s_meta;
static unsigned s_count;
static void (*s_on_pick)(unsigned);
static unsigned s_cursor;

/* ---- progress state ---- */
static char     s_img[16];
static unsigned s_pct;
static uint32_t s_done, s_total;

void fwog_c6flash_ui_init(const fwog_c6_meta_t *builds, unsigned count,
                          void (*on_pick)(unsigned idx)) {
    s_meta = builds; s_count = count; s_on_pick = on_pick; s_cursor = 0;
}

/* Greedy word-wrap: draw `text` at scale 1 inside [x, x+maxw), 6px/char, one
 * word per break point. Returns the y just past the last line drawn. */
static uint16_t draw_wrapped(uint16_t x, uint16_t y, uint16_t maxw,
                             const char *text, uint16_t fg) {
    const unsigned cpl = maxw / 6u;               /* chars per line at scale 1 */
    if (cpl == 0 || !text) return y;
    char line[42];
    unsigned n = 0;
    const char *w = text;
    while (*w) {
        while (*w == ' ') w++;
        const char *e = w; while (*e && *e != ' ') e++;
        unsigned wl = (unsigned)(e - w);
        if (wl >= sizeof line) wl = sizeof line - 1;   /* pathological long word: hard-cut */
        if (n && n + 1 + wl > cpl) {                   /* would overflow: flush the line */
            line[n] = '\0'; lcd_text_draw(x, y, line, 1, fg, C_BG); y += 12; n = 0;
        }
        if (n && n < cpl) line[n++] = ' ';
        for (unsigned i = 0; i < wl && n < cpl && n < sizeof line - 1; i++) line[n++] = w[i];
        w = e;
    }
    if (n) { line[n] = '\0'; lcd_text_draw(x, y, line, 1, fg, C_BG); y += 12; }
    return y;
}

void fwog_c6flash_chooser_draw(bool full) {
    (void)full;
    st7789_fill_rect(0, 20, ST7789_W, ST7789_H - 20, C_BG);
    lcd_text_draw(6, 24, "Pick a C6 build:", 1, C_GRAY, C_BG);
    st7789_fill_rect(122, 40, 2, ST7789_H - 48, C_DIM);        /* divider */

    /* left: build names, cursor highlighted */
    for (unsigned i = 0; i < s_count && i < 10u; i++) {
        uint16_t y = (uint16_t)(44 + i * 16);
        bool cur = (i == s_cursor);
        if (cur) st7789_fill_rect(2, (uint16_t)(y - 2), 118, 15, C_DIM);
        lcd_text_draw(6, y, s_meta[i].name ? s_meta[i].name : "?", 1,
                      cur ? C_YELLOW : C_FG, cur ? C_DIM : C_BG);
    }

    /* right: the highlighted build's description + when-to-use */
    if (s_cursor < s_count) {
        const fwog_c6_meta_t *b = &s_meta[s_cursor];
        const uint16_t rx = 132, rw = ST7789_W - 132 - 6;
        uint16_t y = 44;
        lcd_text_draw(rx, y, b->name ? b->name : "?", 1, C_GREEN, C_BG); y += 18;
        if (b->description && b->description[0])
            y = draw_wrapped(rx, y, rw, b->description, C_FG);
        if (b->when && b->when[0]) {
            y += 8;
            lcd_text_draw(rx, y, "When:", 1, C_GRAY, C_BG); y += 12;
            y = draw_wrapped(rx, y, rw, b->when, C_GRAY);
        }
    }
    lcd_text_draw(6, ST7789_H - 12, "GRY/YEL move  GRN flash  RED back", 1, C_GRAY, C_BG);
}

void fwog_c6flash_chooser_btn(fwog_btn_id_t b, bool long_press) {
    (void)long_press;
    if (s_count == 0) return;
    switch (b) {
    case FWOG_BTN_GRAY:   s_cursor = (s_cursor + s_count - 1) % s_count; fwog_ui_dirty(); break;
    case FWOG_BTN_YELLOW: s_cursor = (s_cursor + 1) % s_count;          fwog_ui_dirty(); break;
    case FWOG_BTN_GREEN:  if (s_on_pick) s_on_pick(s_cursor); break;
    default: break;
    }
}

void fwog_c6flash_prep_draw(bool full) {
    (void)full;
    st7789_fill_rect(0, 20, ST7789_W, ST7789_H - 20, C_BG);
    lcd_text_draw(10, 28, "FLASH THE C6", 2, C_YELLOW, C_BG);
    lcd_text_draw(6,  56, "The OG arms the C6 for you.", 1, C_FG, C_BG);
    lcd_text_draw(6,  72, "After flashing, tap the C6", 1, C_FG, C_BG);
    lcd_text_draw(6,  88, "RESET once to run it.", 1, C_FG, C_BG);
    lcd_text_draw(6, 116, "If it won't connect, arm by", 1, C_GRAY, C_BG);
    lcd_text_draw(6, 132, "hand: hold BOOT, tap RESET,", 1, C_GRAY, C_BG);
    lcd_text_draw(6, 148, "release BOOT, then retry.", 1, C_GRAY, C_BG);
    lcd_text_draw(10, 182, "GREEN = flash", 2, C_GREEN, C_BG);
    lcd_text_draw(10, 206, "RED = cancel", 1, C_GRAY, C_BG);
}

void fwog_c6flash_progress_reset(void) { s_pct = 0; s_done = 0; s_total = 0; s_img[0] = '\0'; }

unsigned fwog_c6flash_progress_pct(void) { return s_pct; }

void fwog_c6flash_progress_draw(bool full) {
    (void)full;
    st7789_fill_rect(0, 20, ST7789_W, ST7789_H - 20, C_BG);
    lcd_text_draw(10, 28, "FLASHING C6", 2, C_YELLOW, C_BG);

    char l[28];
    snprintf(l, sizeof l, "image: %s", s_img[0] ? s_img : "...");
    lcd_text_draw(10, 54, l, 2, C_FG, C_BG);

    char big[8]; snprintf(big, sizeof big, "%u%%", s_pct);
    lcd_text_draw(10, 84, big, 4, C_GREEN, C_BG);

    if (s_total) {
        snprintf(l, sizeof l, "%lu / %lu KB",
                 (unsigned long)(s_done / 1024u), (unsigned long)(s_total / 1024u));
        lcd_text_draw(140, 96, l, 1, C_GRAY, C_BG);
    }
    st7789_fill_rect(10, 140, 300, 14, C_DIM);
    st7789_fill_rect(10, 140, (uint16_t)(300u * s_pct / 100u), 14, C_GREEN);
    lcd_text_draw(10, 190, "DO NOT UNPLUG", 2, C_RED, C_BG);
}

int fwog_c6flash_ui_on_status(const char *body) {
    if (strncmp(body, "C6img ", 6) == 0) {
        snprintf(s_img, sizeof s_img, "%.15s", body + 6);
        fwog_ui_dirty(); return 1;
    }
    if (strncmp(body, "C6 ", 3) != 0) return 0;
    const char *a = body + 3;
    if (strncmp(a, "done", 4) == 0) { s_pct = 100; return 2; }
    if (strncmp(a, "ERR", 3) == 0)  return 3;
    /* "<pct> <done> <total>" (or "flashing..." -> pct 0) */
    int p = atoi(a); if (p >= 0 && p <= 100) s_pct = (unsigned)p;
    const char *sp = strchr(a, ' ');
    if (sp) { s_done = (uint32_t)strtoul(sp + 1, NULL, 10);
        const char *sp2 = strchr(sp + 1, ' ');
        if (sp2) s_total = (uint32_t)strtoul(sp2 + 1, NULL, 10); }
    fwog_ui_dirty();
    return 1;
}
