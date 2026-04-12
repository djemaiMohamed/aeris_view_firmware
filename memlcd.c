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
// Extern to know temperature unit (0=C, 1=F)
extern uint8_t temperature_unit;

//------------------------------------------------------------------------------
// Display geometry
#define LCD_WIDTH           400
#define LCD_HEIGHT          240
#define BYTES_PER_ROW       (LCD_WIDTH / 8)

#define CO2_HISTORY_SIZE    400
#define CO2_POINTS_RENDER_MAX  200

#define RIGHT_MARGIN        16

// CO2 quality thresholds (ppm)
#define CO2_EXCELLENT_MAX   800
#define CO2_GOOD_MAX        1200

//------------------------------------------------------------------------------
// Local state
static const sl_memlcd_t *s_memlcd = NULL;
static uint8_t s_row[BYTES_PER_ROW]; // 400
static const GLIB_Font_t *s_font = (const GLIB_Font_t *)&GLIB_FontNormal8x8;

//------------------------------------------------------------------------------
// Bit helpers
static inline void row_clear(uint8_t *row)
{
  // For this Memory LCD, a '1' bit corresponds to white. Clear to white.
  memset(row, 0xFF, BYTES_PER_ROW);
}

static inline void set_pixel(uint8_t *row, int x)
{
  if ((unsigned)x >= LCD_WIDTH) return;
  // Draw black pixel by clearing the corresponding bit to 0.
  row[x >> 3] &= (uint8_t)~(1u << (x & 0x7));
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

static void draw_text_row_advance(uint8_t *row, const GLIB_Font_t *font,
                                 const char *str, int x, int y0, uint8_t scale,
                                 int advance_px, int y)
{
  if (!str || !font) return;
  if (advance_px <= 0) {
    draw_text_row(row, font, str, x, y0, scale, y);
    return;
  }
  for (const char *p = str; *p; ++p, x += advance_px) {
    draw_char_row(row, font, *p, x, y0, scale, y);
  }
}

// Anisotropic glyph rendering (independent X/Y scaling) to keep digits slim
static void draw_char_row_aniso(uint8_t *row, const GLIB_Font_t *font,
                                char ch, int x0, int y0,
                                uint8_t scale_x, uint8_t scale_y, int y)
{
  if (!font || scale_x == 0 || scale_y == 0) return;
  int char_h = font->fontHeight * (int)scale_y;
  if (y < y0 || y >= y0 + char_h) return;

  uint16_t fontIdx;
  if (!font_char_to_index(font, ch, &fontIdx)) return;

  int rel_y = y - y0;
  int src_row = rel_y / (int)scale_y;

  uint32_t row_bits = 0;
  uint32_t elem_index = (uint32_t)fontIdx + ((uint32_t)src_row * (uint32_t)font->fontRowOffset);
  switch (font->sizeOfMapElement) {
    case 1: { const uint8_t  *p = (const uint8_t  *)font->pFontPixMap; row_bits = (uint32_t)p[elem_index]; } break;
    case 2: { const uint16_t *p = (const uint16_t *)font->pFontPixMap; row_bits = (uint32_t)p[elem_index]; } break;
    default: { const uint32_t *p = (const uint32_t *)font->pFontPixMap; row_bits = p[elem_index]; } break;
  }

  int fw = font->fontWidth;
  for (int sx = 0; sx < fw; ++sx) {
    if (row_bits & (1u << sx)) {
      int px0 = x0 + (sx * (int)scale_x);
      for (int k = 0; k < (int)scale_x; ++k) set_pixel(row, px0 + k);
    }
  }
}

static void draw_text_row_aniso(uint8_t *row, const GLIB_Font_t *font,
                                const char *str, int x, int y0,
                                uint8_t scale_x, uint8_t scale_y, int y)
{
  if (!str || !font) return;
  int advance = (font->fontWidth + font->charSpacing) * (int)scale_x;
  for (const char *p = str; *p; ++p, x += advance) {
    draw_char_row_aniso(row, font, *p, x, y0, scale_x, scale_y, y);
  }
}

static int text_width_px_aniso(const GLIB_Font_t *font, const char *str, uint8_t scale_x)
{
  if (!font || !str) return 0;
  size_t len = strlen(str);
  return (int)len * (font->fontWidth + font->charSpacing) * (int)scale_x;
}

static int text_width_px(const GLIB_Font_t *font, const char *str, uint8_t scale)
{
  if (!font || !str) return 0;
  size_t len = strlen(str);
  return (int)len * (font->fontWidth + font->charSpacing) * (int)scale;
}

static int text_width_px_advance(const GLIB_Font_t *font, const char *str, uint8_t scale, int advance_px)
{
  if (!font || !str) return 0;
  size_t len = strlen(str);
  if (len == 0) return 0;
  int glyph_w = font->fontWidth * (int)scale;
  if (advance_px <= 0) {
    int adv = (font->fontWidth + font->charSpacing) * (int)scale;
    return (int)((len - 1) * (size_t)adv + (size_t)glyph_w);
  }
  return (int)((len - 1) * (size_t)advance_px + (size_t)glyph_w);
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
  // Outline 22x12, terminal 2x6 at right
  const int w = 22, h = 12;
  if (y < y0 || y >= y0 + h) return;
  int yy = y - y0;
  if (yy == 0 || yy == h - 1) {
    draw_hline(row, x0, x0 + w - 1);
  } else {
    set_pixel(row, x0);
    set_pixel(row, x0 + w - 1);
  }
  // terminal: centered right
  if (yy >= 3 && yy <= 8) {
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

// Simple thermometer icon (~8x16)
static void draw_thermo_row(uint8_t *row, int y, int x0, int y0)
{
  if (y < y0 || y >= y0 + 16) return;
  int yy = y - y0;
  int bulb_r = 3; // radius ~3
  int stem_x = x0 + 4; // center-ish
  int bulb_y = 12; // center of bulb
  // Stem
  if (yy <= bulb_y) {
    set_pixel(row, stem_x);
    set_pixel(row, stem_x - 1);
  }
  // Bulb (filled circle approx)
  int dy = yy - bulb_y;
  int dx = 0;
  int r = bulb_r;
  int rr = r - (dy < 0 ? -dy : dy);
  if (rr >= 0) {
    int half = rr;
    draw_hline(row, stem_x - half, stem_x + half);
  }
}

//------------------------------------------------------------------------------
// Layout constants
static const int TOP_BAR_Y = 8;
static const int LEFT_MARGIN = 16;
static const int BOTTOM_MARGIN = 0; // no bottom margin for chart
static const int V_GAP = 8; // vertical spacing between sections
static const int CHART_HEIGHT = 108; // taller chart height

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
// Simple util
static inline int right_aligned_x(const GLIB_Font_t *font, const char *str, uint8_t scale, int right_margin)
{
  return LCD_WIDTH - right_margin - text_width_px(font, str, scale);
}

static inline bool grey_mask(int x, int y)
{
  // 2x2 checker
  return ((x ^ y) & 1) == 1;
}

//------------------------------------------------------------------------------
// Emoji drawing functions
// Draw a happy emoji (smiley face) centered at (x0, y0) with given size (width)
static void draw_happy_emoji_row(uint8_t *row, int y, int x0, int y0, int size)
{
  if (y < y0 || y >= y0 + size) return;
  
  int rel_y = y - y0;
  int center_x = x0 + size / 2;
  int center_y = y0 + size / 2;
  int radius = size / 2;

  int outline_thick = size / 18 + 2;
  if (outline_thick < 2) outline_thick = 2;
  if (outline_thick > 6) outline_thick = 6;
  int inner_r = radius - outline_thick;
  if (inner_r < 0) inner_r = 0;
  
  // Head circle (draw outline)
  for (int x = x0; x < x0 + size; ++x) {
    int dx = x - center_x;
    int dy = rel_y - (center_y - y0);
    int dist_sq = dx * dx + dy * dy;
    
    // Head outline (hollow circle)
    if (dist_sq >= inner_r * inner_r && dist_sq <= radius * radius) {
      set_pixel(row, x);
    }
  }
  
  // Eyes (filled circles) + tiny highlight for expression
  int left_eye_x = center_x - radius / 3;
  int left_eye_y = center_y - radius / 5;
  int eye_size = size / 14 + 3;
  if (eye_size < 3) eye_size = 3;
  for (int x = x0; x < x0 + size; ++x) {
    int dx = x - left_eye_x;
    int dy = rel_y - (left_eye_y - y0);
    int dist_sq = dx * dx + dy * dy;
    if (dist_sq <= eye_size * eye_size) {
      set_pixel(row, x);
    }
  }
  // Note: no white "highlight" pixel; set_pixel() only draws black.
  
  // Right eye (larger filled circle)
  int right_eye_x = center_x + radius / 3;
  int right_eye_y = center_y - radius / 5;
  for (int x = x0; x < x0 + size; ++x) {
    int dx = x - right_eye_x;
    int dy = rel_y - (right_eye_y - y0);
    int dist_sq = dx * dx + dy * dy;
    if (dist_sq <= eye_size * eye_size) {
      set_pixel(row, x);
    }
  }
  
  // Eyebrows (slight arch) - a bit thicker
  int brow_left_x = center_x - radius / 3 - 3;
  int brow_left_y = center_y - radius / 3;
  for (int x = brow_left_x; x <= brow_left_x + 5; ++x) {
    if (x >= x0 && x < x0 + size) {
      int offset = (x - brow_left_x) / 2;
      int arch = (x - (brow_left_x + 2));
      if (arch < 0) arch = -arch;
      int brow_y = brow_left_y - offset - (arch / 3);
      int brow_rel_y = brow_y - y0;
      if (rel_y == brow_rel_y || rel_y == brow_rel_y + 1) {
        set_pixel(row, x);
      }
    }
  }
  
  // Right eyebrow (diagonal line)
  int brow_right_x = center_x + radius / 3 - 2;
  int brow_right_y = center_y - radius / 3;
  for (int x = brow_right_x; x <= brow_right_x + 5; ++x) {
    if (x >= x0 && x < x0 + size) {
      int offset = (x - brow_right_x) / 2;
      int arch = (x - (brow_right_x + 2));
      if (arch < 0) arch = -arch;
      int brow_y = brow_right_y - offset - (arch / 3);
      int brow_rel_y = brow_y - y0;
      if (rel_y == brow_rel_y || rel_y == brow_rel_y + 1) {
        set_pixel(row, x);
      }
    }
  }
  
  // Cheeks (subtle) - small dots
  int cheek_y_rel = (center_y + radius / 4) - y0;
  if (rel_y == cheek_y_rel || rel_y == cheek_y_rel + 1) {
    int cheek_dx = radius / 2;
    if (cheek_dx < 6) cheek_dx = 6;
    set_pixel(row, center_x - cheek_dx);
    set_pixel(row, center_x - cheek_dx - 1);
    set_pixel(row, center_x + cheek_dx);
    set_pixel(row, center_x + cheek_dx + 1);
  }

  // Smile: thin upward-curving arc (corners up, center down)
  // Make the smile shorter/narrower and a bit less deep for a cuter expression.
  int mouth_half_w = (radius * 2) / 5;
  if (mouth_half_w < 4) mouth_half_w = 4;
  int mouth_depth = radius / 7 + 1;
  if (mouth_depth < 2) mouth_depth = 2;
  int mouth_y_base = center_y + radius / 3;

  int xL = center_x - mouth_half_w;
  int xR = center_x + mouth_half_w;
  if (xL < x0) xL = x0;
  if (xR > x0 + size - 1) xR = x0 + size - 1;

  int denom = mouth_half_w * mouth_half_w;
  for (int x = xL; x <= xR; ++x) {
    int dx = x - center_x;
    int y_curve = mouth_y_base + mouth_depth - (dx * dx * mouth_depth) / (denom ? denom : 1);
    int y_curve_rel = y_curve - y0;
    if (rel_y == y_curve_rel || rel_y == y_curve_rel + 1) {
      set_pixel(row, x);
      set_pixel(row, x - 1);
      set_pixel(row, x + 1);
    }
  }
}

// Draw a sad emoji (sad face) centered at (x0, y0) with given size (width)
static void draw_sad_emoji_row(uint8_t *row, int y, int x0, int y0, int size)
{
  if (y < y0 || y >= y0 + size) return;
  
  int rel_y = y - y0;
  int center_x = x0 + size / 2;
  int center_y = y0 + size / 2;
  int radius = size / 2;

  int outline_thick = size / 18 + 2;
  if (outline_thick < 2) outline_thick = 2;
  if (outline_thick > 6) outline_thick = 6;
  int inner_r = radius - outline_thick;
  if (inner_r < 0) inner_r = 0;
  
  // Head circle (draw outline)
  for (int x = x0; x < x0 + size; ++x) {
    int dx = x - center_x;
    int dy = rel_y - (center_y - y0);
    int dist_sq = dx * dx + dy * dy;
    
    // Head outline (hollow circle)
    if (dist_sq >= inner_r * inner_r && dist_sq <= radius * radius) {
      set_pixel(row, x);
    }
  }
  
  // Eyes (filled circles) + tiny highlight
  int left_eye_x = center_x - radius / 3;
  int left_eye_y = center_y - radius / 5;
  int eye_size = size / 14 + 3;
  if (eye_size < 3) eye_size = 3;
  for (int x = x0; x < x0 + size; ++x) {
    int dx = x - left_eye_x;
    int dy = rel_y - (left_eye_y - y0);
    int dist_sq = dx * dx + dy * dy;
    if (dist_sq <= eye_size * eye_size) {
      set_pixel(row, x);
    }
  }
  // Note: no white "highlight" pixel; set_pixel() only draws black.
  
  // Right eye (larger filled circle)
  int right_eye_x = center_x + radius / 3;
  int right_eye_y = center_y - radius / 5;
  for (int x = x0; x < x0 + size; ++x) {
    int dx = x - right_eye_x;
    int dy = rel_y - (right_eye_y - y0);
    int dist_sq = dx * dx + dy * dy;
    if (dist_sq <= eye_size * eye_size) {
      set_pixel(row, x);
    }
  }
  
  // Left eyebrow (diagonal line - sad)
  int brow_left_x = center_x - radius / 3 - 3;
  int brow_left_y = center_y - radius / 3;
  for (int x = brow_left_x; x <= brow_left_x + 5; ++x) {
    if (x >= x0 && x < x0 + size) {
      int offset = (x - brow_left_x) / 2;
      int brow_y = brow_left_y + offset;
      int brow_rel_y = brow_y - y0;
      if (rel_y == brow_rel_y || rel_y == brow_rel_y + 1) {
        set_pixel(row, x);
      }
    }
  }
  
  // Right eyebrow (diagonal line - sad)
  int brow_right_x = center_x + radius / 3 - 2;
  int brow_right_y = center_y - radius / 3 + 3;
  for (int x = brow_right_x; x <= brow_right_x + 5; ++x) {
    if (x >= x0 && x < x0 + size) {
      int offset = (brow_right_x + 5 - x) / 2;
      int brow_y = brow_right_y - offset;
      int brow_rel_y = brow_y - y0;
      if (rel_y == brow_rel_y || rel_y == brow_rel_y + 1) {
        set_pixel(row, x);
      }
    }
  }
  
  // Tear (subtle) under one eye
  int tear_x = left_eye_x - 1;
  int tear_y0_rel = (left_eye_y - y0) + eye_size + 1;
  if (rel_y == tear_y0_rel) {
    set_pixel(row, tear_x);
  } else if (rel_y == tear_y0_rel + 1) {
    set_pixel(row, tear_x);
    set_pixel(row, tear_x - 1);
  } else if (rel_y == tear_y0_rel + 2) {
    set_pixel(row, tear_x);
  }

  // Frown: thick downward-curving arc (corners down, center up)
  int mouth_half_w = radius / 2;
  if (mouth_half_w < 4) mouth_half_w = 4;
  int mouth_depth = radius / 5 + 2;
  if (mouth_depth < 3) mouth_depth = 3;
  int mouth_y_base = center_y + radius / 3;

  int xL = center_x - mouth_half_w;
  int xR = center_x + mouth_half_w;
  if (xL < x0) xL = x0;
  if (xR > x0 + size - 1) xR = x0 + size - 1;

  int denom = mouth_half_w * mouth_half_w;
  for (int x = xL; x <= xR; ++x) {
    int dx = x - center_x;
    int y_curve = mouth_y_base - mouth_depth + (dx * dx * mouth_depth) / (denom ? denom : 1);
    int y_curve_rel = y_curve - y0;
    if (rel_y == y_curve_rel || rel_y == y_curve_rel + 1 || rel_y == y_curve_rel + 2) {
      set_pixel(row, x);
      set_pixel(row, x - 1);
      set_pixel(row, x + 1);
    }
  }
}

// Map CO2 ppm to qualitative label
static const char *co2_quality_text(int co2)
{
  if (co2 <= CO2_EXCELLENT_MAX) return "Excellent";
  if (co2 <= CO2_GOOD_MAX) return "Good";
  return "Poor";
}

// White→Black ordered-dither gradient bar with a vertical marker for current CO2
static void draw_progress_bar_row(uint8_t *row, int y, int y0, int height, int co2)
{
  if (y < y0 || y >= y0 + height) return;

  const int x0 = LEFT_MARGIN;
  const int x1 = LCD_WIDTH - RIGHT_MARGIN - 1;
  const int w = x1 - x0 + 1;

  // 4x4 Bayer matrix (values 0..15)
  static const uint8_t bayer4[4][4] = {
    { 0,  8,  2, 10},
    {12,  4, 14,  6},
    { 3, 11,  1,  9},
    {15,  7, 13,  5}
  };

  // Gradient background left (white) → right (black)
  for (int x = x0; x <= x1; ++x) {
    int xrel = x - x0;
    int level = (xrel * 16) / (w > 0 ? w : 1); // 0..15
    if (level > bayer4[y & 3][x & 3]) {
      set_pixel(row, x);
    }
  }

  // Marker position from 400 ppm (left) to 5000 ppm (right)
  const int co2_min = 400;
  const int co2_max = 5000;
  int c = co2;
  if (c < co2_min) c = co2_min;
  if (c > co2_max) c = co2_max;
  int marker = x0 + (int)(((long)(c - co2_min) * (w - 1)) / (long)(co2_max - co2_min));
  // 3-pixel vertical marker
  set_pixel(row, marker);
  if (marker - 1 >= x0) set_pixel(row, marker - 1);
  if (marker + 1 <= x1) set_pixel(row, marker + 1);
}

// Draw grey-filled area chart for CO2 history (left-to-right growth), full width with borders
static void draw_chart_row(uint8_t *row,
                           int y,
                           int chart_top,
                           int chart_bottom,
                           const memlcd_handle_t *handle,
                           uint16_t max_value)
{
  if (y < chart_top || y > chart_bottom) return;

  const int baseline_y = chart_bottom;
  const int chart_h = chart_bottom - chart_top + 1;

  // Top and bottom borders
  if (y == chart_top || y == baseline_y) {
    draw_hline(row, 0, LCD_WIDTH - 1);
    return;
  }

  const int x_start = 0;
  const int x_end = LCD_WIDTH - 1;
  const int plot_w = x_end - x_start + 1;

  int total_in_buffer = (handle->history_count < CO2_HISTORY_SIZE) ? handle->history_count : CO2_HISTORY_SIZE;
  if (total_in_buffer == 0 || max_value == 0) return;

  int oldest_logical = handle->history_count - total_in_buffer;
  int oldest_buf_idx = (handle->history_count < CO2_HISTORY_SIZE) ? 0 : handle->history_index;
  int samples_to_draw = (handle->history_count < plot_w) ? handle->history_count : plot_w;
  int window_oldest_logical = handle->history_count - samples_to_draw;
  int delta_from_oldest = window_oldest_logical - oldest_logical;
  int window_oldest_buf_idx = (oldest_buf_idx + ((delta_from_oldest % CO2_HISTORY_SIZE) + CO2_HISTORY_SIZE) % CO2_HISTORY_SIZE) % CO2_HISTORY_SIZE;

  int prev_y_plot = 0;
  bool prev_valid = false;

  for (int x = x_start; x <= x_end; ++x) {
    int offset = x - x_start;
    if (offset >= samples_to_draw) break; // no data beyond available samples
    int buf_idx = (window_oldest_buf_idx + offset) % CO2_HISTORY_SIZE;
    uint16_t value = handle->co2_history[buf_idx];

    // Map value to y (0 at top increasing downward)
    int h = chart_h - 1;
    int rise = (int)((value * (long)h) / (long)max_value);
    int y_plot = baseline_y - rise;
    if (y_plot <= chart_top + 1) y_plot = chart_top + 1; // keep inside borders

    // Fill area under curve with checker dither
    if (y >= y_plot && y < baseline_y) {
      if (grey_mask(x, y)) {
        set_pixel(row, x);
      }
    }

    // Bold, continuous top outline: connect to previous sample to avoid gaps
    // when the curve jumps by >1px between adjacent x columns.
    if (prev_valid) {
      int y_min = (prev_y_plot < y_plot) ? prev_y_plot : y_plot;
      int y_max = (prev_y_plot > y_plot) ? prev_y_plot : y_plot;

      // 1px stroke
      if (y >= y_min && y <= y_max) {
        set_pixel(row, x);
      }
      // 2nd stroke pixel (one row above) to make it bold toward the top
      if (y >= (y_min - 1) && y <= (y_max - 1) && y >= chart_top + 1) {
        set_pixel(row, x);
      }
    } else {
      // First column: just draw the point (bold)
      if (y == y_plot || (y == y_plot - 1 && y >= chart_top + 1)) {
        set_pixel(row, x);
      }
    }

    prev_y_plot = y_plot;
    prev_valid = true;
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
  (void)bt; // not displayed in this layout

  // Fonts
  const GLIB_Font_t *small = (const GLIB_Font_t *)&GLIB_FontNarrow6x8;
  const GLIB_Font_t *status_font = (const GLIB_Font_t *)&GLIB_FontNormal8x8;
  const GLIB_Font_t *num   = (const GLIB_Font_t *)&GLIB_FontNumber16x20;

  // Strings
  const char *interval = handle->date ? handle->date : ""; // measurement interval
  char batt_str[6]; snprintf(batt_str, sizeof(batt_str), "%d%%", batt);
  char co2_str[8]; snprintf(co2_str, sizeof(co2_str), "%d", co2);
  char hum_str[6]; snprintf(hum_str, sizeof(hum_str), "%d", hum);

  int temp_whole = (int)(handle->temperature_value >= 0.0f
                         ? handle->temperature_value + 0.5f
                         : handle->temperature_value - 0.5f);
  char temp_str[12]; snprintf(temp_str, sizeof(temp_str), "%d", temp_whole);
  const char *t_unit = (temperature_unit == 0) ? "C" : "F";
  const char *co2_unit = "ppm";
  const char *hum_unit = "%";

  // Layout metrics
  uint8_t top_scale = 3;                         // make interval + battery a little bigger
  int top_bar_h = small->fontHeight * top_scale;
  int top_bar_bottom = TOP_BAR_Y + top_bar_h;

  // CO2 big number size (fixed scale so it does not change with value)
  const uint8_t co2_scale = 4; // bigger for readability from far

  int co2_h = num->fontHeight * co2_scale;
  // Pull CO2 upward to use the empty space under the top bar.
  // Everything below (ppm, status word/emoji, chart) stays correctly padded
  // because it is derived from co2_y/co2_h.
  int co2_y = top_bar_bottom + 2;                   // small gap below top bar
  int baseline_y = co2_y + co2_h - 1;               // baseline for CO2 number
  int co2_center_y = co2_y + (co2_h / 2);

  // Temperature/Humidity number scale and baseline alignment (slim strokes)
  uint8_t th_num_scale_x = 1;                        // keep digits slim
  uint8_t th_num_scale_y = 2;                        // slightly taller
  uint8_t unit_scale = 2;                            // unit size
  const int th_bold_px = 1;                           // embolden by overdrawing 1px offset
  int th_num_h = num->fontHeight * th_num_scale_y;
  int unit_h = small->fontHeight * unit_scale;
  // Center temp/hum vertically to the CO2 number midline (looks higher and aligned)
  int th_y = co2_center_y - (th_num_h / 2);
  int unit_y = co2_center_y - (unit_h / 2);
  int min_th_y = top_bar_bottom + 1;
  if (th_y < min_th_y) th_y = min_th_y;
  if (unit_y < min_th_y) unit_y = min_th_y;

  // CO2 unit below the CO2 number (fixed placement)
  uint8_t co2_unit_scale = 2;                       // enlarged CO2 unit (ppm)
  int co2_unit_h = small->fontHeight * co2_unit_scale;
  int co2_num_w = text_width_px(num, co2_str, co2_scale);
    // Place ppm above the CO2 number to free space for the chart
    int co2_unit_y = co2_y - co2_unit_h - 2;
    if (co2_unit_y < 0) co2_unit_y = 0;
  int co2_unit_x = (co2_num_w > 0)
                     ? (centered_x(num, co2_str, co2_scale)
                        + (co2_num_w - text_width_px(small, co2_unit, co2_unit_scale)) / 2)
                     : centered_x(small, co2_unit, co2_unit_scale);

  // Battery icon placement (top-right)
  const int batt_icon_w = 24; // 22 body + 2 terminal
  const int batt_icon_h = 12;
  int batt_icon_x = LCD_WIDTH - RIGHT_MARGIN - batt_icon_w;
  int batt_icon_y0 = TOP_BAR_Y + (top_bar_h - batt_icon_h) / 2;
  int batt_text_x = batt_icon_x - 4 - text_width_px(small, batt_str, top_scale);

  // Build qualitative message (mixed-case looks nicer at larger scales)
  char qual_msg[16];
  snprintf(qual_msg, sizeof(qual_msg), "%s", co2_quality_text(co2));
  // Keep overall status block size the same, but render the word slightly smaller.
  // This avoids moving other UI components (chart/etc) while shrinking only the word.
  const uint8_t qual_scale_block = 3;
  const uint8_t qual_scale_text  = 2;
  const int qual_pad_top = 4;                      // slightly tighter to make room for chart

  // Max label will be bigger; set its scale now and derive bottom padding
  uint8_t max_scale = 2;                           // bigger max value label
  int max_h = small->fontHeight * max_scale;
  int qual_pad_bottom = max_h + 2;                 // ensure space above chart for label

  // Status block sits below the CO2 number (no longer below the ppm label)
  int qual_y = co2_y + co2_h + qual_pad_top;
  
  // Emoji positioning: draw emoji to the left of status text
  // Emoji size is 2x the status text height
  int emoji_size = status_font->fontHeight * qual_scale_block * 2;

  // Make status word more compact by reducing per-letter spacing (height unchanged)
  const int status_gap = 8;
  int default_advance = (status_font->fontWidth + status_font->charSpacing) * (int)qual_scale_text;
  // Keep letters compact but never overlapping (overlap can look like "bold")
  int tighten_px = (status_font->charSpacing * (int)qual_scale_text); // remove the built-in inter-letter spacing
  int min_advance = (status_font->fontWidth * (int)qual_scale_text);  // no overlap
  int qual_advance = default_advance - tighten_px;
  if (qual_advance < min_advance) qual_advance = min_advance;

  int text_width = text_width_px_advance(status_font, qual_msg, qual_scale_text, qual_advance);
  // Center the status word with the CO2 value (screen center)
  int qual_x = (LCD_WIDTH - text_width) / 2;
  int emoji_x = qual_x - status_gap - emoji_size;

  // Ensure the emoji never overlaps the CO2 digits horizontally.
  // (It can extend upward into the CO2 vertical region; keeping it left avoids pixel overlap.)
  int co2_x = centered_x(num, co2_str, co2_scale);
  int co2_left = co2_x - 2;
  if (emoji_x + emoji_size > co2_left) {
    emoji_x = co2_left - emoji_size;
  }
  if (emoji_x < 0) emoji_x = 0;
  
  // Align emoji vertically with status text
  int block_text_h = status_font->fontHeight * (int)qual_scale_block;
  int text_height  = status_font->fontHeight * (int)qual_scale_text;
  int text_y       = qual_y + (block_text_h - text_height) / 2;
  // Center the overall status block within the larger emoji
  int emoji_y      = qual_y - (emoji_size - block_text_h) / 2;

  // Chart area: fixed height so it doesn't change
  int chart_top = qual_y + block_text_h + qual_pad_bottom;
  int chart_bottom = chart_top + CHART_HEIGHT - 1;
  int max_bottom = LCD_HEIGHT - 1 - BOTTOM_MARGIN;
  if (chart_bottom > max_bottom) chart_bottom = max_bottom;

  // Precompute history scaling
  uint16_t max_value = 0;
  if (handle->history_count > 0) {
    int total = handle->history_count < CO2_HISTORY_SIZE ? handle->history_count : CO2_HISTORY_SIZE;
    for (int i = 0; i < total; ++i) {
      uint16_t v = handle->co2_history[i];
      if (v > max_value) max_value = v;
    }
    if (max_value == 0) max_value = 1000;
  }

  // Prepare max label just above the chart (right-aligned)
  char max_str[8];
  snprintf(max_str, sizeof(max_str), "%u", (unsigned)max_value);
  int max_y = chart_top - max_h - 2; // 2px gap above border
  if (max_y < 0) max_y = 0;
  int max_x = LCD_WIDTH - RIGHT_MARGIN - text_width_px(small, max_str, max_scale);

  // History window label (centered, just above chart top border)
  char hist_str[20] = "";
  const GLIB_Font_t *hist_font = status_font; // 8x8 at scale 1 ~= "1.5" of Narrow6x8 scale 1
  uint8_t hist_scale = 1;
  if (handle->measurement_interval_s != 0 && handle->history_count != 0) {
    uint32_t samples = (handle->history_count < CO2_HISTORY_SIZE) ? handle->history_count : CO2_HISTORY_SIZE;
    uint32_t window_s = samples * (uint32_t)handle->measurement_interval_s;
    if (window_s >= 3600u) {
      uint32_t hours = (window_s + 1800u) / 3600u; // round to nearest hour
      if (hours == 0) hours = 1;
      snprintf(hist_str, sizeof(hist_str), "%luh history", (unsigned long)hours);
    } else {
      uint32_t minutes = (window_s + 30u) / 60u; // round to nearest minute
      if (minutes == 0) minutes = 1;
      snprintf(hist_str, sizeof(hist_str), "%lum history", (unsigned long)minutes);
    }
  }
  int hist_x = 0;
  int hist_h = hist_font->fontHeight * (int)hist_scale;
  int hist_y = chart_top - hist_h - 1; // sit close to the chart top border
  if (hist_y < 0) hist_y = 0;
  if (hist_str[0] != '\0') {
    int hist_w = text_width_px(hist_font, hist_str, hist_scale);
    hist_x = centered_x(hist_font, hist_str, hist_scale);
    if (hist_x < LEFT_MARGIN) hist_x = LEFT_MARGIN;
    if (hist_x + hist_w > (LCD_WIDTH - RIGHT_MARGIN)) {
      hist_x = (LCD_WIDTH - RIGHT_MARGIN) - hist_w;
    }
  }

  // Iterate rows
  for (int y = 0; y < LCD_HEIGHT; ++y) {
    row_clear(s_row);

    // Top bar
    draw_text_row(s_row, small, interval, LEFT_MARGIN, TOP_BAR_Y, top_scale, y);
    draw_text_row(s_row, small, batt_str, batt_text_x, TOP_BAR_Y, top_scale, y);
    draw_battery_row(s_row, y, batt_icon_x, batt_icon_y0, batt);

    // Temperature (left) with unit, baseline-aligned (anisotropic digits)
    int temp_x = LEFT_MARGIN;
    int temp_unit_x = temp_x + text_width_px_aniso(num, temp_str, th_num_scale_x) + 4 + th_bold_px;
    draw_text_row_aniso(s_row, num, temp_str, temp_x,             th_y, th_num_scale_x, th_num_scale_y, y);
    draw_text_row_aniso(s_row, num, temp_str, temp_x + th_bold_px, th_y, th_num_scale_x, th_num_scale_y, y);
    draw_text_row(s_row, small, t_unit, temp_unit_x, unit_y, unit_scale, y);

    // CO2 centered, extra bold stays the same
    int co2_x = centered_x(num, co2_str, co2_scale);
    draw_text_row(s_row, num, co2_str, co2_x - 1, co2_y, co2_scale, y);
    draw_text_row(s_row, num, co2_str, co2_x,     co2_y, co2_scale, y);
    draw_text_row(s_row, num, co2_str, co2_x + 1, co2_y, co2_scale, y);
    // CO2 unit below number
    draw_text_row(s_row, small, co2_unit, co2_unit_x, co2_unit_y, co2_unit_scale, y);

    // Humidity (right) with unit, baseline-aligned (anisotropic digits)
    int hum_unit_x = right_aligned_x(small, hum_unit, unit_scale, RIGHT_MARGIN);
    int hum_x = hum_unit_x - 6 - (text_width_px_aniso(num, hum_str, th_num_scale_x) + th_bold_px);
    draw_text_row_aniso(s_row, num, hum_str, hum_x,             th_y, th_num_scale_x, th_num_scale_y, y);
    draw_text_row_aniso(s_row, num, hum_str, hum_x + th_bold_px, th_y, th_num_scale_x, th_num_scale_y, y);
    draw_text_row(s_row, small, hum_unit, hum_unit_x, unit_y, unit_scale, y);

    // Status word with emoji
    // Draw emoji based on CO2 level
    if (co2 <= CO2_EXCELLENT_MAX || co2 <= CO2_GOOD_MAX) {
      // Happy emoji for excellent or good
      draw_happy_emoji_row(s_row, y, emoji_x, emoji_y, emoji_size);
    } else {
      // Sad emoji for poor
      draw_sad_emoji_row(s_row, y, emoji_x, emoji_y, emoji_size);
    }
    draw_text_row_advance(s_row, status_font, qual_msg, qual_x, text_y, qual_scale_text, qual_advance, y);

    // Max label (right, above chart)
    draw_text_row(s_row, small, max_str, max_x, max_y, max_scale, y);

    // History window label (centered, above chart)
    if (hist_str[0] != '\0') {
      draw_text_row(s_row, hist_font, hist_str, hist_x, hist_y, hist_scale, y);
    }

    // Chart
    draw_chart_row(s_row, y, chart_top, chart_bottom, handle, max_value);

    // Stream
    (void)sl_memlcd_draw(s_memlcd, s_row, (uint16_t)y, 1);
  }
}

//------------------------------------------------------------------------------
// Simple mode renderer: large CO2 number centered, small "ppm" label, status text
static void render_frame_simple(memlcd_handle_t *handle)
{
  int co2  = (int)handle->co2_value;
  int batt = (int)handle->battery_level;

  const GLIB_Font_t *small       = (const GLIB_Font_t *)&GLIB_FontNarrow6x8;
  const GLIB_Font_t *num         = (const GLIB_Font_t *)&GLIB_FontNumber16x20;
  const GLIB_Font_t *status_font = (const GLIB_Font_t *)&GLIB_FontNormal8x8;

  char co2_str[8];
  snprintf(co2_str, sizeof(co2_str), "%d", co2);
  char batt_str[6];
  snprintf(batt_str, sizeof(batt_str), "%d%%", batt);
  const char *co2_unit  = "ppm";
  const char *qual_msg  = co2_quality_text(co2);
  const char *interval  = handle->date ? handle->date : "";

  // Top bar (interval left, battery right) — identical to complex mode
  uint8_t top_scale   = 3;
  int top_bar_h       = small->fontHeight * (int)top_scale;
  int top_bar_bottom  = TOP_BAR_Y + top_bar_h;

  const int batt_icon_w = 24; // 22 body + 2 terminal
  const int batt_icon_h = 12;
  int batt_icon_x  = LCD_WIDTH - RIGHT_MARGIN - batt_icon_w;
  int batt_icon_y0 = TOP_BAR_Y + (top_bar_h - batt_icon_h) / 2;
  int batt_text_x  = batt_icon_x - 4 - text_width_px(small, batt_str, top_scale);

  // Adaptive CO2 scale: keep digits fitting inside 400px
  int len = (int)strlen(co2_str);
  uint8_t co2_scale = (len <= 3) ? 7 : (len == 4) ? 6 : 5;
  int co2_h = num->fontHeight * (int)co2_scale;

  // Unit label: scale 2 (same size as complex mode)
  uint8_t unit_scale = 2;
  int unit_h = small->fontHeight * (int)unit_scale;

  // Status label: scale 3
  uint8_t qual_scale = 3;
  int qual_h = status_font->fontHeight * (int)qual_scale;

  const int gap1 = 4; // between ppm and CO2 number
  const int gap2 = 8; // between CO2 number and status

  // Center the CO2 number against the full screen height so it appears visually centered
  int co2_y  = (LCD_HEIGHT - co2_h) / 2;
  int unit_y = co2_y - gap1 - unit_h;
  if (unit_y < top_bar_bottom + 2) unit_y = top_bar_bottom + 2; // clamp below top bar
  int qual_y = co2_y + co2_h + gap2;

  // Horizontal centering
  int unit_x = centered_x(small, co2_unit, unit_scale);
  int co2_x  = centered_x(num, co2_str, co2_scale);
  int qual_x = centered_x(status_font, qual_msg, qual_scale);

  for (int y = 0; y < LCD_HEIGHT; ++y) {
    row_clear(s_row);

    // Top bar: interval (left) + battery text + icon (right)
    draw_text_row(s_row, small, interval,  LEFT_MARGIN, TOP_BAR_Y, top_scale, y);
    draw_text_row(s_row, small, batt_str,  batt_text_x, TOP_BAR_Y, top_scale, y);
    draw_battery_row(s_row, y, batt_icon_x, batt_icon_y0, batt);

    // "ppm" label (small, centered above number)
    draw_text_row(s_row, small, co2_unit, unit_x, unit_y, unit_scale, y);

    // CO2 number (large, extra-bold via triple overdraw)
    draw_text_row(s_row, num, co2_str, co2_x - 1, co2_y, co2_scale, y);
    draw_text_row(s_row, num, co2_str, co2_x,     co2_y, co2_scale, y);
    draw_text_row(s_row, num, co2_str, co2_x + 1, co2_y, co2_scale, y);

    // Status label (Excellent / Good / Poor)
    draw_text_row(s_row, status_font, qual_msg, qual_x, qual_y, qual_scale, y);

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
  sl_memlcd_power_on(s_memlcd, false);
}

void memlcd_draw_data_simple(memlcd_handle_t *handle)
{
  if (!s_memlcd) init_display();
  if (!handle) return;
  render_frame_simple(handle);
  sl_memlcd_power_on(s_memlcd, false);
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

void memlcd_display_calibration_message(const char *line1, const char *line2)
{
  if (!s_memlcd) init_display();
  if (!line1) line1 = "";
  if (!line2) line2 = "";
  const GLIB_Font_t *mono8 = (const GLIB_Font_t *)&GLIB_FontNormal8x8;
  uint8_t l1_scale = 3;
  uint8_t l2_scale = 2;
  int total_h = (mono8->fontHeight * l1_scale) + 12 + (mono8->fontHeight * l2_scale);
  int start_y = (LCD_HEIGHT - total_h) / 2;
  int l1_y  = start_y;
  int l2_y  = l1_y + (mono8->fontHeight * l1_scale) + 12;
  int l1_x  = centered_x(mono8, line1, l1_scale);
  int l2_x  = centered_x(mono8, line2, l2_scale);

  for (int y = 0; y < LCD_HEIGHT; ++y) {
    row_clear(s_row);
    draw_text_row(s_row, mono8, line1, l1_x, l1_y, l1_scale, y);
    draw_text_row(s_row, mono8, line2, l2_x, l2_y, l2_scale, y);
    (void)sl_memlcd_draw(s_memlcd, s_row, (uint16_t)y, 1);
  }
}

void memlcd_display_calibration_progress(uint8_t percent, uint16_t seconds_remaining,
                                         uint8_t sample_idx, uint8_t sample_total)
{
  if (!s_memlcd) init_display();
  if (percent > 100) percent = 100;
  const GLIB_Font_t *mono8 = (const GLIB_Font_t *)&GLIB_FontNormal8x8;

  char title[] = "CALIBRATION";
  char timer_str[24];
  snprintf(timer_str, sizeof(timer_str), "%u:%02u left", (unsigned)(seconds_remaining/60), (unsigned)(seconds_remaining%60));
  char sample_str[24];
  snprintf(sample_str, sizeof(sample_str), "%u/%u samples", (unsigned)sample_idx, (unsigned)sample_total);

  // Layout
  uint8_t title_scale = 2;
  uint8_t text_scale  = 2;
  int title_y = 30;
  int title_x = centered_x(mono8, title, title_scale);
  int sample_y = title_y + mono8->fontHeight * title_scale + 12;
  int sample_x = centered_x(mono8, sample_str, text_scale);
  int timer_y = sample_y + mono8->fontHeight * text_scale + 8;
  int timer_x = centered_x(mono8, timer_str, text_scale);

  // Progress bar geometry
  int bar_w = LCD_WIDTH - 80;
  int bar_h = 14;
  int bar_x = (LCD_WIDTH - bar_w) / 2;
  int bar_y = timer_y + mono8->fontHeight * text_scale + 16;
  int fill_w = (bar_w * percent) / 100;

  for (int y = 0; y < LCD_HEIGHT; ++y) {
    row_clear(s_row);
    // Title
    draw_text_row(s_row, mono8, title, title_x, title_y, title_scale, y);
    // Sample counter
    draw_text_row(s_row, mono8, sample_str, sample_x, sample_y, text_scale, y);
    // Timer
    draw_text_row(s_row, mono8, timer_str, timer_x, timer_y, text_scale, y);
    // Progress bar outline (simple top/bottom lines)
    if (y == bar_y || y == (bar_y + bar_h)) {
      draw_hline(s_row, bar_x, bar_x + bar_w);
    }
    // Fill area
    if (y > bar_y && y < (bar_y + bar_h)) {
      // left and right border pixels
      set_pixel(s_row, bar_x);
      set_pixel(s_row, bar_x + bar_w);
      // fill
      draw_hline(s_row, bar_x + 2, bar_x + 2 + fill_w);
    }
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
