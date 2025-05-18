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

// Semaphores for synchronization
extern OS_SEM sensor_data_ready_sem;
OS_TCB *lcd_task_tcb_ptr;
CPU_STK *lcd_task_stk_ptr;
extern OS_SEM bond_sem;
extern uint16_t measurement_interval;
extern bool pairing_mode;

// Variables for LCD display data
static memlcd_handle_t lcd_data;
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
  
  // Update measurement interval string with default interval (5 seconds)
  lcd_update_measurement_interval_string(measurement_interval);
  
  // Draw initial display
  memlcd_draw_data(&lcd_data);
  
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
 * Format sensor measurement values for display.
 * @param[in] sensor_data Pointer to the sensor data structure
 * @param[out] co2_str Buffer to store CO2 string (can be NULL if not needed)
 * @param[out] temp_str Buffer to store temperature string (can be NULL if not needed)
 * @param[out] humidity_str Buffer to store humidity string (can be NULL if not needed)
 *****************************************************************************/
void lcd_format_measurement_values(sensor_data_t *sensor_data, 
                                  char *co2_str, 
                                  char *temp_str, 
                                  char *humidity_str)
{
  if (co2_str != NULL) {
    snprintf(co2_str, 16, "%u ppm", sensor_data->co2_ppm);
  }
  
  if (temp_str != NULL) {
    snprintf(temp_str, 16, "%ld.%ld°C", 
             sensor_data->temperature_mdeg_c / 1000, 
             (sensor_data->temperature_mdeg_c % 1000) / 100);
  }
  
  if (humidity_str != NULL) {
    snprintf(humidity_str, 16, "%lu%%", sensor_data->humidity_percent);
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
  lcd_data.temperature_value = (float)sensor_data->temperature_mdeg_c / 1000.0f; // Convert from milli-degrees to degrees
  lcd_data.humidity_value = sensor_data->humidity_percent;
  // Update measurement interval string
  lcd_update_measurement_interval_string(measurement_interval);
  // Update LCD data structure with measurement interval string
  lcd_data.date = measurement_interval_str;

  // Draw data to LCD
  memlcd_draw_data(&lcd_data);
  
  app_log_info("LCD display updated - CO2: %u ppm, Temp: %ld.%ld°C, Humidity: %lu%%\n",
               sensor_data->co2_ppm,
               sensor_data->temperature_mdeg_c / 1000,
               (sensor_data->temperature_mdeg_c % 1000) / 100,
               sensor_data->humidity_percent);
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
    
    // Update the LCD with the new data
    lcd_update_display(sensor_data);

    // Signal reconnecting to the phone
    OSSemPost(&bond_sem, OS_OPT_POST_1, &err);
  }
}
