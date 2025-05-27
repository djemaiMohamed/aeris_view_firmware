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

typedef struct {
  char  *date;
  uint8_t battery_level;
  uint16_t co2_value;
  uint8_t humidity_value;
  float temperature_value;
} memlcd_handle_t;

/***************************************************************************//**
 * Initialize example
 ******************************************************************************/
void memlcd_draw_data(memlcd_handle_t *handle);

/***************************************************************************//**
 * Display BLE passkey on LCD
 ******************************************************************************/
void memlcd_display_passkey(uint32_t passkey);

/***************************************************************************//**
 * Display waiting for measurement message on LCD
 ******************************************************************************/
void memlcd_display_waiting_message(void);

#endif // MEMLCD_APP_H
