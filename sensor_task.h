#ifndef SENSOR_TASK_H
#define SENSOR_TASK_H

#include <os.h>
#include <stdint.h>
#include <stdbool.h>

// Task constants
#define SENSOR_TASK_PRIO              20u
#define SENSOR_TASK_STK_SIZE          256u

// SCD I2C ADDRESS
#define SCD_ADDR                      0x62
#define MEASUREMENT_INTERVAL_S        5 // Default 5 second for now

// Semaphores for synchronization
extern OS_SEM sensor_data_ready_sem;
extern OS_SEM ble_notification_sem;

// Sensor data structure
typedef struct {
  uint16_t co2_ppm;
  int32_t temperature_mdeg_c;  // Temperature in milli-degrees Celsius
  uint32_t humidity_percent;   // Relative humidity in percent
  bool valid_data;             // Flag indicating if data is valid
} sensor_data_t;

// Function prototypes
void sensor_task_init(void);
void sensor_task(void *p_arg);

// Sensor operation functions
int16_t sensor_init(void);
int16_t sensor_trigger_measurement(void);
int16_t sensor_read_measurement(sensor_data_t *data);
sensor_data_t* sensor_get_latest_readings(void);

#endif // SENSOR_TASK_H
