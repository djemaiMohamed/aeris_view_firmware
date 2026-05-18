#include "notification_task.h"
#include "sl_bluetooth.h"
#include "os.h"
#include "sl_bt_api.h"
#include "app_assert.h"
#include "sl_memory_manager.h" // Added for dynamic allocation
#include "sensor_task.h"
#include "gatt_db.h"
#include "calibration_task.h"
#include "sync_data_task.h"

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
extern uint8_t display_type;
extern uint8_t sync_data_id_notification_enabled;

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
      sl_status_t st = sensor_restart_measurement_timer(measurement_interval);
      if (st == SL_STATUS_OK) {
        app_log_info("Measurement interval updated to %d seconds; timer restarted and measurement triggered.\n",
                     measurement_interval);
      } else {
        app_log_error("Failed to restart measurement timer (status=0x%lx) for interval %u s.\n",
                      (unsigned long)st,
                      (unsigned int)measurement_interval);
      }
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

// Function to handle GATT read requests for the calibration status
void handle_calibration_read(uint16_t connection, uint16_t characteristic)
{
  // status: 0 = idle, 1 = running
  uint16_t status = calibration_task_is_running() ? 1 : 0;
  uint8_t value[2];
  value[0] = status & 0xFF;
  value[1] = (status >> 8) & 0xFF;

  sl_status_t sc = sl_bt_gatt_server_send_user_read_response(
    connection,
    characteristic,
    0, // Success
    sizeof(value),
    value,
    NULL);

  if (sc == SL_STATUS_OK) {
    app_log_info("Calibration status read: %u.\n", (unsigned int)status);
  } else {
    app_log_error("Failed to send calibration read response (0x%lx).\n", (unsigned long)sc);
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

// Function to handle GATT write requests for the display type
void handle_display_type_write(uint8_t *value, uint16_t length)
{
  if (length == sizeof(display_type)) {
    uint8_t new_type = value[0];
    if (new_type == 0 || new_type == 1) {
      display_type = new_type;
      app_log_info("Display type updated to %s.\n", display_type ? "simple" : "complex");
    } else {
      app_log_warning("Invalid display type: %d. Must be 0 (complex) or 1 (simple).\n", new_type);
    }
  } else {
    app_log_warning("Invalid write length for display type.\n");
  }
}

// Function to handle GATT read requests for the display type
void handle_display_type_read(uint16_t connection, uint16_t characteristic)
{
  uint8_t value = display_type;

  sl_status_t sc = sl_bt_gatt_server_send_user_read_response(
    connection,
    characteristic,
    0, // Success
    sizeof(value),
    &value,
    NULL
  );

  if (sc == SL_STATUS_OK) {
    app_log_info("Display type read: %s.\n", display_type ? "simple" : "complex");
  } else {
    app_log_error("Failed to send display type read response. Status: 0x%04x\n", sc);
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
  
  // get sensor data
  sensor_data_t *sensor_data = sensor_get_latest_readings();
  
  // Use unified 12-byte format for sync_data_id characteristic
  // Format: timestamp(4) + co2(2) + temperature(2) + humidity(2) + flags(2)
  // Flag 0x00 = live data, 0x01 = historical data, 0x02 = sync complete
  uint8_t data[12];
  
  // Timestamp = device uptime in seconds (phone computes absolute time).
  // sync_data_get_epoch() is deprecated and returns uptime too, so use the
  // canonical function directly to avoid the previous double-count bug.
  uint32_t timestamp = sync_data_get_uptime_sec();
  
  // Pack timestamp (4 bytes, little-endian)
  data[0] = (uint8_t)(timestamp & 0xFF);
  data[1] = (uint8_t)((timestamp >> 8) & 0xFF);
  data[2] = (uint8_t)((timestamp >> 16) & 0xFF);
  data[3] = (uint8_t)((timestamp >> 24) & 0xFF);
  
  // Pack CO2 (2 bytes, little-endian)
  data[4] = (uint8_t)(sensor_data->co2_ppm & 0xFF);
  data[5] = (uint8_t)((sensor_data->co2_ppm >> 8) & 0xFF);
  
  // Pack temperature (2 bytes, little-endian, centi-degrees Celsius)
  int16_t temp_centi = (int16_t)(sensor_data->temperature_mdeg_c / 10);
  data[6] = (uint8_t)(temp_centi & 0xFF);
  data[7] = (uint8_t)((temp_centi >> 8) & 0xFF);
  
  // Pack humidity (2 bytes, little-endian, percent)
  data[8] = (uint8_t)(sensor_data->humidity_percent & 0xFF);
  data[9] = (uint8_t)((sensor_data->humidity_percent >> 8) & 0xFF);
  
  // Flags: 0x00 = live data
  data[10] = 0x00;
  data[11] = sensor_data->battery_percentage;
  
  // Send unified notification using sync_data_id characteristic
  sc = SL_STATUS_FAIL;
  int retry_count = 0;
  const int MAX_RETRIES = 10;  // Increased to 10 retries
  RTOS_ERR retry_err;
  
  while (sc != SL_STATUS_OK && retry_count < MAX_RETRIES) {
    sc = sl_bt_gatt_server_send_notification(
      connection_handle,
      gattdb_sync_data_id,
      sizeof(data),
      data
    );
    // log fail and retry
    if (sc != SL_STATUS_OK) {
      retry_count++;
      app_log_error("Failed to send live data notification: 0x%04lx. Retry %d/%d\n", 
                    (unsigned long)sc, retry_count, MAX_RETRIES);
      // Add longer delay before retry to give client time to subscribe
      // First few retries wait 1 second, then increase to 2 seconds
      if (retry_count < MAX_RETRIES) {
        if (retry_count < 3) {
          OSTimeDlyHMSM(0, 0, 1, 0, OS_OPT_TIME_HMSM_STRICT, &retry_err);  // 1 second
        } else {
          OSTimeDlyHMSM(0, 0, 2, 0, OS_OPT_TIME_HMSM_STRICT, &retry_err);  // 2 seconds
        }
      }
    }
  }
  
  if (sc == SL_STATUS_OK) {
    app_log_info("Live data notification sent: CO2=%u, Temp=%d.%02d, Hum=%lu%%, Bat=%u%%\n",
                   sensor_data->co2_ppm,
                   temp_centi / 100, (temp_centi >= 0 ? temp_centi : -temp_centi) % 100,
                   (unsigned long)sensor_data->humidity_percent,
                   sensor_data->battery_percentage);
  } else {
    app_log_error("Failed to send live data notification after %d attempts. Status: 0x%04lx\n", 
                  MAX_RETRIES, (unsigned long)sc);
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
      send_notifications();
    }

  }
}
