/***************************************************************************//**
 * @file memlcd_app.h
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

#ifndef MEMLCD_H
#define MEMLCD_H

#include <stdint.h>

#define CO2_HISTORY_SIZE 400

typedef struct {
  char  *date;
  uint8_t battery_level;
  uint8_t bluetooth_connected;  // 1 if connected, 0 if disconnected
  uint16_t co2_value;
  uint8_t humidity_value;
  float temperature_value;
  uint16_t co2_history[CO2_HISTORY_SIZE];
  uint16_t history_index;
  uint16_t history_count;
} memlcd_handle_t;

/***************************************************************************//**
 * Initialize example
 ******************************************************************************/
void memlcd_draw_data(memlcd_handle_t *handle);

/***************************************************************************//**
 * Add CO2 value to history
 ******************************************************************************/
void memlcd_add_co2_to_history(memlcd_handle_t *handle, uint16_t co2_value);

/***************************************************************************//**
 * Display BLE passkey on LCD
 ******************************************************************************/
void memlcd_display_passkey(uint32_t passkey);

/***************************************************************************//**
 * Display waiting for measurement message on LCD
 ******************************************************************************/
void memlcd_display_waiting_message(void);

/***************************************************************************//**
 * Display calibration progress message on LCD
 ******************************************************************************/
void memlcd_display_calibration_message(const char *line1, const char *line2);

// Display calibration progress with a progress bar and time remaining
void memlcd_display_calibration_progress(uint8_t percent, uint16_t seconds_remaining,
                                         uint8_t sample_idx, uint8_t sample_total);

#endif // MEMLCD_APP_H
