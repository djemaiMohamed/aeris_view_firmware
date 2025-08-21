#include "calibration_task.h"
#include "sensor_task.h"
#include "app_assert.h"
#include "app_log.h"
#include "sl_memory_manager.h"
#include "scd4x_i2c.h"
#include "memlcd.h"
#include "sl_bt_api.h"
#include "gatt_db.h"
#include <stdio.h>

// External handles
extern OS_TCB *sensor_task_tcb_ptr;
extern memlcd_handle_t lcd_data;
extern uint8_t connection_handle;

// Local state
static bool calibration_running = false;
static uint16_t target_ppm = 0;

// RTOS objects
OS_TCB *calibration_task_tcb_ptr;
CPU_STK *calibration_task_stk_ptr;
OS_SEM calibration_task_sem;

static void calibration_task(void *p_arg);

sl_status_t calibration_task_init(void)
{
  RTOS_ERR err;

  calibration_task_tcb_ptr = (OS_TCB *)sl_malloc(sizeof(OS_TCB));
  app_assert(calibration_task_tcb_ptr != DEF_NULL, "Failed to allocate TCB for CALIBRATION task");

  calibration_task_stk_ptr = (CPU_STK *)sl_malloc(CALIBRATION_TASK_STK_SIZE * sizeof(CPU_STK));
  app_assert(calibration_task_stk_ptr != DEF_NULL, "Failed to allocate stack for CALIBRATION task");

  // Create semaphore with count 0
  OSSemCreate(&calibration_task_sem, "calibration_sem", 0, &err);
  app_assert(err.Code == RTOS_ERR_NONE, "Failed to create calibration semaphore");

  // Create task
  OSTaskCreate(calibration_task_tcb_ptr,
               "calibration_task",
               calibration_task,
               DEF_NULL,
               CALIBRATION_TASK_PRIO,
               calibration_task_stk_ptr,
               CALIBRATION_TASK_STK_SIZE / 10,
               CALIBRATION_TASK_STK_SIZE,
               0u,
               0u,
               DEF_NULL,
               OS_OPT_TASK_STK_CHK | OS_OPT_TASK_STK_CLR,
               &err);
  app_assert(err.Code == RTOS_ERR_NONE, "Failed to create CALIBRATION task");

  return SL_STATUS_OK;
}

bool calibration_task_is_running(void)
{
  return calibration_running;
}

void handle_calibration_write(const uint8_t *value, uint16_t length)
{
  if (!value || length < 2) {
    app_log_error("Calibration write invalid length=%u\n", length);
    return;
  }
  // little-endian uint16 ppm
  target_ppm = (uint16_t)value[0] | ((uint16_t)value[1] << 8);
  app_log_info("Calibration requested: target %u ppm\n", target_ppm);
  // Semaphore will be posted by caller (BLE event handler)
}

static void show_calibration_screen(const char *msg)
{
  memlcd_display_calibration_message("CALIBRATION", msg);
}

static void calibration_task(void *p_arg)
{
  (void)p_arg;
  RTOS_ERR err;

  for (;;) {
    // Wait for request
    OSSemPend(&calibration_task_sem, 0, OS_OPT_PEND_BLOCKING, DEF_NULL, &err);
    if (err.Code != RTOS_ERR_NONE) {
      continue;
    }

    calibration_running = true;

    // Pause sensor task
    OSTaskSuspend(sensor_task_tcb_ptr, &err);
    if (err.Code != RTOS_ERR_NONE) {
      app_log_error("Failed to suspend sensor task (err=%d)\n", (int)err.Code);
    }

    show_calibration_screen("Calibration: preparing...");

    // Ensure SCD4x initialized at correct I2C address and idle
    scd4x_init(SCD41_I2C_ADDR_62);
    (void)scd4x_stop_periodic_measurement();

    // Take 6 single-shot measurements spaced by ~30s (>= 3 minutes total)
    const int total_samples = 6;
    for (int i = 0; i < total_samples; ++i) {
      int16_t s = scd4x_measure_single_shot();
      if (s) {
        app_log_warning("single_shot cmd failed (%d)\n", s);
      }

      // Poll for measurement readiness (up to ~10s)
      uint16_t co2 = 0; int32_t t = 0, rh = 0;
      bool got_data = false;
      for (int retry = 0; retry < 12; ++retry) {
        bool ready = false;
        (void)scd4x_get_data_ready_status(&ready);
        if (ready) {
          if (scd4x_read_measurement(&co2, &t, &rh) == 0) {
            got_data = true;
          }
          break;
        }
        OSTimeDlyHMSM(0u, 0u, 1u, 0u, OS_OPT_TIME_HMSM_STRICT, &err);
        if (err.Code != RTOS_ERR_NONE) break;
      }

      if (got_data) {
        lcd_data.co2_value = co2;
        lcd_data.temperature_value = t / 1000.0f;
        lcd_data.humidity_value = (uint8_t)(rh / 1000);
      }

      // Show progress with percent and time remaining
      uint8_t samples_done = (uint8_t)(i + 1);
      uint8_t percent = (uint8_t)((samples_done * 100) / total_samples);
      uint16_t seconds_remaining = (uint16_t)((total_samples - samples_done) * 30);
      memlcd_display_calibration_progress(percent, seconds_remaining, samples_done, (uint8_t)total_samples);

      // Maintain ~30s cadence between single shots
      if (i < total_samples - 1) {
        // Already waited up to ~12s while polling; add ~20s more
        OSTimeDlyHMSM(0u, 0u, 20u, 0u, OS_OPT_TIME_HMSM_STRICT, &err);
        if (err.Code != RTOS_ERR_NONE) {
          break;
        }
      }
    }

    // Ensure idle before FRC
    (void)scd4x_stop_periodic_measurement();

    show_calibration_screen("Applying FRC...");

    uint16_t frc_correction = 0;
    int16_t s = scd4x_perform_forced_recalibration(target_ppm, &frc_correction);
    if (s) {
      app_log_error("FRC failed (%d)\n", s);
    } else {
      if (frc_correction == 0xFFFF) {
        app_log_error("FRC reported failure (0xFFFF)\n");
      } else {
        int16_t corr_ppm = (int16_t)frc_correction - (int16_t)0x8000;
        app_log_info("FRC correction: %d ppm\n", (int)corr_ppm);
      }
    }

    // Persist settings
    (void)scd4x_persist_settings();

    // reinit sensor
    sensor_init();

    show_calibration_screen("Calibration done");

    // Notify connected client that calibration finished (status = 1)
    if (connection_handle != SL_BT_INVALID_CONNECTION_HANDLE) {
      uint8_t status_val[2] = { 1, 0 }; // little-endian 0x0001 = finished
      sl_status_t sc = sl_bt_gatt_server_send_notification(
        connection_handle,
        gattdb_run_calibration_id,
        sizeof(status_val),
        status_val);
      if (sc != SL_STATUS_OK) {
        app_log_warning("Calibration finish notification failed: 0x%lx\n", (unsigned long)sc);
      }
    }

    // Signal that new data is available to other tasks
    OSSemPost(&sensor_data_ready_sem, OS_OPT_POST_1, &err);
    app_assert(err.Code == RTOS_ERR_NONE, "Sensor data ready semaphore post failed");

    // Resume sensor task
    OSTaskResume(sensor_task_tcb_ptr, &err);
    if (err.Code != RTOS_ERR_NONE) {
      app_log_error("Failed to resume sensor task (err=%d)\n", (int)err.Code);
    }

    calibration_running = false;
  }
}
