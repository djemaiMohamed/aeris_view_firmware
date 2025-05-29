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

/*******************************************************************************
 ***************************  LOCAL VARIABLES   ********************************
 ******************************************************************************/
static GLIB_Context_t glibContext;

/*******************************************************************************
 **************************   GLOBAL FUNCTIONS   *******************************
 ******************************************************************************/
// Helper function to draw simple battery icon
static void draw_battery_icon(GLIB_Context_t *context, int x, int y, int battery_level)
{
  // Much larger battery outline: 25x14 pixels
  GLIB_Rectangle_t battery_outline = {x, y, x+24, y+13};
  GLIB_drawRectFilled(context, &battery_outline);
  
  // Battery terminal (right side bump) - much larger
  GLIB_Rectangle_t battery_tip = {x+25, y+4, x+27, y+9};
  GLIB_drawRectFilled(context, &battery_tip);
  
  // Battery interior (white background) - larger
  uint32_t original_color = context->foregroundColor;
  context->foregroundColor = White;
  GLIB_Rectangle_t battery_interior = {x+2, y+2, x+22, y+11};
  GLIB_drawRectFilled(context, &battery_interior);
  context->foregroundColor = original_color;
  
  // Battery level indicator (much larger fill area)
  int fill_width = (battery_level * 18) / 100; // Scale to 18 pixels max
  if (fill_width > 0) {
    GLIB_Rectangle_t battery_fill = {x+3, y+3, x+2+fill_width, y+10};
    GLIB_drawRectFilled(context, &battery_fill);
  }
}

// Helper function to draw temperature icon (thermometer)
static void draw_temp_icon(GLIB_Context_t *context, int x, int y)
{
  // Much larger thermometer bulb (radius 4)
  GLIB_drawCircleFilled(context, x+4, y+14, 4);
  // Much larger thermometer stem
  GLIB_Rectangle_t stem = {x+2, y, x+6, y+12};
  GLIB_drawRectFilled(context, &stem);
  // Longer temperature indicator lines
  GLIB_drawLine(context, x+8, y+1, x+11, y+1);
  GLIB_drawLine(context, x+8, y+3, x+12, y+3);
  GLIB_drawLine(context, x+8, y+5, x+11, y+5);
  GLIB_drawLine(context, x+8, y+7, x+12, y+7);
  GLIB_drawLine(context, x+8, y+9, x+11, y+9);
}

// Helper function to draw humidity icon (water drop) - exactly 20px tall to match text
static void draw_humidity_icon(GLIB_Context_t *context, int x, int y)
{
  // Realistic water drop shape: gradual expansion then quick narrowing (20px tall)
  GLIB_drawPixel(context, x+5, y);     // Top point
  GLIB_drawLine(context, x+4, y+1, x+6, y+1); // Very slight expansion
  GLIB_drawLine(context, x+4, y+2, x+6, y+2); // Still narrow
  GLIB_drawLine(context, x+4, y+3, x+6, y+3); // Still narrow
  GLIB_drawLine(context, x+3, y+4, x+7, y+4); // Gradual widening
  GLIB_drawLine(context, x+3, y+5, x+7, y+5); // Gradual widening
  GLIB_drawLine(context, x+3, y+6, x+7, y+6); // Gradual widening
  GLIB_drawLine(context, x+2, y+7, x+8, y+7); // Getting wider slowly
  GLIB_drawLine(context, x+2, y+8, x+8, y+8); // Getting wider slowly
  GLIB_drawLine(context, x+2, y+9, x+8, y+9); // Getting wider slowly
  GLIB_drawLine(context, x+1, y+10, x+9, y+10); // Approaching max width
  GLIB_drawLine(context, x+1, y+11, x+9, y+11); // Approaching max width
  GLIB_drawLine(context, x, y+12, x+10, y+12); // // Max width reached
  GLIB_drawLine(context, x, y+13, x+10, y+13); // Max width
  GLIB_drawLine(context, x, y+14, x+10, y+14); // // Max width reached
  GLIB_drawLine(context, x, y+15, x+10, y+15); // // Max width reached
  GLIB_drawLine(context, x+1, y+16, x+9, y+16); // Max width
  GLIB_drawLine(context, x+3, y+17, x+7, y+17); // Quick narrowing
  GLIB_drawPixel(context, x+5, y+18); // Bottom point (total height 20px)
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
    // Good - all bars full (larger bars)
    bar_heights[0] = 12; bar_heights[1] = 9; bar_heights[2] = 6; bar_heights[3] = 3;
  } else if (co2_value <= 1000) {
    // Moderate - 3 bars (larger bars)
    bar_heights[0] = 12; bar_heights[1] = 9; bar_heights[2] = 6; bar_heights[3] = 0;
  } else if (co2_value <= 1500) {
    // Poor - 2 bars (larger bars)
    bar_heights[0] = 12; bar_heights[1] = 9; bar_heights[2] = 0; bar_heights[3] = 0;
  } else {
    // Very poor - 1 bar (larger bars)
    bar_heights[0] = 12; bar_heights[1] = 0; bar_heights[2] = 0; bar_heights[3] = 0;
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

void memlcd_draw_data(memlcd_handle_t *handle)
{
  uint32_t status;

  /* Enable the memory lcd */
  status = sl_board_enable_display();
  EFM_ASSERT(status == SL_STATUS_OK);

  /* Initialize the DMD support for memory lcd display */
  status = DMD_init(0);
  EFM_ASSERT(status == DMD_OK);

  /* Initialize the glib context */
  status = GLIB_contextInit(&glibContext);
  EFM_ASSERT(status == GLIB_OK);

  glibContext.backgroundColor = White;
  glibContext.foregroundColor = Black;
  
  /* Clear display first to ensure clean slate */
  GLIB_clear(&glibContext);
  
  // CO2 value to display from the handle
  int co2_value = handle->co2_value;
  
  /* ---------- OPTIMIZED LAYOUT FOR MAXIMUM VISIBILITY ---------- */
  
  if(co2_value != 0) {
    /* ---------- MASSIVE CO2 DISPLAY (CENTER OF SCREEN) ---------- */
    // Use largest available font for CO2 value and position it higher up
    GLIB_setFont(&glibContext, (GLIB_Font_t *)&GLIB_FontNumber16x20);
    
    char co2_value_str[10];
    snprintf(co2_value_str, sizeof(co2_value_str), "%d", co2_value);
    
    // Position CO2 value in upper-center area to leave room for other elements
    const GLIB_Font_t *font = &glibContext.font;
    int fontWidth = font->fontWidth;
    int fontHeight = font->fontHeight;
    
    int co2_width = strlen(co2_value_str) * fontWidth;
    int co2_x = (LCD_WIDTH - co2_width) / 2;
    int co2_y = 35; // Position higher to maximize space usage
    
    GLIB_drawString(&glibContext, co2_value_str, strlen(co2_value_str), 
                    co2_x, co2_y, true);
    
    /* ---------- COMPACT TOP ROW: Battery + Update Info ---------- */
    GLIB_setFont(&glibContext, (GLIB_Font_t *)&GLIB_FontNormal8x8);
    
    // Battery icon in top right (much larger, more visible) - adjusted position for larger icon
    draw_battery_icon(&glibContext, 95, 2, handle->battery_level);
    char battery_str[5];
    snprintf(battery_str, sizeof(battery_str), "%d%%", handle->battery_level);
    GLIB_drawString(&glibContext, battery_str, strlen(battery_str), 65, 6, false);
    
    // Update info in top left
    if (handle->date != NULL && strlen(handle->date) <= 4) {
      GLIB_drawString(&glibContext, handle->date, strlen(handle->date), 2, 6, false);
    }
    
    /* ---------- PPM LABEL UNDER CO2 ---------- */
    char ppm_str[] = "ppm";
    int ppm_width = strlen(ppm_str) * 8;
    GLIB_drawString(&glibContext, ppm_str, strlen(ppm_str), 
                   (LCD_WIDTH - ppm_width) / 2, co2_y + fontHeight + 2, false);
    
    /* ---------- LARGE AIR QUALITY INDICATOR (RIGHT SIDE) ---------- */
    // Position air quality bars to the right of CO2, make them larger
    int bars_x = co2_x + co2_width + 12;
    int bars_y = co2_y + 8;
    if (bars_x + 18 < LCD_WIDTH) { // Ensure larger bars fit (4 bars * 4 spacing + 2 width = 18)
      draw_air_quality_indicator(&glibContext, bars_x, bars_y, co2_value);
    }
    
    /* ---------- BOTTOM SECTION: Large Temperature and Humidity ---------- */
    // Position bottom elements much lower to utilize full screen
    int bottom_y = LCD_HEIGHT - 25; // Adjusted for larger font
    
    // Use large font for both temperature and humidity values
    GLIB_setFont(&glibContext, (GLIB_Font_t *)&GLIB_FontNumber16x20);
    
    // Temperature with icon on same row (left side)
    char temp_value[6];
    int temp_whole = (int)handle->temperature_value;
    int temp_frac = (int)((handle->temperature_value - temp_whole) * 10);
    if (temp_frac < 0) temp_frac = -temp_frac;
    snprintf(temp_value, sizeof(temp_value), "%d", temp_whole);
    
    // Draw temperature icon aligned with text baseline
    draw_temp_icon(&glibContext, 2, bottom_y);
    // Draw temperature value next to icon
    GLIB_drawString(&glibContext, temp_value, strlen(temp_value), 16, bottom_y, false);
    
    // Humidity with icon on same row (right side)
    char humidity_value[4];
    snprintf(humidity_value, sizeof(humidity_value), "%d", handle->humidity_value);
    
    int humidity_width = strlen(humidity_value) * 16; // 16 pixels per character for large font
    int humidity_text_x = LCD_WIDTH - humidity_width - 2;
    
    // Draw humidity icon aligned with text baseline (to the left of the text)
    draw_humidity_icon(&glibContext, humidity_text_x - 12, bottom_y);
    // Draw humidity value
    GLIB_drawString(&glibContext, humidity_value, strlen(humidity_value), 
                   humidity_text_x, bottom_y, false);
                   
  } else {
    // No data available - show large message in center
    GLIB_setFont(&glibContext, (GLIB_Font_t *)&GLIB_FontNumber16x20);
    char no_data[] = "NO DATA";
    int no_data_width = strlen(no_data) * 16;
    GLIB_drawString(&glibContext, no_data, strlen(no_data), 
                   (LCD_WIDTH - no_data_width) / 2, LCD_HEIGHT / 2 - 10, true);
  }
  
  /* Update the display */
  DMD_updateDisplay();
  DMD_sleep();
}

/***************************************************************************//**
 * @brief Displays the BLE passkey on the LCD.
 * @param[in] passkey The passkey to display.
 ******************************************************************************/
void memlcd_display_passkey(uint32_t passkey)
{
  uint32_t status;

  /* Enable the memory lcd */
  status = sl_board_enable_display();
  EFM_ASSERT(status == SL_STATUS_OK);

  /* Initialize the DMD support for memory lcd display */
  status = DMD_init(0);
  EFM_ASSERT(status == DMD_OK);

  /* Initialize the glib context */
  status = GLIB_contextInit(&glibContext);
  EFM_ASSERT(status == GLIB_OK);

  glibContext.backgroundColor = White;
  glibContext.foregroundColor = Black;

  /* Clear display first */
  GLIB_clear(&glibContext);

  // Message: "Enter on phone:"
  char msg_str[] = "Enter on phone:";
  // Set font for message and get its properties
  GLIB_setFont(&glibContext, (GLIB_Font_t *)&GLIB_FontNarrow6x8); // Use narrow font for this message
  const GLIB_Font_t *msg_font_props = &glibContext.font;
  int msg_actual_char_width = msg_font_props->fontWidth;
  int msg_actual_char_height = msg_font_props->fontHeight;

  int msg_text_render_width = strlen(msg_str) * msg_actual_char_width;
  int msg_x_pos = (LCD_WIDTH - msg_text_render_width) / 2;

  // Passkey
  char passkey_str[7]; // 6 digits + null terminator
  snprintf(passkey_str, sizeof(passkey_str), "%06lu", (unsigned long)passkey % 1000000); // Ensure 6 digits

  // Set font for passkey and get its properties
  GLIB_setFont(&glibContext, (GLIB_Font_t *)&GLIB_FontNumber16x20);
  const GLIB_Font_t *passkey_font_props = &glibContext.font;
  int passkey_actual_char_width = passkey_font_props->fontWidth;
  int passkey_actual_char_height = passkey_font_props->fontHeight;

  int passkey_text_len = strlen(passkey_str);
  int passkey_text_render_width = passkey_text_len * passkey_actual_char_width;
  int passkey_x_pos = (LCD_WIDTH - passkey_text_render_width) / 2;

  // Calculate Y positions for vertical centering of the block
  // Uses actual heights obtained from font properties
  int total_content_height = msg_actual_char_height + 10 /* spacing */ + passkey_actual_char_height;
  int start_y_pos = (LCD_HEIGHT - total_content_height) / 2;

  int msg_y_pos = start_y_pos;
  int passkey_y_pos = msg_y_pos + msg_actual_char_height + 10; // 10px spacing

  // Draw message
  GLIB_setFont(&glibContext, (GLIB_Font_t *)&GLIB_FontNarrow6x8); // Ensure correct font (msg_str font) is active
  GLIB_drawString(&glibContext, msg_str, strlen(msg_str), msg_x_pos, msg_y_pos, false);

  // Draw passkey
  GLIB_setFont(&glibContext, (GLIB_Font_t *)&GLIB_FontNumber16x20); // Ensure correct font (passkey font) is active
  GLIB_drawString(&glibContext, passkey_str, passkey_text_len, passkey_x_pos, passkey_y_pos, true);

  /* Update the display */
  DMD_updateDisplay();
  DMD_sleep();
}

/***************************************************************************//**
 * @brief Displays a waiting for measurement message on the LCD.
 ******************************************************************************/
void memlcd_display_waiting_message(void)
{
  uint32_t status;

  /* Enable the memory lcd */
  status = sl_board_enable_display();
  EFM_ASSERT(status == SL_STATUS_OK);

  /* Initialize the DMD support for memory lcd display */
  status = DMD_init(0);
  EFM_ASSERT(status == DMD_OK);

  /* Initialize the glib context */
  status = GLIB_contextInit(&glibContext);
  EFM_ASSERT(status == GLIB_OK);

  glibContext.backgroundColor = White;
  glibContext.foregroundColor = Black;

  /* Clear display first */
  GLIB_clear(&glibContext);

  // Main message: "Measuring..."
  char main_msg[] = "MEASURING...";
  // Set font for main message and get its properties
  GLIB_setFont(&glibContext, (GLIB_Font_t *)&GLIB_FontNormal8x8);
  const GLIB_Font_t *main_font_props = &glibContext.font;
  int main_actual_char_width = main_font_props->fontWidth;
  int main_actual_char_height = main_font_props->fontHeight;

  int main_text_render_width = strlen(main_msg) * main_actual_char_width;
  int main_x_pos = (LCD_WIDTH - main_text_render_width) / 2;

  // Secondary message: "Please wait"
  char sub_msg[] = "PLEASE WAIT :)";
  // Set font for secondary message and get its properties
  GLIB_setFont(&glibContext, (GLIB_Font_t *)&GLIB_FontNormal8x8);
  const GLIB_Font_t *sub_font_props = &glibContext.font;
  int sub_actual_char_width = sub_font_props->fontWidth;
  int sub_actual_char_height = sub_font_props->fontHeight;

  int sub_text_render_width = strlen(sub_msg) * sub_actual_char_width;
  int sub_x_pos = (LCD_WIDTH - sub_text_render_width) / 2;

  // Calculate Y positions for vertical centering of the block
  // Uses actual heights obtained from font properties
  int total_content_height = main_actual_char_height + 8 /* spacing */ + sub_actual_char_height;
  int start_y_pos = (LCD_HEIGHT - total_content_height) / 2;

  int main_y_pos = start_y_pos;
  int sub_y_pos = main_y_pos + main_actual_char_height + 8; // 8px spacing

  // Draw main message
  GLIB_setFont(&glibContext, (GLIB_Font_t *)&GLIB_FontNormal8x8);
  GLIB_drawString(&glibContext, main_msg, strlen(main_msg), main_x_pos, main_y_pos, true);

  // Draw secondary message
  GLIB_setFont(&glibContext, (GLIB_Font_t *)&GLIB_FontNormal8x8);
  GLIB_drawString(&glibContext, sub_msg, strlen(sub_msg), sub_x_pos, sub_y_pos, false);

  /* Update the display */
  DMD_updateDisplay();
  DMD_sleep();
}
