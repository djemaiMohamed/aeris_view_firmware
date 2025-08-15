/***************************************************************************//**
 * @file memlcd_app.c
 * @brief Memory Liquid Crystal Display (LCD) direct renderer (no full framebuffer)
 *******************************************************************************
 * # License
 * <b>Copyright 2020 Silicon Laboratories Inc. www.silabs.com</b>
 *******************************************************************************
 *
 * The licensor of this software is Silicon Laboratories Inc. Your use of this
 * software is governed by the terms of Silicon Labs Master Software License
 * Agreement (MSLA) available at
 * www.silabs.com/about-us/legal/master-software-license-agreement. This
 * software is distributed to you in Source Code format and is governed by the
 * sections of the MSLA applicable to Source Code.
 *
 ******************************************************************************/
#include <memlcd.h>
#include <stdio.h>
#include <string.h>

#include "sl_assert.h"
#include "sl_memlcd.h"
#include "sl_memlcd_display.h"
#include "sl_board_control.h"
// Keep access to font metadata/bitmaps only (no GLIB/DMD rendering APIs)
#include "glib.h"

//------------------------------------------------------------------------------
// Display geometry
#define LCD_WIDTH           400
#define LCD_HEIGHT          240
#define BYTES_PER_ROW       (LCD_WIDTH / 8)

#define CO2_HISTORY_SIZE    400
#define CO2_POINTS_RENDER_MAX  200

//------------------------------------------------------------------------------
// Local state
static const sl_memlcd_t *s_memlcd = NULL;
static uint8_t s_row[BYTES_PER_ROW]; // 400/8 = 50 bytes

// Current font pointer (read-only bitmaps in flash)
static const GLIB_Font_t *s_font = (const GLIB_Font_t *)&GLIB_FontNormal8x8;

//------------------------------------------------------------------------------
// Bit helpers
static inline void row_clear(uint8_t *row)
{
  memset(row, 0x00, BYTES_PER_ROW); // 0 = white background; 1-bits = black pixels
}

static inline void set_pixel(uint8_t *row, int x)
{
  if ((unsigned)x >= LCD_WIDTH) return;
  row[x >> 3] |= (uint8_t)(1u << (x & 0x7));
}

static inline void draw_hline(uint8_t *row, int x0, int x1)
{
  if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
  if (x1 < 0 || x0 >= LCD_WIDTH) return;
  if (x0 < 0) x0 = 0;
  if (x1 >= LCD_WIDTH) x1 = LCD_WIDTH - 1;
  for (int x = x0; x <= x1; ++x) set_pixel(row, x);
}

//------------------------------------------------------------------------------
// Font helpers (render only the parts that affect the current row)
static void set_font(const GLIB_Font_t *font)
{
  s_font = font;
}

// Map ASCII char to font index (subset needed)
static bool font_char_to_index(const GLIB_Font_t *font, char ch, uint16_t *out_idx)
{
  if (!font || !out_idx) return false;
  if (font->fontClass == NumbersOnlyFont) {
    if (ch >= '0' && ch <= '9') { *out_idx = (uint16_t)(ch - '0'); return true; }
    if (ch == ':') { *out_idx = 10; return true; }
    if (ch == ' ') { *out_idx = 11; return true; }
    return false;
  }
  if (ch < ' ' || ch > '~') return false;
  *out_idx = (uint16_t)(ch - ' ');
  return true;
}

// Draw a single character contribution into a row (y is row index on screen)
static void draw_char_row(uint8_t *row, const GLIB_Font_t *font,
                          char ch, int x0, int y0, uint8_t scale, int y)
{
  if (!font || scale == 0) return;
  // Check vertical bounds
  int char_h = font->fontHeight * scale;
  if (y < y0 || y >= y0 + char_h) return;

  uint16_t fontIdx;
  if (!font_char_to_index(font, ch, &fontIdx)) return;

  // Which source row(s) of the glyph affect this screen row?
  int rel_y = y - y0;                // 0..char_h-1
  int src_row = rel_y / (int)scale;  // 0..fontHeight-1

  // Font storage is interleaved by rows. Compute the element index:
  // element_index = glyph_index + src_row * fontRowOffset
  uint32_t row_bits = 0;
  uint32_t elem_index = (uint32_t)fontIdx + ((uint32_t)src_row * (uint32_t)font->fontRowOffset);
  switch (font->sizeOfMapElement) {
    case 1: {
      const uint8_t *p = (const uint8_t *)font->pFontPixMap;
      row_bits = (uint32_t)p[elem_index];
    } break;
    case 2: {
      const uint16_t *p = (const uint16_t *)font->pFontPixMap;
      row_bits = (uint32_t)p[elem_index];
    } break;
    default: {
      const uint32_t *p = (const uint32_t *)font->pFontPixMap;
      row_bits = p[elem_index];
    } break;
  }

  // Draw horizontally with scaling (LSB -> leftmost pixel)
  int fw = font->fontWidth;
  for (int sx = 0; sx < fw; ++sx) {
    if (row_bits & (1u << sx)) {
      int px0 = x0 + (sx * (int)scale);
      for (int k = 0; k < (int)scale; ++k) set_pixel(row, px0 + k);
    }
  }
}

static void draw_text_row(uint8_t *row, const GLIB_Font_t *font,
                          const char *str, int x, int y0, uint8_t scale, int y)
{
  if (!str || !font) return;
  int advance = (font->fontWidth + font->charSpacing) * (int)scale;
  for (const char *p = str; *p; ++p, x += advance) {
    draw_char_row(row, font, *p, x, y0, scale, y);
  }
}

static int text_width_px(const GLIB_Font_t *font, const char *str, uint8_t scale)
{
  if (!font || !str) return 0;
  size_t len = strlen(str);
  return (int)len * (font->fontWidth + font->charSpacing) * (int)scale;
}

static int centered_x(const GLIB_Font_t *font, const char *str, uint8_t scale)
{
  int w = text_width_px(font, str, scale);
  return (LCD_WIDTH - w) / 2;
}

//------------------------------------------------------------------------------
// Simple icons (row-by-row drawing)
static void draw_battery_row(uint8_t *row, int y, int x0, int y0, int level)
{
  // Outline 18x10, terminal 2x4 at right
  const int w = 18, h = 10;
  if (y < y0 || y >= y0 + h) return;
  int yy = y - y0;
  if (yy == 0 || yy == h - 1) {
    draw_hline(row, x0, x0 + w - 1);
  } else {
    set_pixel(row, x0);
    set_pixel(row, x0 + w - 1);
  }
  // terminal: centered right
  if (yy >= 3 && yy <= 6) {
    set_pixel(row, x0 + w);
    set_pixel(row, x0 + w + 1);
  }
  // level fill inside (white background -> draw a bar from left)
  int fill_w = (level * (w - 4)) / 100; // leave 2px margin each side
  if (fill_w > 0 && yy >= 2 && yy <= h - 3) {
    draw_hline(row, x0 + 2, x0 + 1 + 1 + fill_w); // a bit thicker visually
  }
}

static void draw_ble_row(uint8_t *row, int y, int x0, int y0, bool connected)
{
  // Simple antenna symbol + link dot if connected
  if (y < y0 || y > y0 + 10) return;
  int yy = y - y0;
  set_pixel(row, x0 + 4);
  if (yy <= 10) {
    // vertical spine
    set_pixel(row, x0 + 4);
  }
  // two diagonals roughly
  if (yy >= 0 && yy <= 5) set_pixel(row, x0 + 4 + (5 - yy) / 2);
  if (yy >= 5 && yy <= 10) set_pixel(row, x0 + 4 + (yy - 5) / 2);
  if (connected && yy == 12) {
    set_pixel(row, x0 + 4);
  }
}

static void draw_drop_row(uint8_t *row, int y, int x0, int y0)
{
  // Small water drop ~ 7x16
  if (y < y0 || y >= y0 + 16) return;
  int yy = y - y0;
  int half = 3;
  int radius = (yy < 8) ? yy / 2 : (15 - yy) / 2;
  draw_hline(row, x0 + half - radius, x0 + half + radius);
}

//------------------------------------------------------------------------------
// Layout constants
static const int TOP_BAR_Y = 4;
static const int LEFT_MARGIN = 8;

//------------------------------------------------------------------------------
// Direct display init/clear
static void init_display(void)
{
  sl_status_t st;
  st = sl_board_enable_display();
  EFM_ASSERT(st == SL_STATUS_OK);
  st = sl_memlcd_init();
  EFM_ASSERT(st == SL_STATUS_OK);
  s_memlcd = sl_memlcd_get();
  EFM_ASSERT(s_memlcd != NULL);

  // Clear to white
  for (int y = 0; y < LCD_HEIGHT; ++y) {
    row_clear(s_row);
    (void)sl_memlcd_draw(s_memlcd, s_row, (uint16_t)y, 1);
  }
}

//------------------------------------------------------------------------------
// One-frame renderer: compose each row and stream to LCD
static void render_frame(memlcd_handle_t *handle)
{
  // Cache values
  int co2 = (int)handle->co2_value;
  int hum = (int)handle->humidity_value;
  int batt = (int)handle->battery_level;
  bool bt = handle->bluetooth_connected != 0;

  // Text setup
  const GLIB_Font_t *small = (const GLIB_Font_t *)&GLIB_FontNarrow6x8;
  const GLIB_Font_t *num   = (const GLIB_Font_t *)&GLIB_FontNumber16x20;
  const GLIB_Font_t *mono8 = (const GLIB_Font_t *)&GLIB_FontNormal8x8;

  // Precompute strings and positions
  char co2_str[8]; snprintf(co2_str, sizeof(co2_str), "%d", co2);
  char hum_str[8]; snprintf(hum_str, sizeof(hum_str), "%d%%", hum);

  uint8_t co2_scale = 3; // 16x20 -> 48x60
  int co2_y = 70;
  int co2_x = centered_x(num, co2_str, co2_scale);

  int unit_y = co2_y + (num->fontHeight * co2_scale) + 8;
  const char *ppm = "ppm";
  int unit_x = centered_x(small, ppm, 2);

  // Optional date/interval at bottom
  const char *bottom_text = handle->date ? handle->date : "";
  int bottom_y = LCD_HEIGHT - (mono8->fontHeight * 1) - 4;
  int bottom_x = centered_x(mono8, bottom_text, 1);

  // Iterate rows
  for (int y = 0; y < LCD_HEIGHT; ++y) {
    row_clear(s_row);

    // Top bar: battery, BLE, labels
    draw_battery_row(s_row, y, LEFT_MARGIN, TOP_BAR_Y, batt);
    draw_ble_row(s_row, y, LEFT_MARGIN + 26, TOP_BAR_Y, bt);

    // Humidity icon + value on right
    draw_drop_row(s_row, y, LCD_WIDTH - 40, TOP_BAR_Y);
    set_font(small); draw_text_row(s_row, small, hum_str, LCD_WIDTH - 28, TOP_BAR_Y, 1, y);

    // Big CO2 value centered
    set_font(num);   draw_text_row(s_row, num, co2_str, co2_x, co2_y, co2_scale, y);
    // ppm unit
    set_font(small); draw_text_row(s_row, small, ppm, unit_x, unit_y, 2, y);

    // Bottom centered text (measurement interval / date)
    set_font(mono8); draw_text_row(s_row, mono8, bottom_text, bottom_x, bottom_y, 1, y);

    // Stream this row to LCD
    (void)sl_memlcd_draw(s_memlcd, s_row, (uint16_t)y, 1);
  }
}

//------------------------------------------------------------------------------
// Public API
void memlcd_draw_data(memlcd_handle_t *handle)
{
  if (!s_memlcd) init_display();
  if (!handle) return;
  render_frame(handle);
}

void memlcd_display_passkey(uint32_t passkey)
{
  if (!s_memlcd) init_display();
  char msg[] = "Enter on phone:";
  char key[7]; snprintf(key, sizeof(key), "%06lu", (unsigned long)(passkey % 1000000));

  const GLIB_Font_t *small = (const GLIB_Font_t *)&GLIB_FontNarrow6x8;
  const GLIB_Font_t *num   = (const GLIB_Font_t *)&GLIB_FontNumber16x20;

  uint8_t msg_scale = 2;
  uint8_t key_scale = 3;
  int total_h = (small->fontHeight * msg_scale) + 12 + (num->fontHeight * key_scale);
  int start_y = (LCD_HEIGHT - total_h) / 2;
  int msg_y = start_y;
  int key_y = msg_y + (small->fontHeight * msg_scale) + 12;
  int msg_x = centered_x(small, msg, msg_scale);
  int key_x = centered_x(num, key, key_scale);

  for (int y = 0; y < LCD_HEIGHT; ++y) {
    row_clear(s_row);
    draw_text_row(s_row, small, msg, msg_x, msg_y, msg_scale, y);
    draw_text_row(s_row, num,   key, key_x, key_y, key_scale, y);
    (void)sl_memlcd_draw(s_memlcd, s_row, (uint16_t)y, 1);
  }
}

void memlcd_display_waiting_message(void)
{
  if (!s_memlcd) init_display();
  const GLIB_Font_t *mono8 = (const GLIB_Font_t *)&GLIB_FontNormal8x8;
  const char *main_msg = "MEASURING...";
  const char *sub_msg  = "Please wait";
  uint8_t main_scale = 3; // 8x8 -> 24x24
  uint8_t sub_scale  = 2; // 8x8 -> 16x16
  int total_h = (mono8->fontHeight * main_scale) + 12 + (mono8->fontHeight * sub_scale);
  int start_y = (LCD_HEIGHT - total_h) / 2;
  int main_y  = start_y;
  int sub_y   = main_y + (mono8->fontHeight * main_scale) + 12;
  int main_x  = centered_x(mono8, main_msg, main_scale);
  int sub_x   = centered_x(mono8, sub_msg,  sub_scale);

  for (int y = 0; y < LCD_HEIGHT; ++y) {
    row_clear(s_row);
    draw_text_row(s_row, mono8, main_msg, main_x, main_y, main_scale, y);
    draw_text_row(s_row, mono8, sub_msg,  sub_x,  sub_y,  sub_scale,  y);
    (void)sl_memlcd_draw(s_memlcd, s_row, (uint16_t)y, 1);
  }
}

void memlcd_add_co2_to_history(memlcd_handle_t *handle, uint16_t co2_value)
{
  if (!handle) return;
  if (handle->history_count < CO2_HISTORY_SIZE) {
    handle->co2_history[handle->history_count++] = co2_value;
    handle->history_index = handle->history_count % CO2_HISTORY_SIZE;
  } else {
    handle->co2_history[handle->history_index] = co2_value;
    handle->history_index = (handle->history_index + 1) % CO2_HISTORY_SIZE;
  }
}
