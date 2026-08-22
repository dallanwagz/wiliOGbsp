#pragma once
/* Reusable on-device ESP32-C6 flasher for FreeWili OG apps.
 *
 * The OG's MAIN CPU programs an attached Bottlenose C6 over UART, host-free,
 * from images baked into MAIN's flash -- no laptop, no esptool. The ESP ROM
 * download protocol is Espressif's esp-serial-flasher (third_party/, v2.0.0);
 * this module supplies the USER_DEFINED port on BSP primitives (the C6's
 * EN/IO9 route through the FPGA io_buffer, which only fwog_io_pin_drive
 * reaches -- the stock pico port drives bare pads and cannot).
 *
 * This is an OPT-IN library (link fwog_c6flash IN ADDITION to fwog_main_bsp),
 * the same shape as fwog_main_fs. It knows nothing about any app's link
 * protocol: progress is a status(const char*) callback the app wires to its
 * own channel.
 *
 * The images are supplied by the app as a catalog baked with
 * fwog_embed_c6_catalog() (see tools/embed_c6_catalog.py). The C6 must be in
 * download mode when a flash starts -- a wired reset (en/io9 below) does it,
 * or the operator holds BOOT + taps RESET on the Bottlenose by hand.
 */
#include <stdbool.h>
#include <stdint.h>
#include "hardware/uart.h"

/* One flashable piece: bytes in MAIN flash + its C6 flash offset. */
typedef struct { const uint8_t *data; uint32_t size; uint32_t offset; } fwog_c6_image_t;

/* A build = the three pieces a C6 needs, in the fixed ESP32-C6 order
 * [0]=bootloader @0x0, [1]=partition-table @0x8000, [2]=app @0x10000.
 * The generated MAIN-side table is `fwog_c6_catalog[]` / `fwog_c6_catalog_count`. */
typedef struct { fwog_c6_image_t img[3]; } fwog_c6_build_t;

/* (The DISPLAY-side per-build metadata the chooser renders -- name /
 * description / "when to use" -- is fwog_c6_meta_t in the display header
 * ui/fwog_c6flash_ui.h, so the display CPU need not include this main-CPU
 * header. Both tables are generated from the one manifest, same order/count.) */

/* Where the C6 is wired, and where status text goes. */
typedef struct {
    uart_inst_t *uart;      /* the UART to the C6 (the app brings it up) */
    unsigned     en_gpio;   /* breakout GPIO -> C6 EN,       driven via fwog_io_pin_drive */
    unsigned     io9_gpio;  /* breakout GPIO -> C6 IO9/BOOT, driven via fwog_io_pin_drive */
    void (*status)(const char *line);   /* progress/result sink; may be NULL */
} fwog_c6flash_cfg_t;

/* Claim EN/IO9 as outputs released-high. Call once at boot: if those pins are
 * wired to the C6's reset/strap, an unclaimed pad would float its reset. */
void fwog_c6flash_pins_idle(const fwog_c6flash_cfg_t *cfg);

/* Flash `build`'s three images (MD5-verified), then reset the C6 to run.
 * Blocks ~a minute; kicks the watchdog throughout; restores the UART to
 * `restore_baud` on exit. Returns true on success. Emits, through
 * cfg->status: "S:C6img <piece>" per image, "S:C6 <pct> <done> <total>" per
 * block, then "S:C6 done" or "S:C6 ERR <stage> rc<n>". */
bool fwog_c6flash_run(const fwog_c6flash_cfg_t *cfg,
                      const fwog_c6_build_t *build, unsigned restore_baud);
