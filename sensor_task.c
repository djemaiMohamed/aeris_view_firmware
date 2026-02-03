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
#include "em_gpio.h"
#include "sync_data_task.h"
#include "sl_sleeptimer.h"
#include "sl_status.h"

// Battery measurement defines
// New design:
// - PB0: battery sense node (R5/R6 divider)
// - PB1: divider bottom switch; drive low only during measurement, otherwise hi-Z
#define BATTERY_SENSE_PORT gpioPortB
#define BATTERY_SENSE_PIN  0
#define BATTERY_SW_PORT    gpioPortB
#define BATTERY_SW_PIN     1

#define BATTERY_R5_OHMS 100000UL
#define BATTERY_R6_OHMS 150000UL

// IADC single results are 12-bit aligned by default
#define BATTERY_IADC_COUNTS_FULL_SCALE 4096UL

// If VDDX is used as IADC reference, set this to your regulated VMCU/VDDX in mV.
// Default is 3000mV (matches emlib's nominal VDDX reference value).
#ifndef BATTERY_VDDX_MV
#define BATTERY_VDDX_MV 3000UL
#endif

// Set to 1 to include extra ADC debug logging
#ifndef BATTERY_MONITOR_LOG_DEBUG
#define BATTERY_MONITOR_LOG_DEBUG 0
#endif

// Divider/node settling delay after enabling PB1 GND switch.
// Increase this if you have an RC filter on the sense node.
#ifndef BATTERY_SETTLE_DELAY_MS
#define BATTERY_SETTLE_DELAY_MS 20U
#endif

// Set to 1 if the external divider-to-GND switch is active-high (e.g., PB1 drives a MOSFET gate).
// Set to 0 if PB1 directly pulls the divider bottom to GND when driven low.
#ifndef BATTERY_SW_ACTIVE_HIGH
#define BATTERY_SW_ACTIVE_HIGH 1
#endif

// If computed battery voltage is below this, print extra diagnostics.
#ifndef BATTERY_DIAG_BELOW_MV
#define BATTERY_DIAG_BELOW_MV 3300UL
#endif

// Optional calibration factor applied to computed battery voltage.
// Example: if DMM=4620mV and firmware=4465mV, scale ~= 4620/4465 = 1.0347.
// Set BATTERY_CAL_NUM=1035 and BATTERY_CAL_DEN=1000.
#ifndef BATTERY_CAL_NUM
#define BATTERY_CAL_NUM 1035UL
#endif
#ifndef BATTERY_CAL_DEN
#define BATTERY_CAL_DEN 1000UL
#endif

// Prefer the lowest available analog gain to increase allowable input range.
// Approx full-scale input multiplier vs Vref:
// - gain 0.25x => ~4 * Vref
// - gain 0.5x  => ~2 * Vref
// - gain 1x    => ~1 * Vref
#if defined(_IADC_CFG_ANALOGGAIN_ANAGAIN0P25)
  #define BATTERY_IADC_ANALOG_GAIN        iadcCfgAnalogGain0P25x
  #define BATTERY_IADC_INPUT_SCALE_NUM   4UL
  #define BATTERY_IADC_REFSEL             iadcCfgReferenceInt1V2
  #define BATTERY_IADC_VREF_MV            1210UL
#else
  #define BATTERY_IADC_ANALOG_GAIN        iadcCfgAnalogGain0P5x
  #define BATTERY_IADC_INPUT_SCALE_NUM   2UL
  // With only 0.5x gain available, the PB0 node (~0.6*Vbat) can exceed 2*1.2V and saturate,
  // producing a plateau around ~4.03V on the computed battery voltage. Use VDDX as reference.
  #define BATTERY_IADC_REFSEL             iadcCfgReferenceVddx
  #define BATTERY_IADC_VREF_MV            BATTERY_VDDX_MV
#endif

// Semaphores for synchronization
OS_SEM sensor_data_ready_sem;
OS_SEM measurement_sem;
OS_TCB *sensor_task_tcb_ptr;
CPU_STK *sensor_task_stk_ptr;// measurement interval in seconds

// Global sensor data accessible to other tasks
static sensor_data_t sensor_readings = {0};
// measurement interval in seconds
uint16_t measurement_interval = 7; // Default to 7 seconds
// temperature unit (0 = Celsius, 1 = Fahrenheit)
uint8_t temperature_unit = 0; // Default to Celsius

// Timer handle for scheduling measurements
static sl_sleeptimer_timer_handle_t measurement_timer_handle;

// Timer callback: post the measurement semaphore from sleeptimer context
static void measurement_timer_cb(sl_sleeptimer_timer_handle_t *handle, void *data)
{
  (void)handle;
  (void)data;

  RTOS_ERR err;
  OSSemPost(&measurement_sem, OS_OPT_POST_1, &err);
}

sl_status_t sensor_restart_measurement_timer(uint16_t interval_sec)
{
  sl_status_t st;

  // Stop existing timer if running
  st = sl_sleeptimer_stop_timer(&measurement_timer_handle);
  if ((st != SL_STATUS_OK) && (st != SL_STATUS_INVALID_STATE)) {
    return st;
  }

  st = sl_sleeptimer_start_periodic_timer_ms(&measurement_timer_handle,
                                             (uint32_t)interval_sec * 1000UL,
                                             measurement_timer_cb,
                                             NULL,
                                             0,
                                             0);
  if (st != SL_STATUS_OK) {
    return st;
  }

  // Kick off an immediate measurement after interval change
  RTOS_ERR err;
  OSSemPost(&measurement_sem, OS_OPT_POST_1, &err);
  (void)err;

  return SL_STATUS_OK;
}

sl_status_t sensor_stop_measurement_timer(void)
{
  sl_status_t st = sl_sleeptimer_stop_timer(&measurement_timer_handle);
  if ((st != SL_STATUS_OK) && (st != SL_STATUS_INVALID_STATE)) {
    return st;
  }

  return SL_STATUS_OK;
}

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
  
  // Set temperature offset to compensate for device enclosure effects
  // Formula: raw_value = (offset_celsius * 65535) / 175
  // Reduced from 4°C to 2°C offset because sensor was reading 2°C below actual.
  // 2°C offset = (2 * 65535) / 175 = 749
  uint16_t temp_offset_raw = 749;  // 2°C offset (adjusted: sensor was reading 2°C low)
  error = scd4x_set_temperature_offset_raw(temp_offset_raw);
  if (error) {
    app_log_error("Error setting temperature offset: %d\n", error);
    // Continue despite this error
  } else {
    app_log_info("Temperature offset set to raw value %u (~%.1f°C)\n", 
                 temp_offset_raw, (float)(temp_offset_raw * 175) / 65535.0f);
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

// IADC helpers using PB0/PB1 divider (R5/R6) and configurable reference/gain
static uint32_t battery_read_adc(void)
{
  IADC_Result_t sample;

  IADC_command(IADC0, iadcCmdStartSingle);
  while ((IADC0->STATUS & (_IADC_STATUS_CONVERTING_MASK | _IADC_STATUS_SINGLEFIFODV_MASK))
         != _IADC_STATUS_SINGLEFIFODV_MASK) {}

  sample = IADC_pullSingleFifoResult(IADC0);
  return sample.data; // 12-bit aligned
}

static uint32_t battery_read_voltage_with_sw_level(bool sw_high)
{
  RTOS_ERR err;
  uint32_t sw_out = sw_high ? 1U : 0U;

  GPIO_PinModeSet(BATTERY_SW_PORT, BATTERY_SW_PIN, gpioModePushPull,
                  (unsigned int)sw_out);
  OSTimeDlyHMSM(0, 0, 0, BATTERY_SETTLE_DELAY_MS, OS_OPT_TIME_HMSM_STRICT, &err);
  (void)err;

  uint32_t raw_reading = battery_read_adc();
  GPIO_PinModeSet(BATTERY_SW_PORT, BATTERY_SW_PIN, gpioModeDisabled, 0);

  uint32_t vnode_mv = (raw_reading * (BATTERY_IADC_VREF_MV * BATTERY_IADC_INPUT_SCALE_NUM))
                      / BATTERY_IADC_COUNTS_FULL_SCALE;
  uint32_t vbat_mv = (vnode_mv * (BATTERY_R5_OHMS + BATTERY_R6_OHMS)) / BATTERY_R6_OHMS;

  // Apply calibration (rounded)
  vbat_mv = (uint32_t)((vbat_mv * BATTERY_CAL_NUM + (BATTERY_CAL_DEN / 2UL))
                       / BATTERY_CAL_DEN);

  return (vbat_mv & 0xFFFFFU) | ((raw_reading & 0xFFFU) << 20);
}

void battery_init(void)
{
  // Ensure GPIO clock is enabled for PB0/PB1 configuration
  CMU_ClockEnable(cmuClock_GPIO, true);

  // Route Port B (even pins group 0 incl. PB0) onto the analog bus for ADC0/IADC.
#if defined(GPIO_BBUSALLOC_BEVEN0_ADC0)
  GPIO->BBUSALLOC_SET = GPIO_BBUSALLOC_BEVEN0_ADC0;
#endif

  // PB0: disable digital (analog input)
  GPIO_PinModeSet(BATTERY_SENSE_PORT, BATTERY_SENSE_PIN, gpioModeDisabled, 0);

  // PB1: keep divider disconnected by default (hi-Z) to avoid leakage
  GPIO_PinModeSet(BATTERY_SW_PORT, BATTERY_SW_PIN, gpioModeDisabled, 0);

  // Enable IADC0 clock branch
  CMU_ClockEnable(cmuClock_IADC0, true);

  // Declare init structs
  IADC_Init_t init = IADC_INIT_DEFAULT;
  IADC_AllConfigs_t initAllConfigs = IADC_ALLCONFIGS_DEFAULT;
  IADC_InitSingle_t initSingle = IADC_INITSINGLE_DEFAULT;

  // Single input structure
  IADC_SingleInput_t singleInput = IADC_SINGLEINPUT_DEFAULT;

  // Widen input range for GPIO measurements
  initAllConfigs.configs[0].analogGain = BATTERY_IADC_ANALOG_GAIN;
  initAllConfigs.configs[1].analogGain = BATTERY_IADC_ANALOG_GAIN;

  // Reference selection + mV value used by emlib scaling
  initAllConfigs.configs[0].reference = BATTERY_IADC_REFSEL;
  initAllConfigs.configs[1].reference = BATTERY_IADC_REFSEL;
  initAllConfigs.configs[0].vRef = BATTERY_IADC_VREF_MV;
  initAllConfigs.configs[1].vRef = BATTERY_IADC_VREF_MV;

  // Initialize IADC
  IADC_init(IADC0, &init, &initAllConfigs);

  // Configure single input
  singleInput.posInput = iadcPosInputPortBPin0; // PB0 battery sense node
  singleInput.negInput = iadcNegInputGnd;       // Use ground as negative input

  // Initialize single conversion
  IADC_initSingle(IADC0, &initSingle, &singleInput);

  app_log_info("Battery IADC initialized\n");
}

void battery_deinit(void)
{
  IADC0->EN_CLR = IADC_EN_EN;
  CMU_ClockEnable(cmuClock_IADC0, false);
  app_log_info("Battery IADC deinitialized\n");
}

uint32_t battery_read_voltage(void)
{
  const bool sw_high = (BATTERY_SW_ACTIVE_HIGH != 0);
  uint32_t packed = battery_read_voltage_with_sw_level(sw_high);
  uint32_t raw_reading = (packed >> 20) & 0xFFFU;
  uint32_t vbat_mv = packed & 0xFFFFFU;

  // Convert ADC counts to sense-node voltage (mV)
  uint32_t vnode_mv = (raw_reading * (BATTERY_IADC_VREF_MV * BATTERY_IADC_INPUT_SCALE_NUM))
                      / BATTERY_IADC_COUNTS_FULL_SCALE;

  if (vbat_mv < BATTERY_DIAG_BELOW_MV) {
    uint32_t packed_alt = battery_read_voltage_with_sw_level(!sw_high);
    uint32_t raw_alt = (packed_alt >> 20) & 0xFFFU;
    uint32_t vbat_alt_mv = packed_alt & 0xFFFFFU;
    uint32_t vnode_alt_mv = (raw_alt * (BATTERY_IADC_VREF_MV * BATTERY_IADC_INPUT_SCALE_NUM))
                            / BATTERY_IADC_COUNTS_FULL_SCALE;
#if defined(GPIO_BBUSALLOC_BEVEN0_ADC0)
    app_log_warning("Battery diag: sw=%u raw=%lu vnode=%lu mV vbat=%lu mV (alt sw=%u raw=%lu vnode=%lu mV vbat=%lu mV) (refsel=%u gain=%u vref=%lu mV scale=%lu BBUSALLOC=0x%08lx CFG=0x%08lx)\n",
                    (unsigned int)(sw_high ? 1U : 0U),
                    (unsigned long)raw_reading,
                    (unsigned long)vnode_mv,
                    (unsigned long)vbat_mv,
                    (unsigned int)((!sw_high) ? 1U : 0U),
                    (unsigned long)raw_alt,
                    (unsigned long)vnode_alt_mv,
                    (unsigned long)vbat_alt_mv,
                    (unsigned int)BATTERY_IADC_REFSEL,
                    (unsigned int)BATTERY_IADC_ANALOG_GAIN,
                    (unsigned long)BATTERY_IADC_VREF_MV,
                    (unsigned long)BATTERY_IADC_INPUT_SCALE_NUM,
                    (unsigned long)GPIO->BBUSALLOC,
                    (unsigned long)IADC0->CFG);
#else
    app_log_warning("Battery diag: sw=%u raw=%lu vnode=%lu mV vbat=%lu mV (alt sw=%u raw=%lu vnode=%lu mV vbat=%lu mV) (refsel=%u gain=%u vref=%lu mV scale=%lu CFG=0x%08lx)\n",
                    (unsigned int)(sw_high ? 1U : 0U),
                    (unsigned long)raw_reading,
                    (unsigned long)vnode_mv,
                    (unsigned long)vbat_mv,
                    (unsigned int)((!sw_high) ? 1U : 0U),
                    (unsigned long)raw_alt,
                    (unsigned long)vnode_alt_mv,
                    (unsigned long)vbat_alt_mv,
                    (unsigned int)BATTERY_IADC_REFSEL,
                    (unsigned int)BATTERY_IADC_ANALOG_GAIN,
                    (unsigned long)BATTERY_IADC_VREF_MV,
                    (unsigned long)BATTERY_IADC_INPUT_SCALE_NUM,
                    (unsigned long)IADC0->CFG);
#endif
  }

  if (raw_reading >= (BATTERY_IADC_COUNTS_FULL_SCALE - 2U)) {
    app_log_warning("Battery ADC near full-scale (raw=%lu). Reading may be saturated; check Vref/gain/divider.\n",
                    (unsigned long)raw_reading);
  }

#if BATTERY_MONITOR_LOG_DEBUG
  app_log_info("Battery ADC raw=%lu, vnode=%lu mV, vbat=%lu mV\n",
               (unsigned long)raw_reading,
               (unsigned long)vnode_mv,
               (unsigned long)vbat_mv);
#endif

  return vbat_mv;
}

uint8_t battery_calculate_percentage(uint32_t voltage_mv)
{
  const uint32_t full_mv = 4500;
  const uint32_t empty_mv = 3300;

  if (voltage_mv >= full_mv) {
    return 100;
  } else if (voltage_mv <= empty_mv) {
    return 0;
  } else {
    return (uint8_t)((voltage_mv - empty_mv) * 100 / (full_mv - empty_mv));
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
  
  OSSemCreate(&measurement_sem,
              "Sensor Measurement Sem",
              0,
              &err);
  app_assert(err.Code == RTOS_ERR_NONE, "Failed to create measurement semaphore");

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

  // Initialize battery ADC for PB0/PB1 divider measurement
  battery_init();

  // Start peridic measurement
  //error = sensor_trigger_periodic_measurement();
  if (error) {
    app_log_error("Error starting periodic measurement: %d\n", error);
    // Continue anyway - will try to recover in the main loop
  }
  
  // Start periodic sleeptimer to drive measurement cadence
  sl_status_t st = sensor_restart_measurement_timer(measurement_interval);
  app_assert((st == SL_STATUS_OK), "Failed to start measurement timer");

  while (DEF_TRUE) {
    // Wait for timer to signal a measurement window
    OSSemPend(&measurement_sem, 0, OS_OPT_PEND_BLOCKING, DEF_NULL, &err);
    if (err.Code != RTOS_ERR_NONE) {
      continue;
    }

    // wakeup the sensor if it was in sleep mode
    //error = scd4x_wake_up();
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
    //scd4x_power_down();

    if (error == 0 && sensor_readings.valid_data) {
      // Add battery measurement (uses AVDD via app_iadc-style routine)
      sensor_readings.battery_voltage_mv = battery_read_voltage();
      sensor_readings.battery_percentage = battery_calculate_percentage(sensor_readings.battery_voltage_mv);
      app_log_info("Battery: %lu mV (%u%%)\n",
                   (unsigned long)sensor_readings.battery_voltage_mv,
                   (unsigned int)sensor_readings.battery_percentage);
      
      // Save measurement to NVM3 for later sync
      // Timestamp is calculated automatically from epoch + uptime
      sync_data_save_sample(sensor_readings.co2_ppm, 
                            sensor_readings.temperature_mdeg_c, 
                            sensor_readings.humidity_percent,
                            sensor_readings.battery_percentage);
      
      // Signal that new data is available to other tasks
      OSSemPost(&sensor_data_ready_sem, OS_OPT_POST_1, &err);
      app_assert(err.Code == RTOS_ERR_NONE, "Sensor data ready semaphore post failed");
      app_mark_data_activity();
    } else if (error != 1) { // error=1 means data not ready, which is not a critical error
      app_log_warning("Failed to read measurement or data not valid. Will retry.\n");
    }

  }
}
