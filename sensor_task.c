#include "sensor_task.h"
#include "app.h"
#include "app_log.h"
#include <os.h>
#include "scd4x_i2c.h"
#include "sensirion_common.h"
#include "sensirion_i2c_hal.h"
#include "app_assert.h"
#include "sl_memory_manager.h" // Added for dynamic allocation
#include "em_device.h"
#include "em_cmu.h"
#include "em_iadc.h"

// Battery measurement defines
#define MICROVOLTS_PER_STEP 1221

// Semaphores for synchronization
OS_SEM sensor_data_ready_sem;
OS_TCB *sensor_task_tcb_ptr;
CPU_STK *sensor_task_stk_ptr;// measurement interval in seconds

// Global sensor data accessible to other tasks
static sensor_data_t sensor_readings = {0};
// measurement interval in seconds
uint16_t measurement_interval = 5; // Default to 5 seconds
// temperature unit (0 = Celsius, 1 = Fahrenheit)
uint8_t temperature_unit = 0; // Default to Celsius

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
 * Initialize IADC for battery voltage measurement.
 *****************************************************************************/
void battery_init(void)
{
  // Enable IADC0 clock branch
  CMU_ClockEnable(cmuClock_IADC0, true);

  // Declare init structs
  IADC_Init_t init = IADC_INIT_DEFAULT;
  IADC_AllConfigs_t initAllConfigs = IADC_ALLCONFIGS_DEFAULT;
  IADC_InitSingle_t initSingle = IADC_INITSINGLE_DEFAULT;

  // Single input structure
  IADC_SingleInput_t singleInput = IADC_SINGLEINPUT_DEFAULT;

  // Initialize IADC
  IADC_init(IADC0, &init, &initAllConfigs);

  // Configure single input
  singleInput.posInput = iadcPosInputAvdd;      // Add AVDD to GPIO so it can be measured
  singleInput.negInput = iadcNegInputGnd;       // Use ground as negative input

  // Initialize single conversion
  IADC_initSingle(IADC0, &initSingle, &singleInput);
  
  app_log_info("Battery IADC initialized\n");
}

/**************************************************************************//**
 * Deinitialize IADC for battery voltage measurement.
 *****************************************************************************/
void battery_deinit(void)
{
  // Reset IADC to disable the module
  //IADC_disable(IADC0);
  IADC0->EN_CLR = IADC_EN_EN;
  
  // Disable IADC0 clock branch to save power
  CMU_ClockEnable(cmuClock_IADC0, false);
  
  app_log_info("Battery IADC deinitialized\n");
}

/**************************************************************************//**
 * Read IADC value for battery voltage.
 * @return IADC reading
 *****************************************************************************/
static uint32_t battery_read_adc(void)
{
  IADC_Result_t sample;
  
  // Start IADC conversion
  IADC_command(IADC0, iadcCmdStartSingle);

  // Wait for conversion to be complete
  while((IADC0->STATUS & (_IADC_STATUS_CONVERTING_MASK | _IADC_STATUS_SINGLEFIFODV_MASK)) != _IADC_STATUS_SINGLEFIFODV_MASK);

  // Get ADC result
  sample = IADC_pullSingleFifoResult(IADC0);

  // Convert to 12-bit unsigned value for compatibility
  return sample.data;
}

/**************************************************************************//**
 * Read battery voltage in millivolts.
 * @return Battery voltage in millivolts
 *****************************************************************************/
uint32_t battery_read_voltage(void)
{
  uint32_t raw_reading = battery_read_adc();
  uint32_t supply_voltage_mV = raw_reading * MICROVOLTS_PER_STEP / 1000UL;
  return supply_voltage_mV;
}

/**************************************************************************//**
 * Calculate battery percentage from voltage.
 * @param voltage_mv Battery voltage in millivolts
 * @return Battery percentage (0-100)
 *****************************************************************************/
uint8_t battery_calculate_percentage(uint32_t voltage_mv)
{
  // Simple linear calculation: assume 3000mV = 100%
  // You may need to adjust this based on your battery characteristics
  if (voltage_mv >= 3000) {
    return 100;
  } else if (voltage_mv < 2400) {
    return 0;
  } else {
    return (uint8_t)((voltage_mv - 2400) * 100 / (3000 - 2400));
  }
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

  // Initialize battery ADC
  //battery_init();

  // Start peridic measurement
  //error = sensor_trigger_periodic_measurement();
  if (error) {
    app_log_error("Error starting periodic measurement: %d\n", error);
    // Continue anyway - will try to recover in the main loop
  }
  
  while (DEF_TRUE) {
    // wakeup the sensor if it was in sleep mode
    error = scd4x_wake_up();
    // Step 1: Trigger a measurement
    error = sensor_trigger_measurement();
    if (error) {
      app_log_error("Failed to trigger measurement. Will retry after delay.\n");
      // Wait and try again
      OSTimeDlyHMSM(0, 0, 5, 0, OS_OPT_TIME_HMSM_STRICT, &err);
      continue;
    }
    // sleep for 5 seconds to allow measurement to complete
    OSTimeDlyHMSM(0, 0, 5, 0, OS_OPT_TIME_HMSM_STRICT, &err);
    
    // Step 3: Read the measurement
    error = sensor_read_measurement(&sensor_readings);
    // power down the sensor after reading
    scd4x_power_down();

    OSTimeDlyHMSM(0, 0, measurement_interval + 1 - 5, 0, OS_OPT_TIME_HMSM_STRICT, &err);

    if (error == 0 && sensor_readings.valid_data) {
      // Add battery measurement
      battery_init();
      sensor_readings.battery_voltage_mv = battery_read_voltage();
      battery_deinit();
      sensor_readings.battery_percentage = battery_calculate_percentage(sensor_readings.battery_voltage_mv);
      app_log_info("Battery: %lu mV (%d%%)\n", sensor_readings.battery_voltage_mv, sensor_readings.battery_percentage);
      
      // Signal that new data is available to other tasks
      OSSemPost(&sensor_data_ready_sem, OS_OPT_POST_1, &err);
      app_assert(err.Code == RTOS_ERR_NONE, "Sensor data ready semaphore post failed");
    } else if (error != 1) { // error=1 means data not ready, which is not a critical error
      app_log_warning("Failed to read measurement or data not valid. Will retry.\n");
    }
  }
}
