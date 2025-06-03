#include "notification_task.h"
#include "sl_bluetooth.h"
#include "os.h"
#include "sl_bt_api.h"
#include "app_assert.h"
#include "sl_memory_manager.h" // Added for dynamic allocation
#include "sensor_task.h"
#include "gatt_db.h"

OS_TCB *notification_task_tcb_ptr;
CPU_STK *notification_task_stk_ptr;
// Task control block and stack
static bool    notification_task_running = false;
// Semaphores for synchronization
extern OS_SEM sensor_data_ready_sem;
extern OS_TCB *sensor_task_tcb_ptr;
OS_SEM ble_notification_sem;
extern uint8_t connection_handle;
extern uint16_t measurement_interval;
extern uint8_t temperature_unit;

// Task function prototype
static void notification_task(void *p_arg);

/***************************************************************************//**
 * Initialize NOTIFICATION task.
 ******************************************************************************/
sl_status_t notification_task_init(void)
{
  RTOS_ERR err;

  // Dynamically allocate TCB
  notification_task_tcb_ptr = (OS_TCB *)sl_malloc(sizeof(OS_TCB));
  app_assert(notification_task_tcb_ptr != DEF_NULL, "Failed to allocate TCB for NOTIFICATION task");

  // Dynamically allocate stack
  notification_task_stk_ptr = (CPU_STK *)sl_malloc(NOTIFICATION_TASK_STK_SIZE * sizeof(CPU_STK));
  app_assert(notification_task_stk_ptr != DEF_NULL, "Failed to allocate stack for NOTIFICATION task");

  // Create NOTIFICATION task
  OSTaskCreate(notification_task_tcb_ptr, // Pass pointer to allocated TCB
               "NOTIFICATION Task",
               notification_task,
               DEF_NULL,
               NOTIFICATION_TASK_PRIO,
               notification_task_stk_ptr, // Pass pointer to allocated stack
               (NOTIFICATION_TASK_STK_SIZE / 10u),
               NOTIFICATION_TASK_STK_SIZE,
               0u,
               0u,
               DEF_NULL,
               (OS_OPT_TASK_STK_CLR),
               &err);

  OSSemCreate(&ble_notification_sem,
              "BLE Notification Sem",
              0,                     // Initial count (no data available)
              &err);
  
  // Check if task was created successfully
  if (RTOS_ERR_CODE_GET(err) != RTOS_ERR_NONE) {
    return SL_STATUS_FAIL;
  }
  
  notification_task_running = true;
  return SL_STATUS_OK;
}

/***************************************************************************//**
 * Check if NOTIFICATION task is running.
 ******************************************************************************/
bool notification_task_is_running(void)
{
  return notification_task_running;
}

// Function to handle GATT write requests for the measurement interval
void handle_measurement_interval_write(uint8_t *value, uint16_t length)
{
  if (length == sizeof(measurement_interval)) {
    uint16_t new_interval = (value[1] << 8) | value[0]; // Convert from little-endian
    if (new_interval >= 1 && new_interval <= 3600) { // Validate range (1 second to 1 hour)
      measurement_interval = new_interval;
      app_log_info("Measurement interval updated to %d seconds.\n", measurement_interval);
    } else {
      app_log_warning("Invalid measurement interval: %d seconds. Must be between 1 and 3600.\n", new_interval);
    }
  } else {
    app_log_warning("Invalid write length for measurement interval.\n");
  }
}

// Function to handle GATT read requests for the measurement interval
void handle_measurement_interval_read(uint16_t connection, uint16_t characteristic)
{
  uint8_t value[2];
  value[0] = measurement_interval & 0xFF; // Convert to little-endian
  value[1] = (measurement_interval >> 8) & 0xFF;

  sl_status_t sc = sl_bt_gatt_server_send_user_read_response(
    connection,
    characteristic,
    0, // Success
    sizeof(value),
    value,
    NULL
  );

  if (sc == SL_STATUS_OK) {
    app_log_info("Measurement interval read: %d seconds.\n", measurement_interval);
  } else {
    app_log_error("Failed to send measurement interval read response. Status: 0x%04x\n", sc);
  }
}

// Function to handle GATT write requests for the temperature unit
void handle_temperature_unit_write(uint8_t *value, uint16_t length)
{
  if (length == sizeof(temperature_unit)) {
    uint8_t new_unit = value[0];
    if (new_unit == 0 || new_unit == 1) { // Validate range (0 = Celsius, 1 = Fahrenheit)
      temperature_unit = new_unit;
      app_log_info("Temperature unit updated to %s.\n", temperature_unit ? "Fahrenheit" : "Celsius");
    } else {
      app_log_warning("Invalid temperature unit: %d. Must be 0 (Celsius) or 1 (Fahrenheit).\n", new_unit);
    }
  } else {
    app_log_warning("Invalid write length for temperature unit.\n");
  }
}

// Function to handle GATT read requests for the temperature unit
void handle_temperature_unit_read(uint16_t connection, uint16_t characteristic)
{
  uint8_t value = temperature_unit;

  sl_status_t sc = sl_bt_gatt_server_send_user_read_response(
    connection,
    characteristic,
    0, // Success
    sizeof(value),
    &value,
    NULL
  );

  if (sc == SL_STATUS_OK) {
    app_log_info("Temperature unit read: %s.\n", temperature_unit ? "Fahrenheit" : "Celsius");
  } else {
    app_log_error("Failed to send temperature unit read response. Status: 0x%04x\n", sc);
  }
}

// Function to convert temperature to buffer format
static void temperature_measurement_val_to_buf(int32_t value, bool fahrenheit, uint8_t *buffer)
{
  int32_t temp_value = value;
  
  // Convert temperature based on unit
  if (fahrenheit) {
    // Convert from Celsius to Fahrenheit: F = C * 1.8 + 32
    // For milli-degrees: F_mdeg = (C_mdeg * 18) / 10 + 32000
    temp_value = (value * 18) / 10 + 32000;
  }
  
  uint32_t tmp_value = ((uint32_t)temp_value & 0x00ffffffu) | ((uint32_t)(-3) << 24);
  buffer[0] = fahrenheit ? 1 : 0;
  buffer[1] = tmp_value & 0xff;
  buffer[2] = (tmp_value >> 8) & 0xff;
  buffer[3] = (tmp_value >> 16) & 0xff;
  buffer[4] = (tmp_value >> 24) & 0xff;
}

static void send_notifications(void)
{
  sl_status_t sc;
  RTOS_ERR err;
  // Handle BLE notifications if needed
  uint8_t temperature[5];
  uint8_t co2_data[2];
  uint8_t humidity_data[2];
  // get sensor data
  sensor_data_t *sensor_data = sensor_get_latest_readings();

  // Prepare temperature data for BLE notification
  temperature_measurement_val_to_buf(sensor_data->temperature_mdeg_c, temperature_unit, temperature);

  // Send temperature notification
  sc = sl_bt_gatt_server_send_notification(
    connection_handle,
    gattdb_temperature_measurement,
    sizeof(temperature),
    temperature
  );
  
  if (sc == SL_STATUS_OK) {
    app_log_info("Temperature notification sent successfully.\n");
  } else {
    app_log_error("Failed to send temperature notification. Status: 0x%04x\n", sc);
  }

  // Prepare Humidity data for BLE notifications
  humidity_data[0] = (uint8_t)(sensor_data->humidity_percent & 0xFF);
  humidity_data[1] = (uint8_t)((sensor_data->humidity_percent >> 8) & 0xFF);

  // Send humidity notification
  sc = sl_bt_gatt_server_send_notification(
    connection_handle,
    gattdb_humidity_0,
    sizeof(humidity_data),
    humidity_data
  );
  
  if (sc == SL_STATUS_OK) {
    app_log_info("Humidity notification sent successfully: %d%%\n", sensor_data->humidity_percent);
  } else {
    app_log_error("Failed to send humidity notification. Status: 0x%04x\n", sc);
  }

  // Prepare CO2 data for BLE notifications
  co2_data[0] = (uint8_t)(sensor_data->co2_ppm & 0xFF);
  co2_data[1] = (uint8_t)((sensor_data->co2_ppm >> 8) & 0xFF);

  // Send CO2 notification
  sc = sl_bt_gatt_server_send_notification(
    connection_handle,
    gattdb_air_quality_id,
    sizeof(co2_data),
    co2_data
  );

  if (sc == SL_STATUS_OK) {
    app_log_info("CO2 notification sent successfully: %d ppm\n", sensor_data->co2_ppm);
  } else {
    app_log_error("Failed to send CO2 notification. Status: 0x%04x\n", sc);
  }

  // Send battery level notification
  sc = sl_bt_gatt_server_send_notification(
    connection_handle,
    gattdb_battery_level,
    sizeof(sensor_data->battery_percentage),
    &sensor_data->battery_percentage
  );

  if (sc == SL_STATUS_OK) {
    app_log_info("Battery level notification sent successfully: %d%%\n", sensor_data->battery_percentage);
  } else {
    app_log_error("Failed to send battery level notification. Status: 0x%04x\n", sc);
  }
}

/***************************************************************************//**
 * NOTIFICATION task function.
 ******************************************************************************/
static void notification_task(void *p_arg)
{
  RTOS_ERR err;
  sl_bt_msg_t evt;
  
  (void)p_arg; // Unused parameter
  
  // Infinite loop
  while (1) {
    // Wait for semaphore that indicates new NOTIFICATION event
    OSSemPend(&ble_notification_sem,
              0,                    // Wait indefinitely
              OS_OPT_PEND_BLOCKING,
              NULL,
              &err);
    app_assert_status(err.Code);

    // log the event
    app_log_info("Notification task triggered.\n");

    if(connection_handle != SL_BT_INVALID_CONNECTION_HANDLE) {
      sl_bt_connection_set_parameters(connection_handle, 160, 200, 4, 600, 0, 0xffff);
      app_log_info("Connection parameters set: min_interval=160, max_interval=200, latency=4, timeout=600.\n");
      OSTimeDlyHMSM(0, 0, 2, 0, OS_OPT_TIME_HMSM_STRICT, &err);
    }

    send_notifications();
  }
}
