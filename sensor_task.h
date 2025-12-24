#ifndef SENSOR_TASK_H
#define SENSOR_TASK_H

#include <os.h>
#include <stdint.h>
#include <stdbool.h>
#include "sl_status.h"

// Task constants
#define SENSOR_TASK_PRIO              20u
#define SENSOR_TASK_STK_SIZE          192u

// SCD I2C ADDRESS
#define SCD_ADDR                      0x62

// Semaphores for synchronization
extern OS_SEM sensor_data_ready_sem;
extern OS_SEM ble_notification_sem;
extern OS_SEM measurement_sem;

// Sensor data structure
typedef struct {
  uint16_t co2_ppm;
  int32_t temperature_mdeg_c;  // Temperature in milli-degrees Celsius
  uint32_t humidity_percent;   // Relative humidity in percent
  uint32_t battery_voltage_mv; // Battery voltage in millivolts
  uint8_t battery_percentage;  // Battery percentage (0-100)
  bool valid_data;             // Flag indicating if data is valid
} sensor_data_t;

// Function prototypes
void sensor_task_init(void);
void sensor_task(void *p_arg);

// Sensor operation functions
int16_t sensor_init(void);
int16_t sensor_trigger_measurement(void);
int16_t sensor_trigger_periodic_measurement(void);
int16_t sensor_read_measurement(sensor_data_t *data);
sensor_data_t* sensor_get_latest_readings(void);

// Battery functions
void battery_init(void);
void battery_deinit(void);
uint32_t battery_read_voltage(void);
uint8_t battery_calculate_percentage(uint32_t voltage_mv);
sl_status_t sensor_restart_measurement_timer(uint16_t interval_sec);

// Global variables
extern uint16_t measurement_interval;
extern uint8_t temperature_unit;
extern int16_t temperature_offset_mdeg;

#endif // SENSOR_TASK_H
