#include "sensor_task.h"
#include "app.h"
#include "app_log.h"
#include <os.h>
#include "scd4x_i2c.h"
#include "sensirion_common.h"
#include "sensirion_i2c_hal.h"
#include "app_assert.h"
#include "sl_memory_manager.h" // Added for dynamic allocation

// Semaphores for synchronization
OS_SEM sensor_data_ready_sem;
OS_TCB *sensor_task_tcb_ptr;
CPU_STK *sensor_task_stk_ptr;// measurement interval in seconds

// Global sensor data accessible to other tasks
static sensor_data_t sensor_readings = {0};
// measurement interval in seconds
uint16_t measurement_interval = 30; // Default to 5 seconds

/**************************************************************************//**
 * Initialize the SCD4x sensor for single shot measurements.
 * @return Status code (0 = success, otherwise error)
 *****************************************************************************/
int16_t sensor_init(void)
{
  int16_t error;
  
  app_log_info("Initializing SCD4x sensor...\n");
  
  // Initialize the I2C communication
  sensirion_i2c_hal_init();
  
  // Initialize the SCD4x sensor
  scd4x_init(SCD_ADDR); // SCD41_I2C_ADDR_62

  
  // Wake up the sensor if it was in sleep mode
  error = scd4x_wake_up();
  if (error) {
    app_log_error("Error waking up sensor: %d\n", error);
    return error;
  }
  
  // Stop any ongoing periodic measurement
  error = scd4x_stop_periodic_measurement();
  if (error) {
    app_log_error("Error stopping periodic measurement: %d\n", error);
    return error;
  }
  
  // Reinitialize sensor
  error = scd4x_reinit();
  if (error) {
    app_log_error("Error reinitializing sensor: %d\n", error);
    return error;
  }
  
  // Enable automatic self-calibration
  error = scd4x_set_automatic_self_calibration_enabled(1);
  if (error) {
    app_log_error("Error enabling self-calibration: %d\n", error);
    // Continue despite this error since it's not critical
  }
  
  app_log_info("SCD4x sensor initialized for single shot measurement mode\n");
  return 0; // Success
}

/**************************************************************************//**
 * Trigger a single shot measurement on the SCD4x sensor.
 * @return Status code (0 = success, otherwise error)
 *****************************************************************************/
int16_t sensor_trigger_measurement(void)
{
  int16_t error;
  
  error = scd4x_measure_single_shot();
  if (error) {
    app_log_error("Error triggering single shot measurement: %d\n", error);
    return error;
  }
  
  app_log_info("Single shot measurement triggered\n");
  return 0; // Success
}

/**************************************************************************//**
 * Trigger a single shot measurement on the SCD4x sensor.
 * @return Status code (0 = success, otherwise error)
 *****************************************************************************/
int16_t sensor_trigger_periodic_measurement(void)
{
  int16_t error;
  
  error = scd4x_start_low_power_periodic_measurement();
  if (error) {
    app_log_error("Error triggering periodic  measurement: %d\n", error);
    return error;
  }
  
  app_log_info("Periodic measurement triggered\n");
  return 0; // Success
}

/**************************************************************************//**
 * Read measurement data from the SCD4x sensor.
 * @param[out] data Pointer to sensor_data_t structure to store the readings
 * @return Status code (0 = success, otherwise error)
 *****************************************************************************/
int16_t sensor_read_measurement(sensor_data_t *data)
{
  int16_t error;
  bool data_ready = false;
  
  // Check if data is ready
  error = scd4x_get_data_ready_status(&data_ready);
  if (error) {
    app_log_error("Error checking data ready status: %d\n", error);
    data->valid_data = false;
    return error;
  }
  
  if (!data_ready) {
    app_log_warning("Sensor data not ready yet\n");
    data->valid_data = false;
    return 1; // Not ready but not an error
  }
  
  // Read measurement values
  uint16_t co2;
  int32_t temperature;
  int32_t humidity;
  
  error = scd4x_read_measurement(&co2, &temperature, &humidity);
  if (error) {
    app_log_error("Error reading measurement data: %d\n", error);
    data->valid_data = false;
    return error;
  }
  
  // Store readings in the data structure
  data->co2_ppm = co2;
  data->temperature_mdeg_c = temperature;
  data->humidity_percent = humidity / 1000; // Convert from milli-percent to percent
  data->valid_data = true;
  
  app_log_info("Measurement read - CO2: %u ppm, Temp: %ld.%ld°C, Humidity: %lu%%\n", 
               data->co2_ppm, 
               data->temperature_mdeg_c / 1000, 
               (data->temperature_mdeg_c % 1000) / 100,
               data->humidity_percent);
               
  return 0; // Success
}

/**************************************************************************//**
 * Get the latest sensor readings.
 * @return Pointer to the sensor data structure
 *****************************************************************************/
sensor_data_t* sensor_get_latest_readings(void)
{
  return &sensor_readings;
}

/**************************************************************************//**
 * Initialize the sensor task and related resources.
 *****************************************************************************/
void sensor_task_init(void)
{
  RTOS_ERR err;

  // Dynamically allocate TCB
  sensor_task_tcb_ptr = (OS_TCB *)sl_malloc(sizeof(OS_TCB));
  app_assert(sensor_task_tcb_ptr != DEF_NULL, "Failed to allocate TCB for sensor task");

  // Dynamically allocate stack
  sensor_task_stk_ptr = (CPU_STK *)sl_malloc(SENSOR_TASK_STK_SIZE * sizeof(CPU_STK));
  app_assert(sensor_task_stk_ptr != DEF_NULL, "Failed to allocate stack for sensor task");

  app_log_info("Initializing sensor and LCD tasks...\n");
  
  // Create the semaphores
  OSSemCreate(&sensor_data_ready_sem,
              "Sensor Data Ready Sem",
              0,                     // Initial count (no data available)
              &err);
  app_assert(err.Code == RTOS_ERR_NONE, "Failed to create sensor data ready semaphore");
  
  app_assert(err.Code == RTOS_ERR_NONE, "Failed to create BLE notification semaphore");
  
  // Create the sensor task
  OSTaskCreate(sensor_task_tcb_ptr, // Pass pointer to allocated TCB
               "Sensor Task",
               sensor_task,
               DEF_NULL,
               SENSOR_TASK_PRIO,
               sensor_task_stk_ptr, // Pass pointer to allocated stack
               (SENSOR_TASK_STK_SIZE / 10u),
               SENSOR_TASK_STK_SIZE,
               0u,
               0u,
               DEF_NULL,
               (OS_OPT_TASK_STK_CLR | OS_OPT_TASK_STK_CHK),
               &err);
  app_assert(err.Code == RTOS_ERR_NONE, "Failed to create sensor task");
}

/**************************************************************************//**
 * Sensor task - handles periodic sensor reading.
 *****************************************************************************/
void sensor_task(void *p_arg)
{
  RTOS_ERR err;
  int16_t error;
  
  app_log_info("Sensor task started.\n");
  
  // Initialize the sensor
  error = sensor_init();
  if (error) {
    app_log_error("Sensor initialization failed with error %d\n", error);
    // Continue anyway - will try to recover in the main loop
  }

  // Start peridic measurement
  error = sensor_trigger_periodic_measurement();
  if (error) {
    app_log_error("Error starting periodic measurement: %d\n", error);
    // Continue anyway - will try to recover in the main loop
  }
  
  while (DEF_TRUE) {
    // Step 1: Trigger a measurement
    //error = sensor_trigger_measurement();
    //if (error) {
    //  app_log_error("Failed to trigger measurement. Will retry after delay.\n");
    //  // Wait and try again
    //  OSTimeDlyHMSM(0, 0, 5, 0, OS_OPT_TIME_HMSM_STRICT, &err);
    //  continue;
    //}
    
    // Step 2: Wait for measurement to complete
    OSTimeDlyHMSM(0, 0, measurement_interval + 1, 0, OS_OPT_TIME_HMSM_STRICT, &err);
    
    // Step 3: Read the measurement
    error = sensor_read_measurement(&sensor_readings);
    if (error == 0 && sensor_readings.valid_data) {
      // Signal that new data is available to other tasks
      OSSemPost(&sensor_data_ready_sem, OS_OPT_POST_1, &err);
      app_assert(err.Code == RTOS_ERR_NONE, "Sensor data ready semaphore post failed");
    } else if (error != 1) { // error=1 means data not ready, which is not a critical error
      app_log_warning("Failed to read measurement or data not valid. Will retry.\n");
    }
  }
}
