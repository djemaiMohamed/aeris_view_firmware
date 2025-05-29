/***************************************************************************//**
 * @file memlcd_app.c
 * @brief Memory Liquid Crystal Display (LCD) example functions
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
#include "glib.h"
#include "dmd.h"
#include "sl_memlcd.h"

/*******************************************************************************
 *******************************   DEFINES   ***********************************
 ******************************************************************************/

#define LCD_WIDTH           128
#define LCD_HEIGHT          128
#define CO2_HISTORY_SIZE    128

/*******************************************************************************
 ***************************  LOCAL VARIABLES   ********************************
 ******************************************************************************/
static GLIB_Context_t glibContext;

/*******************************************************************************
 **************************   GLOBAL FUNCTIONS   *******************************
 ******************************************************************************/
// Common LCD initialization helper
static void init_lcd_context(void)
{
  uint32_t status;
  status = sl_board_enable_display();
  EFM_ASSERT(status == SL_STATUS_OK);
  status = DMD_init(0);
  EFM_ASSERT(status == DMD_OK);
  status = GLIB_contextInit(&glibContext);
  EFM_ASSERT(status == GLIB_OK);
  glibContext.backgroundColor = White;
  glibContext.foregroundColor = Black;
  GLIB_clear(&glibContext);
}

// Common display finalization helper
static void finalize_display(void)
{
  DMD_updateDisplay();
  DMD_sleep();
}

// Helper to set font and get properties
static void set_font_and_get_props(GLIB_Font_t *font, int *char_width, int *char_height)
{
  GLIB_setFont(&glibContext, font);
  const GLIB_Font_t *font_props = &glibContext.font;
  *char_width = font_props->fontWidth;
  *char_height = font_props->fontHeight;
}

// Helper to calculate centered X position
static int get_centered_x(const char *text, int char_width)
{
  return (LCD_WIDTH - (strlen(text) * char_width)) / 2;
}

// Helper to draw centered string
static void draw_centered_string(const char *text, int y, bool opaque)
{
  const GLIB_Font_t *font_props = &glibContext.font;
  int x = get_centered_x(text, font_props->fontWidth);
  GLIB_drawString(&glibContext, text, strlen(text), x, y, opaque);
}

// Helper to format integer and draw it
static void draw_integer(int value, int x, int y, bool opaque)
{
  char buffer[12]; // Enough for any 32-bit integer
  snprintf(buffer, sizeof(buffer), "%d", value);
  GLIB_drawString(&glibContext, buffer, strlen(buffer), x, y, opaque);
}

// Helper to format integer with suffix and draw it
static void draw_integer_with_suffix(int value, const char *suffix, int x, int y, bool opaque)
{
  char buffer[16]; // Enough for integer + short suffix
  snprintf(buffer, sizeof(buffer), "%d%s", value, suffix);
  GLIB_drawString(&glibContext, buffer, strlen(buffer), x, y, opaque);
}

// Helper function to draw battery icon
static void draw_battery_icon(GLIB_Context_t *context, int x, int y, int battery_level)
{
  GLIB_Rectangle_t rects[] = {
    {x, y, x+24, y+13},           // Battery outline
    {x+25, y+4, x+27, y+9},       // Battery terminal
    {x+2, y+2, x+22, y+11}        // Interior (will be white)
  };
  
  // Draw outline and terminal
  GLIB_drawRectFilled(context, &rects[0]);
  GLIB_drawRectFilled(context, &rects[1]);
  
  // Draw white interior
  uint32_t original_color = context->foregroundColor;
  context->foregroundColor = White;
  GLIB_drawRectFilled(context, &rects[2]);
  context->foregroundColor = original_color;
  
  // Battery level indicator
  int fill_width = (battery_level * 18) / 100;
  if (fill_width > 0) {
    GLIB_Rectangle_t fill = {x+3, y+3, x+2+fill_width, y+10};
    GLIB_drawRectFilled(context, &fill);
  }
}

// Helper function to draw temperature icon (thermometer)
static void draw_temp_icon(GLIB_Context_t *context, int x, int y)
{
  GLIB_drawCircleFilled(context, x+4, y+14, 4);
  GLIB_Rectangle_t stem = {x+2, y, x+6, y+12};
  GLIB_drawRectFilled(context, &stem);
  // Temperature indicator lines
  int lines[][4] = {{x+8,y+1,x+11,y+1}, {x+8,y+3,x+12,y+3}, {x+8,y+5,x+11,y+5}, {x+8,y+7,x+12,y+7}, {x+8,y+9,x+11,y+9}};
  for (int i = 0; i < 5; i++) {
    GLIB_drawLine(context, lines[i][0], lines[i][1], lines[i][2], lines[i][3]);
  }
}

// Helper function to draw humidity icon (water drop)
static void draw_humidity_icon(GLIB_Context_t *context, int x, int y)
{
  // Simplified water drop shape using fewer lines
  GLIB_drawPixel(context, x+5, y);     // Top point
  for (int i = 1; i <= 3; i++) {
    GLIB_drawLine(context, x+5-i/2, y+i, x+5+i/2, y+i);
  }
  for (int i = 4; i <= 15; i++) {
    int width = (i < 12) ? 5 : 5 - (i-12);
    GLIB_drawLine(context, x+5-width, y+i, x+5+width, y+i);
  }
  for (int i = 16; i <= 17; i++) {
    GLIB_drawLine(context, x+3, y+i, x+7, y+i);
  }
  GLIB_drawPixel(context, x+5, y+18); // Bottom point
}

// Helper function to draw air quality indicator
static void draw_air_quality_indicator(GLIB_Context_t *context, int x, int y, int co2_value)
{
  // Draw 4 quality bars with different heights based on CO2 level - make them larger
  int bar_heights[4];
  
  if (co2_value == 0) {
    // No data - all bars empty
    bar_heights[0] = bar_heights[1] = bar_heights[2] = bar_heights[3] = 0;
  } else if (co2_value < 800) {
    // Good - all bars full (small to high, left to right)
    bar_heights[0] = 3; bar_heights[1] = 6; bar_heights[2] = 9; bar_heights[3] = 12;
  } else if (co2_value <= 1000) {
    // Moderate - 3 bars (small to high, left to right)
    bar_heights[0] = 3; bar_heights[1] = 6; bar_heights[2] = 9; bar_heights[3] = 0;
  } else if (co2_value <= 1500) {
    // Poor - 2 bars (small to high, left to right)
    bar_heights[0] = 3; bar_heights[1] = 6; bar_heights[2] = 0; bar_heights[3] = 0;
  } else {
    // Very poor - 1 bar (small, left)
    bar_heights[0] = 3; bar_heights[1] = 0; bar_heights[2] = 0; bar_heights[3] = 0;
  }
  
  // Draw the larger bars with more spacing
  for (int i = 0; i < 4; i++) {
    int bar_x = x + i * 4; // Increased spacing
    if (bar_heights[i] > 0) {
      GLIB_Rectangle_t bar = {bar_x, y + 12 - bar_heights[i], bar_x + 2, y + 12}; // Wider bars
      GLIB_drawRectFilled(context, &bar);
    }
  }
}

// Helper function to draw CO2 history chart
static void draw_co2_history_chart(GLIB_Context_t *context, int x, int y, int width, int height, memlcd_handle_t *handle)
{
  // Draw dotted top and bottom borders (no left/right borders)
  for (int i = x; i < x + width; i += 3) {
    GLIB_drawPixel(context, i, y);                    // Top dotted line
    GLIB_drawPixel(context, i, y + height - 1);      // Bottom dotted line
  }
  
  // Draw horizontal grid line at 1500 ppm (middle) - dotted
  int mid_y = y + height/2;
  for (int i = x; i < x + width; i += 3) {
    GLIB_drawPixel(context, i, mid_y);
  }
  
  // Draw the CO2 history data if available
  if (handle->history_count > 1) {
    int chart_area_height = height;
    int chart_area_width = width;
    int points_to_show = (chart_area_width < handle->history_count) ? chart_area_width : handle->history_count;
    
    // Draw the area chart
    for (int i = 0; i < points_to_show; i++) {
      // Get data point
      int idx = (handle->history_index - points_to_show + i + CO2_HISTORY_SIZE) % CO2_HISTORY_SIZE;
      uint16_t co2_value = (handle->co2_history[idx] > 3000) ? 3000 : handle->co2_history[idx];
      
      // Convert CO2 value to Y coordinate (inverted)
      int data_y = y + height - 1 - ((co2_value * chart_area_height) / 3000);
      
      // Draw vertical line from bottom to data point (area fill)
      GLIB_drawLine(context, x + i, y + height - 1, x + i, data_y);
    }
  }
}

void memlcd_draw_data(memlcd_handle_t *handle)
{
  /* Enable the memory lcd */
  init_lcd_context();

  // CO2 value to display from the handle
  int co2_value = handle->co2_value;
  int fontWidth, fontHeight; // Declare at function scope for reuse
  
  /* ---------- OPTIMIZED LAYOUT FOR MAXIMUM VISIBILITY ---------- */
  
  if(co2_value != 0) {
    /* ---------- MASSIVE CO2 DISPLAY (CENTER OF SCREEN) ---------- */
    // Use largest available font for CO2 value and position it higher up
    set_font_and_get_props((GLIB_Font_t *)&GLIB_FontNumber16x20, &fontWidth, &fontHeight);
    
    // Calculate centered position for CO2 value (assume max 4 digits)
    int co2_x = get_centered_x("0000", fontWidth);
    int co2_y = 20; // Moved higher toward battery/measurement interval to create space for chart
    
    draw_integer(co2_value, co2_x, co2_y, true);
    
    /* ---------- COMPACT TOP ROW: Battery + Update Info ---------- */
    int small_font_width, small_font_height;
    set_font_and_get_props((GLIB_Font_t *)&GLIB_FontNormal8x8, &small_font_width, &small_font_height);
    
    // Battery icon in top right (much larger, more visible) - adjusted position for larger icon
    draw_battery_icon(&glibContext, 95, 2, handle->battery_level);
    draw_integer_with_suffix(handle->battery_level, "%", 65, 6, false);
    
    // Update info in top left
    if (handle->date != NULL && strlen(handle->date) <= 4) {
      GLIB_drawString(&glibContext, handle->date, strlen(handle->date), 2, 6, false);
    }
    
    /* ---------- PPM LABEL UNDER CO2 ---------- */
    draw_centered_string("co2", co2_y + fontHeight + 2, false);
    
    /* ---------- AIR QUALITY INDICATOR (RIGHT SIDE OF CO2) ---------- */
    // Position air quality bars to the right of CO2 value (fixed position for max 4 digits)
    int bars_x = co2_x + (4 * fontWidth) + 12; // Assume max 4 digits
    int bars_y = co2_y + 8;
    if (bars_x + 15 < LCD_WIDTH) { // Ensure bars fit (4 bars * 4 spacing - 1 = 15 pixels)
      draw_air_quality_indicator(&glibContext, bars_x, bars_y, co2_value);
    }
    
    /* ---------- CO2 HISTORY CHART (MIDDLE SECTION) ---------- */
    // Center chart vertically between CO2 section and temperature/humidity section
    int co2_section_bottom = co2_y + fontHeight + 10; // Bottom of CO2 + co2 label
    int temp_section_top = LCD_HEIGHT - 25; // Top of temperature/humidity section
    int available_space = temp_section_top - co2_section_bottom;
    int chart_height = 35; // Chart height
    int chart_y = co2_section_bottom + (available_space - chart_height) / 2; // Centered position
    int chart_width = LCD_WIDTH; // Full screen width (128 pixels)
    int chart_x = 0; // Start from left edge
    
    // Only draw chart if we have history data
    if (handle->history_count > 0) {
      draw_co2_history_chart(&glibContext, chart_x, chart_y, chart_width, chart_height, handle);
    }
    
    /* ---------- BOTTOM SECTION: Large Temperature and Humidity ---------- */
    // Position bottom elements much lower to utilize full screen
    int bottom_y = LCD_HEIGHT - 25; // Adjusted for larger font
    
    // Use large font for both temperature and humidity values
    int large_font_width, large_font_height;
    set_font_and_get_props((GLIB_Font_t *)&GLIB_FontNumber16x20, &large_font_width, &large_font_height);
    
    // Temperature with icon on same row (left side)
    int temp_whole = (int)handle->temperature_value;
    int temp_frac = (int)((handle->temperature_value - temp_whole) * 10);
    if (temp_frac < 0) temp_frac = -temp_frac;
    
    // Draw temperature icon aligned with text baseline
    draw_temp_icon(&glibContext, 2, bottom_y);
    // Draw temperature value next to icon
    draw_integer(temp_whole, 16, bottom_y, false);
    
    // Humidity with icon on same row (right side)
    int humidity_width = 2 * large_font_width; // Assume max 2 digits for humidity
    int humidity_text_x = LCD_WIDTH - humidity_width - 2;
    
    // Draw humidity icon aligned with text baseline (to the left of the text)
    draw_humidity_icon(&glibContext, humidity_text_x - 12, bottom_y);
    // Draw humidity value
    draw_integer(handle->humidity_value, humidity_text_x, bottom_y, false);
                   
  } else {
    // No data available - show large message in center
    set_font_and_get_props((GLIB_Font_t *)&GLIB_FontNumber16x20, &fontWidth, &fontHeight);
    draw_centered_string("NO DATA", LCD_HEIGHT / 2 - 10, true);
  }
  
  /* Update the display */
  finalize_display();
}

/***************************************************************************//**
 * @brief Displays the BLE passkey on the LCD.
 * @param[in] passkey The passkey to display.
 ******************************************************************************/
void memlcd_display_passkey(uint32_t passkey)
{
  /* Enable the memory lcd */
  init_lcd_context();

  // Message: "Enter on phone:"
  char msg_str[] = "Enter on phone:";
  int msg_char_width, msg_char_height;
  set_font_and_get_props((GLIB_Font_t *)&GLIB_FontNarrow6x8, &msg_char_width, &msg_char_height);
  int msg_x_pos = get_centered_x(msg_str, msg_char_width);

  // Passkey
  char passkey_str[7]; // 6 digits + null terminator
  snprintf(passkey_str, sizeof(passkey_str), "%06lu", (unsigned long)passkey % 1000000);

  // Set font for passkey and get its properties
  int passkey_char_width, passkey_char_height;
  set_font_and_get_props((GLIB_Font_t *)&GLIB_FontNumber16x20, &passkey_char_width, &passkey_char_height);
  int passkey_x_pos = get_centered_x(passkey_str, passkey_char_width);

  // Calculate Y positions for vertical centering
  int total_content_height = msg_char_height + 10 + passkey_char_height;
  int start_y_pos = (LCD_HEIGHT - total_content_height) / 2;
  int msg_y_pos = start_y_pos;
  int passkey_y_pos = msg_y_pos + msg_char_height + 10;

  // Draw message
  set_font_and_get_props((GLIB_Font_t *)&GLIB_FontNarrow6x8, &msg_char_width, &msg_char_height);
  GLIB_drawString(&glibContext, msg_str, strlen(msg_str), msg_x_pos, msg_y_pos, false);

  // Draw passkey
  set_font_and_get_props((GLIB_Font_t *)&GLIB_FontNumber16x20, &passkey_char_width, &passkey_char_height);
  GLIB_drawString(&glibContext, passkey_str, strlen(passkey_str), passkey_x_pos, passkey_y_pos, true);

  /* Update the display */
  finalize_display();
}

/***************************************************************************//**
 * @brief Displays a waiting for measurement message on the LCD.
 ******************************************************************************/
void memlcd_display_waiting_message(void)
{
  /* Enable the memory lcd */
  init_lcd_context();

  // Main message: "Measuring..."
  char main_msg[] = "MEASURING...";
  char sub_msg[] = "PLEASE WAIT :)";
  
  // Get font properties for Normal8x8 (both messages use same font)
  int char_width, char_height;
  set_font_and_get_props((GLIB_Font_t *)&GLIB_FontNormal8x8, &char_width, &char_height);

  // Calculate positions
  int main_x_pos = get_centered_x(main_msg, char_width);
  int sub_x_pos = get_centered_x(sub_msg, char_width);

  // Calculate Y positions for vertical centering
  int total_content_height = char_height + 8 + char_height; // main + spacing + sub
  int start_y_pos = (LCD_HEIGHT - total_content_height) / 2;
  int main_y_pos = start_y_pos;
  int sub_y_pos = main_y_pos + char_height + 8;

  // Draw messages
  GLIB_drawString(&glibContext, main_msg, strlen(main_msg), main_x_pos, main_y_pos, true);
  GLIB_drawString(&glibContext, sub_msg, strlen(sub_msg), sub_x_pos, sub_y_pos, false);

  /* Update the display */
  finalize_display();
}

/***************************************************************************//**
 * @brief Add CO2 value to history buffer
 * @param[in] handle The handle containing the history buffer
 * @param[in] co2_value The CO2 value to add
 ******************************************************************************/
void memlcd_add_co2_to_history(memlcd_handle_t *handle, uint16_t co2_value)
{
  // Add the new value to the circular buffer
  handle->co2_history[handle->history_index] = co2_value;
  
  // Move to next position
  handle->history_index = (handle->history_index + 1) % CO2_HISTORY_SIZE;
  
  // Update count (max at CO2_HISTORY_SIZE)
  if (handle->history_count < CO2_HISTORY_SIZE) {
    handle->history_count++;
  }
}
