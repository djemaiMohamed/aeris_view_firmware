#include "lcd_task.h"
#include "sensor_task.h"
#include "app.h"
#include "app_log.h"
#include "app_assert.h"
#include <os.h>
#include "memlcd.h"
#include <stdio.h>
#include <string.h>
#include "sl_memory_manager.h"
#include "sl_bt_api.h"

// CO2 alert thresholds (ppm)
#define CO2_BAD_THRESHOLD   2000u   // above this → bad air quality; start alert
#define CO2_GOOD_THRESHOLD  1500u   // below this → recovered; hysteresis band

// Alert animation: 10 × 500ms toggles = 5 s, then stay inverted
#define ALERT_TOGGLE_COUNT  10u

typedef enum {
    CO2_ALERT_GOOD,   // normal polarity; waiting for data
    CO2_ALERT_ANIM,   // toggling polarity every 500ms for 5 s
    CO2_ALERT_BAD,    // polarity locked inverted; waiting for recovery
} co2_alert_state_t;

// Semaphores for synchronization
extern OS_SEM sensor_data_ready_sem;
extern OS_SEM ble_notification_sem;
OS_TCB *lcd_task_tcb_ptr;
CPU_STK *lcd_task_stk_ptr;
extern uint16_t measurement_interval;
extern uint8_t connection_handle;
extern uint8_t temperature_unit;
extern uint8_t display_type;

// Variables for LCD display data
memlcd_handle_t lcd_data;
static char measurement_interval_str[32];

/**************************************************************************//**
 * Initialize the LCD task and related resources.
 *****************************************************************************/
void lcd_task_init(void)
{
  RTOS_ERR err;

  // Dynamically allocate TCB
  lcd_task_tcb_ptr = (OS_TCB *)sl_malloc(sizeof(OS_TCB));
  app_assert(lcd_task_tcb_ptr != DEF_NULL, "Failed to allocate TCB for LCD task");

  // Dynamically allocate stack
  lcd_task_stk_ptr = (CPU_STK *)sl_malloc(LCD_TASK_STK_SIZE * sizeof(CPU_STK));
  app_assert(lcd_task_stk_ptr != DEF_NULL, "Failed to allocate stack for LCD task");
  
  app_log_info("Initializing LCD task...\n");
  
  // Create the LCD task
  OSTaskCreate(lcd_task_tcb_ptr, // Pass pointer to allocated TCB
               "LCD Task",
               lcd_task,
               DEF_NULL,
               LCD_TASK_PRIO,
               lcd_task_stk_ptr, // Pass pointer to allocated stack
               (LCD_TASK_STK_SIZE / 10u),
               LCD_TASK_STK_SIZE,
               0u,
               0u,
               DEF_NULL,
               (OS_OPT_TASK_STK_CLR | OS_OPT_TASK_STK_CHK),
               &err);
  app_assert(err.Code == RTOS_ERR_NONE, "Failed to create LCD task");
}

/**************************************************************************//**
 * Initialize the LCD display.
 *****************************************************************************/
void lcd_init_display(void)
{
  // Initialize LCD data structure
  memset(&lcd_data, 0, sizeof(lcd_data));
  
  // Set initial values for display
  lcd_data.battery_level = 100;
  lcd_data.co2_value = 0;
  lcd_data.humidity_value = 0;
  lcd_data.temperature_value = 0;
  lcd_data.history_index = 0;
  lcd_data.history_count = 0;
  
  // Update measurement interval string with default interval (5 seconds)
  lcd_update_measurement_interval_string(measurement_interval);
  
  // Draw initial display
  //memlcd_draw_data(&lcd_data);
  memlcd_display_waiting_message();
  
  app_log_info("LCD initialized with default values\n");
}

/**************************************************************************//**
 * Format the measurement interval string for display.
 * @param[in] interval_seconds Measurement interval in seconds
 *****************************************************************************/
void lcd_update_measurement_interval_string(uint16_t interval_seconds)
{
  // Format measurement interval as "Xsec" or "Xmin" or "Xmin Xsec"
  if (interval_seconds < 60) {
    snprintf(measurement_interval_str, sizeof(measurement_interval_str),
             "%dsec", interval_seconds);
  } else {
    int minutes = interval_seconds / 60;
    int seconds = interval_seconds % 60;
    if (seconds == 0) {
      snprintf(measurement_interval_str, sizeof(measurement_interval_str),
               "%dmin", minutes);
    } else {
      snprintf(measurement_interval_str, sizeof(measurement_interval_str),
               "%dmin %dsec", minutes, seconds);
    }
  }
}

/**************************************************************************//**
 * Update the LCD display with sensor data.
 * @param[in] sensor_data Pointer to the sensor data structure
 *****************************************************************************/
void lcd_update_display(sensor_data_t *sensor_data)
{
  if (!sensor_data->valid_data) {
    app_log_warning("No valid sensor data yet; showing waiting screen\n");
    memlcd_display_waiting_message();
    return;
  }
  
  // Update LCD data structure with latest measurements
  lcd_data.co2_value = sensor_data->co2_ppm;
  
  // Convert temperature based on unit setting
  if (temperature_unit == 0) {
    lcd_data.temperature_value = (float)sensor_data->temperature_mdeg_c / 1000.0f; // Celsius
  } else {
    // Convert to Fahrenheit: F = C * 1.8 + 32
    float temp_celsius = (float)sensor_data->temperature_mdeg_c / 1000.0f;
    lcd_data.temperature_value = temp_celsius * 1.8f + 32.0f; // Fahrenheit
  }
  
  lcd_data.humidity_value = sensor_data->humidity_percent;
  lcd_data.battery_level = sensor_data->battery_percentage; // Update battery level from sensor data
  lcd_data.bluetooth_connected = (connection_handle != SL_BT_INVALID_CONNECTION_HANDLE) ? 1 : 0; // Update bluetooth connection status
  // Update measurement interval string
  lcd_update_measurement_interval_string(measurement_interval);
  // Update LCD data structure with measurement interval string
  lcd_data.date = measurement_interval_str;
  lcd_data.measurement_interval_s = measurement_interval;

  memlcd_add_co2_to_history(&lcd_data, lcd_data.co2_value);

  // Draw data to LCD based on display type
  if (display_type == 1) {
    memlcd_draw_data_simple(&lcd_data);
  } else {
    memlcd_draw_data(&lcd_data);
  }
  
  // Log temperature in the selected unit for display purposes
  if (temperature_unit == 0) {
    // Log in Celsius
    app_log_info("LCD display updated - CO2: %u ppm, Temp: %ld.%ld°C, Humidity: %lu%%, Battery: %d%%, BT: %s\n",
                 sensor_data->co2_ppm,
                 sensor_data->temperature_mdeg_c / 1000,
                 (sensor_data->temperature_mdeg_c % 1000) / 100,
                 sensor_data->humidity_percent,
                 sensor_data->battery_percentage,
                 lcd_data.bluetooth_connected ? "Connected" : "Disconnected");
  } else {
    // Convert to Fahrenheit for logging
    int32_t temp_fahrenheit_mdeg = (sensor_data->temperature_mdeg_c * 18) / 10 + 32000;
    app_log_info("LCD display updated - CO2: %u ppm, Temp: %ld.%ld°F, Humidity: %lu%%, Battery: %d%%, BT: %s\n",
                 sensor_data->co2_ppm,
                 temp_fahrenheit_mdeg / 1000,
                 (temp_fahrenheit_mdeg % 1000) / 100,
                 sensor_data->humidity_percent,
                 sensor_data->battery_percentage,
                 lcd_data.bluetooth_connected ? "Connected" : "Disconnected");
  }
}

/**************************************************************************//**
 * LCD task - handles display updates based on sensor data.
 *****************************************************************************/
void lcd_task(void *p_arg)
{
  (void)p_arg;

  RTOS_ERR err;
  sensor_data_t     *sensor_data;
  co2_alert_state_t  alert_state = CO2_ALERT_GOOD;
  uint32_t           anim_count  = 0;

  app_log_info("LCD task started.\n");
  lcd_init_display();

  // 500ms in OS ticks — used as pend timeout during alert animation
  OS_TICK alert_ticks = OSTimeTickRateHzGet(&err) / 2u;

  while (DEF_TRUE) {
    // During animation, wake every 500ms to toggle polarity even without new data.
    OS_TICK timeout = (alert_state == CO2_ALERT_ANIM) ? alert_ticks : 0u;

    OSSemPend(&sensor_data_ready_sem, timeout, OS_OPT_PEND_BLOCKING, NULL, &err);

    if (err.Code == RTOS_ERR_TIMEOUT) {
      // Animation tick: flip polarity and redraw with last known sensor data
      memlcd_set_polarity_inverted(!memlcd_get_polarity_inverted());
      anim_count++;

      if (anim_count >= ALERT_TOGGLE_COUNT) {
        // 5 seconds elapsed — lock into inverted state
        memlcd_set_polarity_inverted(true);
        alert_state = CO2_ALERT_BAD;
        app_log_info("CO2 alert: animation done, display locked inverted\n");
      }

      sensor_data = sensor_get_latest_readings();
      if (sensor_data->valid_data) {
        lcd_update_display(sensor_data);
      }
      continue;
    }

    if (err.Code != RTOS_ERR_NONE) {
      continue;
    }

    // New sensor data available
    sensor_data = sensor_get_latest_readings();
    lcd_update_display(sensor_data);

    // CO2 alert state machine
    if (sensor_data->valid_data) {
      uint16_t co2 = sensor_data->co2_ppm;

      switch (alert_state) {
        case CO2_ALERT_GOOD:
          if (co2 >= CO2_BAD_THRESHOLD) {
            alert_state = CO2_ALERT_ANIM;
            anim_count  = 0;
            memlcd_set_polarity_inverted(false); // first tick will toggle to inverted
            app_log_info("CO2 bad (%u ppm) — starting 5s polarity alert animation\n", co2);
          }
          break;

        case CO2_ALERT_ANIM:
        case CO2_ALERT_BAD:
          if (co2 < CO2_GOOD_THRESHOLD) {
            alert_state = CO2_ALERT_GOOD;
            anim_count  = 0;
            memlcd_set_polarity_inverted(false);
            lcd_update_display(sensor_data); // redraw immediately with normal polarity
            app_log_info("CO2 recovered (%u ppm) — normal polarity restored\n", co2);
          }
          break;
      }
    }

    if (connection_handle != SL_BT_INVALID_CONNECTION_HANDLE) {
      app_log_info("Triggering BLE notifications for updated sensor data.\n");
      OSSemPost(&ble_notification_sem, OS_OPT_POST_1, &err);
    } else {
      app_log_info("No connection. Skipping BLE notifications.\n");
    }
  }
}
