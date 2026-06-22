#pragma once

#include <stdint.h>
#include <stddef.h>

#define EPD_WIDTH       400
#define EPD_HEIGHT      600
#define EPD_BUFFER_SIZE (EPD_WIDTH * EPD_HEIGHT / 2)  /* 120000 bytes */

/* Spectra 6 valid color codes (4-bit). 0x4 and 0x7-0xF are reserved. */
#define EPD_BLACK   0x0
#define EPD_WHITE   0x1
#define EPD_YELLOW  0x2
#define EPD_RED     0x3
#define EPD_BLUE    0x5
#define EPD_GREEN   0x6

/* Bring up GPIO, SPI, and run the panel's init sequence. Blocking, ~1s. */
void epd_init(void);

/* Stream a packed panel buffer (2 px/byte, high nibble first) and refresh.
 * Blocks ~15-25s while the Spectra 6 cycles its color separation passes. */
void epd_display(const uint8_t *buf, size_t len);

/* Send the panel into deep sleep and cut PWR. The image persists with no power. */
void epd_sleep(void);

/* Render up to a few short centered lines of text and push them to the panel.
 * Self-contained: runs epd_init(), the refresh, and epd_sleep(). fg/bg are
 * EPD_* color codes. Text is auto-scaled to fit the 400 px width. Use for
 * status screens (e.g. "join WiFi build-1-XXXX"). Blocks ~15-25s. */
void epd_show_message(const char *const *lines, int n_lines, uint8_t fg, uint8_t bg);
