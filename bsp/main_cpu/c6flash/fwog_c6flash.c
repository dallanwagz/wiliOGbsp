/* Reusable on-device C6 flasher -- see fwog_c6flash.h.
 *
 * Lifted from the venom app's proven c6flash.c and generalized: the UART and
 * EN/IO9 pins come from a config struct instead of #defines, and progress
 * goes to a status callback instead of the app's linkui. The esp-serial-
 * flasher port is otherwise unchanged -- it was already written on BSP
 * primitives (fwog_io_pin_drive for the FPGA-routed reset pins,
 * board_watchdog_kick inside every wait, fwog_uart_baud_ok to validate a
 * rate against the live clk_peri).
 */
#include "c6flash/fwog_c6flash.h"

#include "gpio/breakout.h"
#include "watchdog/watchdog.h"
#include "common/link/link_uart.h"

#include "hardware/clocks.h"
#include "pico/time.h"

#include "esp_loader.h"
#include "esp_loader_io.h"

#include <stdarg.h>
#include <stdio.h>

#define C6_SYNC_BAUD 115200u   /* stay here the whole flash (a mid-flight ROM
                                * baud bump broke the very next command in the
                                * field); ~700 KB at 115200 is ~60 s */

/* ===========================================================================
 * PORT LAYER -- esp_loader_port_ops_t on BSP primitives, cfg threaded through.
 * ========================================================================= */

typedef struct {
    esp_loader_port_t          port;      /* base; cast recovers this struct */
    uint32_t                   time_end;
    const fwog_c6flash_cfg_t  *cfg;
} c6_port_t;

static const fwog_c6flash_cfg_t *pcfg(esp_loader_port_t *p) {
    return ((c6_port_t *)p)->cfg;
}
static void en_drive(const fwog_c6flash_cfg_t *c, bool level)  { (void)fwog_io_pin_drive(c->en_gpio, level); }
static void io9_drive(const fwog_c6flash_cfg_t *c, bool level) { (void)fwog_io_pin_drive(c->io9_gpio, level); }

void fwog_c6flash_pins_idle(const fwog_c6flash_cfg_t *cfg) {
    io9_drive(cfg, true); en_drive(cfg, true);   /* released-high; C6 runs */
}

/* Classic esptool reset-into-download, driven by code not USB DTR/RTS: hold
 * IO9 low across the EN reset pulse so the ROM latches download mode when
 * reset releases, then let IO9 go. Timings are esp-serial-flasher's defaults. */
static void c6_enter_bootloader(esp_loader_port_t *p) {
    const fwog_c6flash_cfg_t *c = pcfg(p);
    io9_drive(c, false); en_drive(c, false);
    sleep_ms(100);
    en_drive(c, true);
    sleep_ms(50);
    io9_drive(c, true);
}

static void c6_reset_target(esp_loader_port_t *p) {
    const fwog_c6flash_cfg_t *c = pcfg(p);
    io9_drive(c, true);
    en_drive(c, false); sleep_ms(100); en_drive(c, true);
}

static esp_loader_error_t c6_write(esp_loader_port_t *p, const uint8_t *data,
                                   uint16_t size, uint32_t timeout) {
    uart_inst_t *u = pcfg(p)->uart;
    const uint32_t start = to_ms_since_boot(get_absolute_time());
    uint16_t pos = 0;
    while (pos < size) {
        if (to_ms_since_boot(get_absolute_time()) - start > timeout) break;
        if (uart_is_writable(u)) uart_putc_raw(u, data[pos++]);
        else board_watchdog_kick();
    }
    return (pos == size) ? ESP_LOADER_SUCCESS : ESP_LOADER_ERROR_TIMEOUT;
}

static esp_loader_error_t c6_read(esp_loader_port_t *p, uint8_t *data,
                                  uint16_t size, uint32_t timeout) {
    uart_inst_t *u = pcfg(p)->uart;
    const uint32_t start = to_ms_since_boot(get_absolute_time());
    uint16_t pos = 0;
    while (pos < size) {
        if (to_ms_since_boot(get_absolute_time()) - start > timeout) break;
        if (uart_is_readable(u)) data[pos++] = uart_getc(u);
        else board_watchdog_kick();     /* the long waits live here */
    }
    return (pos == size) ? ESP_LOADER_SUCCESS : ESP_LOADER_ERROR_TIMEOUT;
}

static void c6_delay_ms(esp_loader_port_t *p, uint32_t ms) {
    (void)p;
    while (ms > 0) { uint32_t s = ms > 200u ? 200u : ms; board_watchdog_kick(); sleep_ms(s); ms -= s; }
}

static void c6_start_timer(esp_loader_port_t *p, uint32_t ms) {
    ((c6_port_t *)p)->time_end = to_ms_since_boot(get_absolute_time()) + ms;
}

static uint32_t c6_remaining_time(esp_loader_port_t *p) {
    int32_t rem = (int32_t)(((c6_port_t *)p)->time_end - to_ms_since_boot(get_absolute_time()));
    return rem > 0 ? (uint32_t)rem : 0;
}

static esp_loader_error_t c6_change_rate(esp_loader_port_t *p, uint32_t rate) {
    if (!fwog_uart_baud_ok(clock_get_hz(clk_peri), rate)) return ESP_LOADER_ERROR_FAIL;
    uart_set_baudrate(pcfg(p)->uart, rate);
    return ESP_LOADER_SUCCESS;
}

static const esp_loader_port_ops_t c6_ops = {
    .init = NULL, .deinit = NULL,
    .enter_bootloader = c6_enter_bootloader,
    .reset_target = c6_reset_target,
    .start_timer = c6_start_timer,
    .remaining_time = c6_remaining_time,
    .delay_ms = c6_delay_ms,
    .log = NULL, .log_hex = NULL,   /* flasher chatter would corrupt the status stream */
    .change_transmission_rate = c6_change_rate,
    .write = c6_write, .read = c6_read,
};

/* ===========================================================================
 * DRIVER
 * ========================================================================= */

static void statusf(const fwog_c6flash_cfg_t *cfg, const char *fmt, ...) {
    if (!cfg->status) return;
    char line[48];
    va_list ap; va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap); va_end(ap);
    cfg->status(line);
}

static const char *const k_piece[3] = { "bootloader", "partition", "app" };

static bool flash_one(const fwog_c6flash_cfg_t *cfg, esp_loader_t *ld,
                      const fwog_c6_image_t *img, const char *name,
                      uint32_t *done, uint32_t total) {
    statusf(cfg, "S:C6img %s", name);
    esp_loader_flash_cfg_t fc = { .offset = img->offset, .image_size = img->size,
                                  .block_size = 1024, .skip_verify = false };
    esp_loader_error_t e;
    if ((e = esp_loader_flash_start(ld, &fc)) != ESP_LOADER_SUCCESS) {
        statusf(cfg, "S:C6 ERR erase %s rc%d", name, (int)e); return false;
    }
    uint32_t off = 0;
    while (off < img->size) {
        board_watchdog_kick();
        uint32_t n = img->size - off; if (n > fc.block_size) n = fc.block_size;
        if ((e = esp_loader_flash_write(ld, &fc, (void *)(img->data + off), n)) != ESP_LOADER_SUCCESS) {
            statusf(cfg, "S:C6 ERR write %s rc%d", name, (int)e); return false;
        }
        off += n; *done += n;
        statusf(cfg, "S:C6 %u %u %u", (unsigned)((uint64_t)*done * 100u / total),
                (unsigned)*done, (unsigned)total);
    }
    if ((e = esp_loader_flash_finish(ld, &fc)) != ESP_LOADER_SUCCESS) {
        statusf(cfg, "S:C6 ERR verify %s rc%d", name, (int)e); return false;
    }
    return true;
}

bool fwog_c6flash_run(const fwog_c6flash_cfg_t *cfg,
                      const fwog_c6_build_t *build, unsigned restore_baud) {
    uint32_t total = build->img[0].size + build->img[1].size + build->img[2].size;
    bool ok = false;

    statusf(cfg, "S:C6 flashing...");

    c6_port_t port = { .port = { .ops = &c6_ops }, .time_end = 0, .cfg = cfg };
    esp_loader_t loader;
    if (esp_loader_init_serial(&loader, &port.port) != ESP_LOADER_SUCCESS) {
        statusf(cfg, "S:C6 ERR init"); goto done;
    }
    uart_set_baudrate(cfg->uart, C6_SYNC_BAUD);

    /* Nudge a running C6 into download mode over the wire -- no BOOT/RESET
     * buttons. If it is running firmware with the DLMODE verb (see the C6
     * app), it sets its LP_AON force-download bit and software-resets into the
     * ROM download loader. Harmless to a C6 already hand-armed in download:
     * the ROM tolerates pre-sync bytes, and we flush before connecting. */
    statusf(cfg, "S:C6 arming...");
    static const char dl[] = "DLMODE\n";
    for (unsigned i = 0; i < sizeof dl - 1; i++) uart_putc_raw(cfg->uart, dl[i]);
    for (int i = 0; i < 12; i++) { board_watchdog_kick(); sleep_ms(100); }   /* let it reboot */
    while (uart_is_readable(cfg->uart)) (void)uart_getc(cfg->uart);          /* flush boot chatter */

    esp_loader_connect_args_t cargs = ESP_LOADER_CONNECT_DEFAULT();
    esp_loader_error_t ce = esp_loader_connect(&loader, &cargs);
    if (ce != ESP_LOADER_SUCCESS) {
        statusf(cfg, "S:C6 ERR connect rc%d (arm: hold BOOT+tap RESET)", (int)ce); goto done;
    }

    uint32_t written = 0;
    for (int i = 0; i < 3; i++)
        if (!flash_one(cfg, &loader, &build->img[i], k_piece[i], &written, total)) goto done;

    /* Clear the C6's persist force-download bit (LP_AON_SYS_CFG_REG bit 30)
     * so it boots the new app on its next reset instead of looping back into
     * download. A power/CHIP_PU reset would clear it too, but do not rely on
     * that. The app also clears it early in app_main as a second guarantee. */
    { uint32_t v;
      if (esp_loader_read_register(&loader, 0x600B1034u, &v) == ESP_LOADER_SUCCESS)
          (void)esp_loader_write_register(&loader, 0x600B1034u, v & ~(1u << 30)); }

    esp_loader_reset_target(&loader);   /* EN pulse; a no-op if EN is unwired */
    statusf(cfg, "S:C6 done");
    ok = true;

done:
    io9_drive(cfg, true); en_drive(cfg, true);
    uart_set_baudrate(cfg->uart, restore_baud);
    return ok;
}
