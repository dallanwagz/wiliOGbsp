/* DECK demo — every screen kind the framework has, in one app. Doubles as
 * the living documentation of docs/deck-ui-spec.md's developer story: count
 * the lines an app actually writes.
 *
 * FWOG_POWER_DEFAULT: the contract's promise is "fwog_power_poll() runs
 * every loop", and fwog_ui_run() is what keeps it — the framework owns the
 * input loop and performs the one poll per iteration the BSP requires. */
#include "fwog_display.h"
#include "pico/stdlib.h"
#include "ui/fwog_ui.h"

FWOG_POWER_DEFAULT();

/* ---- App state ---- */
static int32_t s_count;
static int32_t s_bright = 255, s_sound = 1, s_theme;
static uint32_t s_brew_start_ms;
static bool s_brewing;
static int s_box_x = 140, s_box_y = 90, s_dx = 2, s_dy = 2;
static uint32_t s_last_ms;

enum { SCR_MENU, SCR_SET, SCR_STORY, SCR_BREW, SCR_PLAY };

/* ---- Verbs and callbacks ---- */
static void count_up(void) {
    s_count++;
    char m[24];
    snprintf(m, sizeof m, "Count: %ld", (long)s_count);
    fwog_ui_toast(m);
}

static void apply_settings(void) { board_backlight((uint8_t)s_bright); }

static unsigned brew_pct(void) {
    if (!s_brewing) return 0;
    uint32_t e = to_ms_since_boot(get_absolute_time()) - s_brew_start_ms;
    return e >= 10000u ? 100u : e / 100u;
}
static void brew_done(void) {
    s_brewing = false;
    fwog_ui_toast("Enjoy!");
    fwog_ui_pop();
}
static void brew_restart(void) {
    s_brew_start_ms = to_ms_since_boot(get_absolute_time());
}

static void play_draw(bool full) {
    if (full) st7789_fill_rect(0, 20, 320, 196, 0x0000);
    st7789_fill_rect((uint16_t)s_box_x, (uint16_t)s_box_y, 24, 24,
                     st7789_rgb565(46, 158, 79));
}
static void play_btn(fwog_btn_id_t b, bool long_press) {
    (void)long_press;
    if (b == FWOG_BTN_GRAY) s_dx = -2;
    if (b == FWOG_BTN_YELLOW) s_dx = 2;
    if (b == FWOG_BTN_GREEN) { s_dy = -s_dy; }
}

static void tick(uint32_t now) {
    if (fwog_ui_current() != SCR_PLAY) return; /* animate only when visible */
    if (now - s_last_ms < 33) return;          /* ~30 fps                   */
    s_last_ms = now;
    st7789_fill_rect((uint16_t)s_box_x, (uint16_t)s_box_y, 24, 24, 0x0000);
    s_box_x += s_dx;
    s_box_y += s_dy;
    if (s_box_x < 0) { s_box_x = 0; s_dx = 2; }
    if (s_box_x > 296) { s_box_x = 296; s_dx = -2; }
    if (s_box_y < 20) { s_box_y = 20; s_dy = 2; }
    if (s_box_y > 192) { s_box_y = 192; s_dy = -2; }
}

static void about(void) { fwog_ui_toast("DECK demo 001"); }

/* ---- Screens: const data, the whole UI ---- */
static const char *menu_rows[] = { "Brew coffee", "Settings", "Story", "Bounce" };
static void start_brew(void) {
    s_brewing = true;
    brew_restart();
    fwog_ui_push(SCR_BREW);
}
static void menu_pick(unsigned i) {
    if (i == 0) start_brew();
    else if (i == 1) fwog_ui_push(SCR_SET);
    else if (i == 2) fwog_ui_push(SCR_STORY);
    else fwog_ui_push(SCR_PLAY);
}

static const char *theme_names[] = { "Wili", "Ocean", "Ember" };
static const fwog_ui_field_t fields[] = {
    FWOG_FIELD_INT("Backlight", &s_bright, 10, 255, 15),
    FWOG_FIELD_BOOL("Sound", &s_sound),
    FWOG_FIELD_ENUM("Theme", &s_theme, theme_names, 3),
};

static const char *story_pages[] = {
    "DECK in five lines:\n\nRed always gets\nyou out.",
    "The bottom of the\nscreen tells you\nthe rest.\n\nGray and yellow\nare the rocker.",
    "Green says yes.\nBlue is the app's\nown verb.\n\nHold red: the\nSystem Card.",
};

static const fwog_ui_screen_t screens[] = {
    [SCR_MENU] = FWOG_UI_LIST("Home", menu_rows, 4, menu_pick,
                              .blue = { "COUNT", count_up }),
    [SCR_SET] = FWOG_UI_FORM("Settings", fields, 3, apply_settings),
    [SCR_STORY] = FWOG_UI_DETAIL("Story", story_pages, 3),
    [SCR_BREW] = FWOG_UI_PROGRESS("Brewing", brew_pct, brew_done, "Fresh pot incoming",
                                  .blue = { "AGAIN", brew_restart }),
    [SCR_PLAY] = FWOG_UI_CANVAS("Bounce", play_draw, play_btn),
};

int main(void) {
    board_init();
    fwog_ui_on_exit(apply_settings);
    fwog_ui_on_tick(tick);
    fwog_ui_menu_add("About", about);
    fwog_ui_run("DECK", screens, 5, SCR_MENU);
}
