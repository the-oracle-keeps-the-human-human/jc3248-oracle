/*
 * hud_gfx.h — jc3248-pet-idf STEP 7: dependency-free RGB565 draw primitives.
 *
 * There is NO LovyanGFX / Adafruit_GFX in the ESP-IDF body. The HUD (pet name,
 * status line, connection dot) is drawn by hand-rolled blitters that write
 * host-order RGB565 directly into a caller-supplied framebuffer — the SAME
 * buffer display.c allocates and pushes (display_framebuffer() / display_flush()),
 * and the SAME host byte order display.c's test pattern uses (red == 0xF800).
 *
 * All functions take an explicit (fb, fb_w, fb_h) so this file knows nothing about
 * display.c's statics and can be unit-tested off-target against a malloc'd buffer.
 * Every primitive clips to [0,fb_w) x [0,fb_h); out-of-bounds pixels are dropped,
 * never wrapped — so a too-long string or an off-edge circle is always safe.
 *
 * Font: an embedded 8x8 bitmap covering printable ASCII 0x20..0x7E (see hud_gfx.c
 * for provenance). One glyph = 8 bytes, one byte per row, bit0 = leftmost column.
 * Scale is an integer nearest-neighbor multiplier (scale=2 -> 16x16 cell, etc.),
 * matching the Arduino HUD's setTextSize(2/3/4) feel.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Glyph cell metrics (pre-scale). The 8x8 font advances 8px/char with no gap;
 * callers that want spacing can add it to x between gfx_char() calls, but
 * gfx_text() uses the bare 8px advance to match the dense Arduino layout. */
#define GFX_FONT_W 8
#define GFX_FONT_H 8

/* A transparent background: pass as `bg` to gfx_char / gfx_text to skip the cell
 * fill and blit only the lit (foreground) pixels over whatever is already there
 * (e.g. text directly over the frog). Any real RGB565 value (incl. 0x0000 black)
 * is treated as opaque and fills the whole cell first. */
#define GFX_TRANSPARENT 0xFFFFFFFFu

/* Handy host-order RGB565 constants (match display.c's test-pattern values). */
#define GFX_BLACK   0x0000
#define GFX_WHITE   0xFFFF
#define GFX_RED     0xF800
#define GFX_GREEN   0x07E0
#define GFX_BLUE    0x001F
#define GFX_YELLOW  0xFFE0
#define GFX_CYAN    0x07FF
#define GFX_MAGENTA 0xF81F
#define GFX_ORANGE  0xFD20   /* pairing dot in the Arduino HUD */
#define GFX_GREY    0x8410   /* textDim default */

/* Pack 8-bit R,G,B into host-order RGB565 (same bit layout as display.c). */
static inline uint16_t gfx_rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

/* ── Pixel / rect / circle primitives ──────────────────────────────────────*/

/* One clipped pixel. */
void gfx_pixel(uint16_t *fb, int fb_w, int fb_h, int x, int y, uint16_t color);

/* Filled rectangle [x,x+w) x [y,y+h), clipped. */
void gfx_fill_rect(uint16_t *fb, int fb_w, int fb_h,
                   int x, int y, int w, int h, uint16_t color);

/* 1px rectangle outline (the four edges of [x,x+w) x [y,y+h)), clipped. */
void gfx_rect(uint16_t *fb, int fb_w, int fb_h,
              int x, int y, int w, int h, uint16_t color);

/* Horizontal / vertical fast lines (used for the HUD divider), clipped. */
void gfx_hline(uint16_t *fb, int fb_w, int fb_h, int x, int y, int w, uint16_t color);
void gfx_vline(uint16_t *fb, int fb_w, int fb_h, int x, int y, int h, uint16_t color);

/* Filled circle centered at (cx,cy), radius r — the connection dot. Clipped. */
void gfx_fill_circle(uint16_t *fb, int fb_w, int fb_h,
                     int cx, int cy, int r, uint16_t color);

/* ── Text ───────────────────────────────────────────────────────────────────*/

/*
 * Blit one character's 8x8 glyph at top-left (x,y), nearest-neighbor scaled by
 * `scale` (>=1). `fg` is the lit-pixel colour; `bg` fills the cell first unless
 * it is GFX_TRANSPARENT. Non-printable / out-of-range chars render as a space.
 * Returns the x advance (GFX_FONT_W * scale) so callers can chain manually.
 */
int gfx_char(uint16_t *fb, int fb_w, int fb_h,
             int x, int y, char ch, int scale, uint16_t fg, uint32_t bg);

/*
 * Blit a NUL-terminated string starting at (x,y), advancing 8*scale px per char.
 * '\n' moves to the next line (x reset to the original x, y += 8*scale). `bg`
 * may be GFX_TRANSPARENT. Returns the final x cursor after the last glyph.
 */
int gfx_text(uint16_t *fb, int fb_w, int fb_h,
             int x, int y, const char *s, int scale, uint16_t fg, uint32_t bg);

/* Pixel width a string would occupy at `scale` (8*scale per char, longest line
 * if it contains '\n'). Useful for right-aligning the connection dot / values. */
int gfx_text_width(const char *s, int scale);

#ifdef __cplusplus
}
#endif
