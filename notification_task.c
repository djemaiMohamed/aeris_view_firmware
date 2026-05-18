#include "notification_task.h"
#include "sl_bluetooth.h"
#include "os.h"
#include "sl_bt_api.h"
#include "app_assert.h"
#include "sl_memory_manager.h"
#include "sensor_task.h"
#include "gatt_db.h"
#include "calibration_task.h"
#include "sync_data_task.h"
#include <string.h>

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

static void send_notifications(void)
{
  sl_status_t sc;
  RTOS_ERR retry_err;

  sensor_data_t *sensor_data = sensor_get_latest_readings();

  // v2 live data frame — 20 bytes, frame type (0x00) at byte 14.
  // Layout mirrors the sync DATA frame so the iOS decoder handles both:
  //   [0-3]  seq_id    = 0  (live frames carry no seq_id)
  //   [4-7]  uptime    (device uptime in seconds, little-endian)
  //   [8-9]  co2       (PPM, uint16 LE)
  //   [10-11] temp     (centi-°C, int16 LE)
  //   [12-13] humidity (%, uint16 LE)
  //   [14]   frame type = FRAME_LIVE (0x00)
  //   [15]   battery   (%)
  //   [16-17] boot_count (uint16 LE)
  //   [18-19] reserved
  uint8_t data[SYNC_FRAME_SIZE];
  memset(data, 0, sizeof(data));

  uint32_t uptime     = sync_data_get_uptime_sec();
  uint16_t boot_cnt   = sync_data_get_boot_count();
  int16_t  temp_centi = (int16_t)(sensor_data->temperature_mdeg_c / 10);

  // seq_id = 0 (bytes 0-3 already zeroed)

  data[4]  = (uint8_t)(uptime & 0xFF);
  data[5]  = (uint8_t)((uptime >> 8) & 0xFF);
  data[6]  = (uint8_t)((uptime >> 16) & 0xFF);
  data[7]  = (uint8_t)((uptime >> 24) & 0xFF);

  data[8]  = (uint8_t)(sensor_data->co2_ppm & 0xFF);
  data[9]  = (uint8_t)((sensor_data->co2_ppm >> 8) & 0xFF);

  data[10] = (uint8_t)((uint16_t)temp_centi & 0xFF);
  data[11] = (uint8_t)(((uint16_t)temp_centi >> 8) & 0xFF);

  data[12] = (uint8_t)(sensor_data->humidity_percent & 0xFF);
  data[13] = (uint8_t)((sensor_data->humidity_percent >> 8) & 0xFF);

  data[14] = 0x00;  // FRAME_LIVE

  data[15] = sensor_data->battery_percentage;

  data[16] = (uint8_t)(boot_cnt & 0xFF);
  data[17] = (uint8_t)((boot_cnt >> 8) & 0xFF);

  // bytes 18-19 reserved, already 0

  sc = SL_STATUS_FAIL;
  int retry_count = 0;
  const int MAX_RETRIES = 10;

  while (sc != SL_STATUS_OK && retry_count < MAX_RETRIES) {
    sc = sl_bt_gatt_server_send_notification(
      connection_handle,
      gattdb_sync_data_id,
      sizeof(data),
      data
    );
    if (sc != SL_STATUS_OK) {
      retry_count++;
      app_log_error("Failed to send live data notification: 0x%04lx. Retry %d/%d\n",
                    (unsigned long)sc, retry_count, MAX_RETRIES);
      if (retry_count < MAX_RETRIES) {
        uint32_t delay_sec = (retry_count < 3) ? 1u : 2u;
        OSTimeDlyHMSM(0, 0, delay_sec, 0, OS_OPT_TIME_HMSM_STRICT, &retry_err);
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

  (void)p_arg;
  
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
