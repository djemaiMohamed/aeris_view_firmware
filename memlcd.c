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
  
  // Set the small font for peripheral metrics
  GLIB_setFont(&glibContext, (GLIB_Font_t *)&GLIB_FontNarrow6x8);
  
  /* ---------- TOP ROWS: Update interval and battery level ---------- */
  // First row: Update interval with clear label
  char date_str[32];
  if (handle->date != NULL) {
    snprintf(date_str, sizeof(date_str), "UPDATE FREQ: %s", handle->date);
  } else {
    strcpy(date_str, "UPDATE FREQ: --");
  }
  GLIB_drawString(&glibContext, date_str, strlen(date_str), 2, 2, false);
  
  // Second row: Battery level
  char battery_str[20];
  snprintf(battery_str, sizeof(battery_str), "BATTERY: %d %%", handle->battery_level);
  GLIB_drawString(&glibContext, battery_str, strlen(battery_str), 2, 12, false);
  
  /* ---------- CO2 value (main display) ---------- */
  // Fixed starting position after the top rows with spacing
  int current_y = 30; // Start after battery info with some spacing
  
  // Switch to the larger font for the main CO2 display
  GLIB_setFont(&glibContext, (GLIB_Font_t *)&GLIB_FontNumber16x20);
  
  // Get font dimensions directly from the font structure
  const GLIB_Font_t *font = &glibContext.font;
  int fontWidth = font->fontWidth;
  int fontHeight = font->fontHeight;
  
  // CO2 value to display from the handle
  int co2_value = handle->co2_value;
  char co2_value_str[10];
  snprintf(co2_value_str, sizeof(co2_value_str), "%d", co2_value);

  if(co2_value == 0) {
  }
  
  // Center the CO2 value (horizontally only)
  int co2_width = strlen(co2_value_str) * fontWidth;
  int co2_x = (LCD_WIDTH - co2_width) / 2;
  int co2_y = current_y; // Use our tracking variable
  
  if(co2_value != 0) {
    GLIB_drawString(&glibContext, co2_value_str, strlen(co2_value_str), 
                    co2_x, co2_y, true);
  }
  
  // Update our vertical position tracker
  current_y += fontHeight + 4; // Move down by font height plus spacing
  
  // Switch to the small font for other information
  GLIB_setFont(&glibContext, (GLIB_Font_t *)&GLIB_FontNarrow6x8);
  
  // Add "PPM" label under the CO2 value
  char ppm_str[] = "PPM";
  int ppm_width = strlen(ppm_str) * 6;
  GLIB_drawString(&glibContext, ppm_str, strlen(ppm_str), 
                 (LCD_WIDTH - ppm_width) / 2, current_y, false);
  
  // Update position for next element
  current_y += 14; // Move down for air quality section
  
  /* ---------- AIR QUALITY INTERPRETATION ---------- */
  char *quality_label = "INDOOR AIR QUALITY";
  char *quality_value;

  // Determine air quality value based on CO2 level
  if(co2_value == 0) {
    quality_value = "N/A";
  } else if (co2_value < 800) {
    quality_value = "GOOD";
  } else if (co2_value <= 1000) {
    quality_value = "MODERATE";
  } else if (co2_value <= 1500) {
    quality_value = "POOR";
  } else {
    quality_value = "VERY POOR";
  }

  // Calculate position for the air quality label (centered)
  int quality_label_width = strlen(quality_label) * 6; 
  int quality_label_x = (LCD_WIDTH - quality_label_width) / 2;
  int quality_y = current_y; // Use our tracked position

  // Display the "Air quality:" label (not bold)
  GLIB_drawString(&glibContext, quality_label, strlen(quality_label), 
                 quality_label_x, quality_y, false);
  
  // Update position for quality value
  current_y += 10;

  // Switch to a larger font for the quality value
  GLIB_setFont(&glibContext, (GLIB_Font_t *)&GLIB_FontNormal8x8);

  // Calculate position for the quality value (centered)
  int quality_value_width = strlen(quality_value) * 8; 
  int quality_value_x = (LCD_WIDTH - quality_value_width) / 2;
  
  // Draw quality value with the larger font
  GLIB_drawString(&glibContext, quality_value, strlen(quality_value), 
                 quality_value_x, current_y, false);
  
  // Update position for next section
  current_y += 18; // Increase space after air quality section

  // Switch back to the small font for remaining text
  GLIB_setFont(&glibContext, (GLIB_Font_t *)&GLIB_FontNarrow6x8);
  
  /* ---------- BOTTOM SECTION: Temperature and Humidity ---------- */
  // Position at the bottom of the screen rather than using current_y
  int bottom_padding = 10;  // Space from bottom edge of screen
  int row_spacing = 0;      // No space between rows
  
  // Calculate positions for temperature and humidity rows from bottom
  int humidity_row_y = LCD_HEIGHT - bottom_padding;
  int temp_row_y = humidity_row_y - 10 - row_spacing; // 18px is the height of a row with the larger font
  
  // Switch back to small font for labels
  GLIB_setFont(&glibContext, (GLIB_Font_t *)&GLIB_FontNarrow6x8);
  
  // Temperature label and value
  char temp_label[] = "TEMPERATURE:";
  char temp_value[10];
  
  // Convert float to integer parts for more reliable formatting
  int temp_whole = (int)handle->temperature_value;
  int temp_frac = (int)((handle->temperature_value - temp_whole) * 10);
  if (temp_frac < 0) temp_frac = -temp_frac; // Handle negative temperatures
  
  // Format using integers instead of float
  snprintf(temp_value, sizeof(temp_value), "%d.%d C", temp_whole, temp_frac);
  
  // Calculate the total width of the temperature display
  int temp_label_width = strlen(temp_label) * 6;

  // Draw temperature label
  GLIB_drawString(&glibContext, temp_label, strlen(temp_label), 
                 2, temp_row_y, false);
  
  // Switch to larger font for value
  GLIB_setFont(&glibContext, (GLIB_Font_t *)&GLIB_FontNormal8x8);
  
  // Draw temperature value with unit
  if(co2_value != 0) {
    GLIB_drawString(&glibContext, temp_value, strlen(temp_value), 
                    2 + temp_label_width + 4, temp_row_y, false);
  }
  
  // Switch back to small font for humidity label
  GLIB_setFont(&glibContext, (GLIB_Font_t *)&GLIB_FontNarrow6x8);
  
  // Humidity label and value
  char humidity_label[] = "HUMIDITY:";
  char humidity_value[10];
  snprintf(humidity_value, sizeof(humidity_value), "%d %%", handle->humidity_value);
  
  // Calculate the total width of the humidity display
  int humidity_label_width = strlen(humidity_label) * 6;
  
  // Draw humidity label
  GLIB_drawString(&glibContext, humidity_label, strlen(humidity_label), 
                 2, humidity_row_y, false);
  
  // Switch to larger font for value
  GLIB_setFont(&glibContext, (GLIB_Font_t *)&GLIB_FontNormal8x8);
  
  // Draw humidity value
  if(co2_value != 0) {
    GLIB_drawString(&glibContext, humidity_value, strlen(humidity_value), 
                    2 + humidity_label_width + 4, humidity_row_y, false);
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
