#include "lcd_task.h"
#include "sensor_task.h"
#include "app.h"
#include "app_log.h"
#include "app_assert.h"
#include <os.h>
#include "memlcd.h"
#include <stdio.h>
#include <string.h>
#include "sl_memory_manager.h" // Added for dynamic allocation
#include "sl_bt_api.h" // Added for SL_BT_INVALID_CONNECTION_HANDLE

// Semaphores for synchronization
extern OS_SEM sensor_data_ready_sem;
extern OS_SEM ble_notification_sem;
OS_TCB *lcd_task_tcb_ptr;
CPU_STK *lcd_task_stk_ptr;
extern uint16_t measurement_interval;
extern uint8_t connection_handle;
extern uint8_t temperature_unit;
extern bool suspend_lcd;

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
  // Format measurement interval as "Xs" or "Xm Xs" based on value
  if (interval_seconds < 60) {
    snprintf(measurement_interval_str, sizeof(measurement_interval_str), 
             "%ds", interval_seconds);
  } else {
    int minutes = interval_seconds / 60;
    int seconds = interval_seconds % 60;
    if (seconds == 0) {
      snprintf(measurement_interval_str, sizeof(measurement_interval_str), 
               "%dm", minutes);
    } else {
      snprintf(measurement_interval_str, sizeof(measurement_interval_str), 
               "%dm %ds", minutes, seconds);
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
    app_log_warning("Trying to update LCD with invalid sensor data\n");
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

  // Draw data to LCD
  memlcd_draw_data(&lcd_data);
  
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
  RTOS_ERR err;
  sensor_data_t *sensor_data;

  app_log_info("LCD task started.\n");
  
  // Initialize the LCD display
  lcd_init_display();

  while (DEF_TRUE) {

    // Wait for the semaphore that indicates new sensor data is available
    OSSemPend(&sensor_data_ready_sem,
              0,                    // Wait indefinitely
              OS_OPT_PEND_BLOCKING,
              NULL,
              &err);
    
    // Get the latest sensor readings
    sensor_data = sensor_get_latest_readings();
    
    if (suspend_lcd) {
      // Skip updating LCD and sending notifications.
      app_log_info(" Skip LCD update.\n");
      continue;
    }
    // Update the LCD with the new data
    lcd_update_display(sensor_data);

    // if connection handle is valid and pairing mode is false
    if (connection_handle != SL_BT_INVALID_CONNECTION_HANDLE) {
      // Trigger BLE notifications to send updated data
      app_log_info("Triggering BLE notifications for updated sensor data.\n");
      // Triger notification task to send notifications
      OSSemPost(&ble_notification_sem, OS_OPT_POST_1, &err);
    } else {
      app_log_info("No connection. Skipping BLE notifications.\n");
    }
  }
}
