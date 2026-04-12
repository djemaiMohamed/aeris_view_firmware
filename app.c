/***************************************************************************//**
 * @file
 * @brief Core application logic.
 *******************************************************************************
 * # License
 * <b>Copyright 2024 Silicon Laboratories Inc. www.silabs.com</b>
 *******************************************************************************
 *
 * SPDX-License-Identifier: Zlib
 *
 * The licensor of this software is Silicon Laboratories Inc.
 *
 * This software is provided 'as-is', without any express or implied
 * warranty. In no event will the authors be held liable for any damages
 * arising from the use of this software.
 *
 * Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely, subject to the following restrictions:
 *
 * 1. The origin of this software must not be misrepresented; you must not
 *    claim that you wrote the original software. If you use this software
 *    in a product, an acknowledgment in the product documentation would be
 *    appreciated but is not required.
 * 2. Altered source versions must be plainly marked as such, and must not be
 *    misrepresented as being the original software.
 * 3. This notice may not be removed or altered from any source distribution.
 *
 ******************************************************************************/
#include "sl_common.h"
#include "sl_bt_api.h"
#include "app_assert.h"
#include "app.h"
#include "scd4x_i2c.h"
#include "sensirion_common.h"
#include "sensirion_i2c_hal.h"
#include "memlcd.h"
#include "os.h"
#include "notification_task.h"
#include "gatt_db.h"
#include "sensor_task.h"
#include <stdbool.h>
#include "nvm3_default.h"
#include "calibration_task.h"
#include "sync_data_task.h"

// Forward declarations for task control blocks used in helpers below
extern OS_TCB *sensor_task_tcb_ptr;

// The advertising set handle allocated from Bluetooth stack.
uint8_t advertising_set_handle = 0xff; // Removed static
static RTOS_ERR err;
// connection handle
uint8_t connection_handle = SL_BT_INVALID_CONNECTION_HANDLE;
static int16_t pwr_min;
static int16_t pwr_max;
static bool tasks_are_suspended = false;
static bool sensor_suspended_for_sync = false;
static uint8_t sensor_suspend_depth = 0u;
static bool pending_param_resume = false;
static OS_TICK param_resume_deadline_tick = 0u;
static OS_TICK last_data_tick = 0u;
static const OS_TICK no_data_timeout_ticks = 600000u; // ~10 min if tick=1kHz; adjust as needed

// Start or restart connectable advertising
static void start_advertising(void)
{
  sl_status_t sc;
  sc = sl_bt_legacy_advertiser_generate_data(advertising_set_handle,
                                             sl_bt_advertiser_general_discoverable);
  (void)sc;
  sc = sl_bt_legacy_advertiser_start(advertising_set_handle,
                                     sl_bt_advertiser_connectable_scannable);
  (void)sc;
  app_log_info("Advertising started/restarted.\n");
}

// Shared suspend helper to keep sensor_task suspend/resume balanced across callers
static void sensor_task_suspend_ref(void)
{
  if (sensor_suspend_depth == 0u) {
    RTOS_ERR err;
    OSTaskSuspend(sensor_task_tcb_ptr, &err);
    app_assert_status(err.Code);
  }

  if (sensor_suspend_depth < 255u) {
    sensor_suspend_depth++;
  }
}

// Shared resume helper that only resumes when depth reaches zero
static void sensor_task_resume_ref(void)
{
  if (sensor_suspend_depth == 0u) {
    return;
  }

  sensor_suspend_depth--;
  if (sensor_suspend_depth == 0u) {
    RTOS_ERR err;
    OSTaskResume(sensor_task_tcb_ptr, &err);
    app_assert_status(err.Code);
  }
}

void app_mark_data_activity(void)
{
  RTOS_ERR err;
  last_data_tick = OSTimeGet(&err);
  (void)err;
}
// Track if client has subscribed to sync_data_id notifications/indications
uint8_t sync_data_id_notification_enabled = 0;

// A semaphore to trigger bonding process
extern OS_SEM sensor_data_ready_sem;
extern OS_SEM ble_notification_sem;
extern OS_SEM scan_sem;
extern OS_SEM calibration_task_sem;
extern OS_SEM sync_data_task_sem;
extern OS_TCB *notification_task_tcb_ptr;
extern OS_TCB *lcd_task_tcb_ptr;
extern OS_TCB *sensor_task_tcb_ptr;
extern OS_TCB *sync_data_task_tcb_ptr;
extern OS_TCB *calibration_task_tcb_ptr;

// Application Init.
SL_WEAK void app_init(void)
{
}

// Application Process Action.
SL_WEAK void app_process_action(void)
{
  if (app_is_process_required()) {
    /////////////////////////////////////////////////////////////////////////////
    // Put your additional application code here!                              //
    // This is will run each time app_proceed() is called.                     //
    // Do not call blocking functions from here!                               //
    /////////////////////////////////////////////////////////////////////////////
  }
}

// function to suspend tasks
static void suspend_tasks(void) {
  if (tasks_are_suspended) {
    app_log_info("Tasks already suspended.\n");
    return;
  }
  RTOS_ERR err;
  // Suspend the sensor task via shared depth-aware helper
  sensor_task_suspend_ref();
  // Suspend the LCD task
  OSTaskSuspend(lcd_task_tcb_ptr, &err);
  app_assert_status(err.Code);
  // Suspend the notification task
  OSTaskSuspend(notification_task_tcb_ptr, &err);
  app_assert_status(err.Code);
  // suspend the sync data task
  OSTaskSuspend(sync_data_task_tcb_ptr, &err);
  app_assert_status(err.Code);
  // suspend the calibration task
  OSTaskSuspend(calibration_task_tcb_ptr, &err);
  app_assert_status(err.Code);
  tasks_are_suspended = true;
  app_log_info("Tasks suspended.\n");
}

// function to resume tasks
static void resume_tasks(void) {
  if (!tasks_are_suspended) {
    app_log_info("Tasks already running.\n");
    return;
  }
  RTOS_ERR err;
  // Resume the sensor task via shared depth-aware helper
  sensor_task_resume_ref();
  // Resume the LCD task
  OSTaskResume(lcd_task_tcb_ptr, &err);
  app_assert_status(err.Code);
  // Resume the notification task
  OSTaskResume(notification_task_tcb_ptr, &err);
  app_assert_status(err.Code);
  // resume the sync data task
  OSTaskResume(sync_data_task_tcb_ptr, &err);
  app_assert_status(err.Code);
  // resume the calibration task
  OSTaskResume(calibration_task_tcb_ptr, &err);
  app_assert_status(err.Code);
  tasks_are_suspended = false;
  app_log_info("Tasks resumed.\n");
}

void app_suspend_sensor_task(void)
{
  if (sensor_suspended_for_sync) {
    return;
  }

  sl_status_t timer_status = sensor_stop_measurement_timer();
  if (timer_status != SL_STATUS_OK) {
    app_log_error("Failed to stop measurement timer for sync (0x%lx).\n", (unsigned long)timer_status);
  }

  sensor_task_suspend_ref();
  sensor_suspended_for_sync = true;
  app_log_info("Sensor task suspended for sync.\n");
}

void app_resume_sensor_task(void)
{
  if (!sensor_suspended_for_sync) {
    return;
  }
  sensor_task_resume_ref();

  if (sensor_suspend_depth == 0u) {
    sl_status_t timer_status = sensor_restart_measurement_timer(measurement_interval);
    if (timer_status != SL_STATUS_OK) {
      app_log_error("Failed to restart measurement timer after sync (0x%lx).\n", (unsigned long)timer_status);
    }
  }

  sensor_suspended_for_sync = false;
  app_log_info("Sensor task resumed after sync.\n");
}

/**************************************************************************//**
 * Bluetooth stack event handler.
 * This overrides the dummy weak implementation.
 *
 * @param[in] evt Event coming from the Bluetooth stack.
 *****************************************************************************/
void sl_bt_on_event(sl_bt_msg_t *evt)
{
  sl_status_t sc;
  RTOS_ERR local_err; // Use a local RTOS_ERR for OSSemPost
  RTOS_ERR err_tick;
  OS_TICK now_tick = OSTimeGet(&err_tick);
  (void)err_tick;

  // Safety: if connected but no data activity for too long, close link and restart ads
  if (connection_handle != SL_BT_INVALID_CONNECTION_HANDLE && now_tick - last_data_tick > no_data_timeout_ticks) {
    app_log_warning("No data activity detected; closing connection to recover.\n");
    sl_bt_connection_close(connection_handle);
    resume_tasks();
    pending_param_resume = false;
    connection_handle = SL_BT_INVALID_CONNECTION_HANDLE;
    start_advertising();
  }

  switch (SL_BT_MSG_ID(evt->header)) {
    // -------------------------------
    // This event indicates the device has started and the radio is ready.
    // Do not call any stack command before receiving this boot event!
    case sl_bt_evt_system_boot_id:
      app_log_info("Preserving existing bondings at startup.\n");
      sl_bt_sm_configure(SL_BT_SM_CONFIGURATION_BONDING_REQUIRED |
                         SL_BT_SM_CONFIGURATION_MITM_REQUIRED, sl_bt_sm_io_capability_displayonly);
      sl_bt_sm_set_bondable_mode(1);
      sl_bt_sm_store_bonding_configuration(32, 1);
      app_log_info("bondable mode is set");

      sl_bt_system_set_tx_power(-30, -10, &pwr_min, &pwr_max);

      // Create an advertising set.
      sc = sl_bt_advertiser_create_set(&advertising_set_handle);
      // Generate data for advertising
      sc = sl_bt_legacy_advertiser_generate_data(advertising_set_handle,
                                                 sl_bt_advertiser_general_discoverable);
      // Set advertising interval.
      sc = sl_bt_advertiser_set_timing(
        advertising_set_handle,
        6400, // min. adv. interval (milliseconds * 1.6)
        6400, // max. adv. interval (milliseconds * 1.6)
        0,   // adv. duration (3 minutes)
        0);  // max. num. adv. events
      // Start advertising and enable connections.
      sc = sl_bt_legacy_advertiser_start(advertising_set_handle,
                                         sl_bt_advertiser_connectable_scannable);
      break;

    // -------------------------------
    // This event indicates that a new connection was opened.
    case sl_bt_evt_connection_opened_id:
      // suspend tasks until secure connection is established
      suspend_tasks();
      app_log_info("Connection opened. Handle: %d\n", evt->data.evt_connection_opened.connection);
      connection_handle = evt->data.evt_connection_opened.connection;

      // Reset activity timestamp on new connection
      app_mark_data_activity();

      sl_bt_system_set_tx_power(0, 80, &pwr_min, &pwr_max);

      // inscrease security level
      sc = sl_bt_sm_increase_security(evt->data.evt_connection_opened.connection);
      app_log_info("Security level increased.\n");

      // Stop advertising
      //sl_bt_advertiser_stop(advertising_set_handle);
      //app_log_info("Advertising stopped.\n");
      break;

    case sl_bt_evt_connection_parameters_id:
      // Fallback: if a pending resume deadline passed, resume tasks even if latency != 4
      if (pending_param_resume) {
        RTOS_ERR err_tick;
        OS_TICK now_tick = OSTimeGet(&err_tick);
        (void)err_tick;
        if (now_tick >= param_resume_deadline_tick) {
          app_log_info("Connection param timeout reached; resuming tasks as fallback.\n");
          resume_tasks();
          pending_param_resume = false;
        }
      }

      bool authenticated =false;
      switch (evt->data.evt_connection_parameters.security_mode)
      {
        case sl_bt_connection_mode1_level1:
          app_log_info("No Security\r\n");
          break;
        case sl_bt_connection_mode1_level2:
          app_log_info("Unauthenticated pairing with encryption (Just Works)\r\n");
          break;
        case sl_bt_connection_mode1_level3:
          app_log_info("Authenticated pairing with encryption (Legacy Pairing)\r\n");
          break;
        case sl_bt_connection_mode1_level4:
          app_log_info("Authenticated Secure Connections pairing with encryption using a 128-bit strength encryption key (Secure Connections)\r\n");
          authenticated = true;
          break;
        default:
          break;
      }

      uint16_t interval = evt->data.evt_connection_parameters.interval, latency = evt->data.evt_connection_parameters.latency, timeout = evt->data.evt_connection_parameters.timeout;
      app_log_info("connection interval %d, latency %d, timeout %d ms\n", interval, latency, timeout * 10);
      if (authenticated) {
        if (latency != 4) {
          app_log_info("Changing connection parameters to desired latency of 4. Suspending tasks.\n");
          sl_bt_connection_set_parameters(evt->data.evt_connection_parameters.connection, 160, 200, 4, 600, 0, 0xffff);
          app_log_info("Re-setting connection parameters to desired latency of 4.\n");
          // Fallback: if latency never reaches 4, resume tasks after timeout
          RTOS_ERR err_tick;
          param_resume_deadline_tick = OSTimeGet(&err_tick) + 10000u; // ~10s at 1kHz tick
          pending_param_resume = true;
        } else {
          app_log_info("Desired connection parameters already set. Resuming tasks.\n");
          // resume tasks
          resume_tasks();
          // trigger notification task to send data
          OSSemPost(&ble_notification_sem, OS_OPT_POST_1, &local_err);
          pending_param_resume = false;
        }
      }

      break;

    case sl_bt_evt_connection_set_parameters_failed_id:
      app_log_error("Failed to set connection parameters");
      break;
    // -------------------------------
    // This event indicates that a connection was closed.
    case sl_bt_evt_connection_closed_id:
      app_log_info("Connection closed. Handle: %d, Reason: 0x%04x\n",
                   evt->data.evt_connection_closed.connection,
                   evt->data.evt_connection_closed.reason);
      resume_tasks();
      connection_handle = SL_BT_INVALID_CONNECTION_HANDLE;
      pending_param_resume = false;

      sl_bt_system_set_tx_power(-30, 0, &pwr_min, &pwr_max);

      // Start advertising and enable connections.
      start_advertising();
      break;

    ///////////////////////////////////////////////////////////////////////////
    // Add additional event handlers here as your application requires!      //
    ///////////////////////////////////////////////////////////////////////////
    case sl_bt_evt_sm_passkey_display_id:
      app_log_info("Passkey: %lu\n", evt->data.evt_sm_passkey_display.passkey);
      memlcd_display_passkey(evt->data.evt_sm_passkey_display.passkey);
      break;

    case sl_bt_evt_sm_confirm_bonding_id:
      app_log_info("Bonding confirm\r\n");
      sc = sl_bt_sm_bonding_confirm(evt->data.evt_sm_confirm_bonding.connection, 1);
      app_assert_status(sc);
      break;

    case sl_bt_evt_sm_bonded_id:
      // Set pairing mode to false after bonding is completed
      app_log_info("Bonding completed. Connection: %d, Security mode: %d, Bonding handle: %d\n",
                   evt->data.evt_sm_bonded.connection,
                   evt->data.evt_sm_bonded.security_mode,
                   evt->data.evt_sm_bonded.bonding);

      // Ensure tasks are running and LCD is cleared after pairing
      resume_tasks();

      //sl_bt_connection_set_parameters(connection_handle, 160, 200, 4, 600, 0, 0xffff);
      OSSemPost(&sensor_data_ready_sem, OS_OPT_POST_1, &local_err);
      break;

    case sl_bt_evt_sm_bonding_failed_id:
      sl_bt_sm_delete_bondings();
      app_log_error("Bonding failed. Connection: %d, Reason: 0x%04X\n",
                    evt->data.evt_sm_bonding_failed.connection,
                    evt->data.evt_sm_bonding_failed.reason);
      app_log_error("Device not secure. Closing connection.\n");
      sl_bt_connection_close(evt->data.evt_sm_bonding_failed.connection);

      sl_bt_system_set_tx_power(-30, -10, &pwr_min, &pwr_max);

      //sc = sl_bt_legacy_advertiser_start(
      //    advertising_set_handle,
      //    sl_bt_advertiser_connectable_scannable);
      //app_log_info("Restarted advertising after bonding failure.\n");
      break;

    case sl_bt_evt_gatt_server_characteristic_status_id:
      if (evt->data.evt_gatt_server_characteristic_status.status_flags == sl_bt_gatt_server_client_config) {
        // Track subscription for sync_data_id characteristic
        if (evt->data.evt_gatt_server_characteristic_status.characteristic == gattdb_sync_data_id) {
          sync_data_id_notification_enabled = 
            (evt->data.evt_gatt_server_characteristic_status.client_config_flags &
             (sl_bt_gatt_server_notification | sl_bt_gatt_server_indication)) ? 1 : 0;
          app_log_info("Sync data notifications/indications %s\n", 
                       sync_data_id_notification_enabled ? "enabled" : "disabled");
          app_mark_data_activity();
        }
      }
      break;

    case sl_bt_evt_gatt_server_attribute_value_id:
      if (evt->data.evt_gatt_server_attribute_value.attribute == gattdb_measurement_interval) {
        handle_measurement_interval_write(evt->data.evt_gatt_server_attribute_value.value.data,
                                          evt->data.evt_gatt_server_attribute_value.value.len);
        // Signal the sensor task to update the measurement interval.
        OSSemPost(&sensor_data_ready_sem, OS_OPT_POST_1, &local_err);
        app_mark_data_activity();
      } else if (evt->data.evt_gatt_server_attribute_value.attribute == gattdb_temp_unit_id) {
        handle_temperature_unit_write(evt->data.evt_gatt_server_attribute_value.value.data,
                                     evt->data.evt_gatt_server_attribute_value.value.len);
        // Signal the sensor task to update temperature.
        OSSemPost(&sensor_data_ready_sem, OS_OPT_POST_1, &local_err);
        app_mark_data_activity();
      } 
      // run calibration
      else if (evt->data.evt_gatt_server_attribute_value.attribute == gattdb_run_calibration_id) {
        handle_calibration_write(evt->data.evt_gatt_server_attribute_value.value.data,
                                 evt->data.evt_gatt_server_attribute_value.value.len);
        // Signal the calibration task to run calibration.
        OSSemPost(&calibration_task_sem, OS_OPT_POST_1, &local_err);
        app_mark_data_activity();
      }
      // sync data request
      else if (evt->data.evt_gatt_server_attribute_value.attribute == gattdb_sync_data_id) {
        handle_sync_data_write(evt->data.evt_gatt_server_attribute_value.value.data,
                               evt->data.evt_gatt_server_attribute_value.value.len);
        // Semaphore is posted by handle_sync_data_write
        app_mark_data_activity();
      }
      // display type
      else if (evt->data.evt_gatt_server_attribute_value.attribute == gattdb_disp_type_id) {
        handle_display_type_write(evt->data.evt_gatt_server_attribute_value.value.data,
                                  evt->data.evt_gatt_server_attribute_value.value.len);
        OSSemPost(&sensor_data_ready_sem, OS_OPT_POST_1, &local_err);
        app_mark_data_activity();
      }
      break;

    case sl_bt_evt_gatt_server_user_read_request_id:
      if (evt->data.evt_gatt_server_user_read_request.characteristic == gattdb_measurement_interval) {
        handle_measurement_interval_read(evt->data.evt_gatt_server_user_read_request.connection,
                                         evt->data.evt_gatt_server_user_read_request.characteristic);
        app_mark_data_activity();
      } else if (evt->data.evt_gatt_server_user_read_request.characteristic == gattdb_temp_unit_id) {
        handle_temperature_unit_read(evt->data.evt_gatt_server_user_read_request.connection,
                                     evt->data.evt_gatt_server_user_read_request.characteristic);
        app_mark_data_activity();
      } else if (evt->data.evt_gatt_server_user_read_request.characteristic == gattdb_run_calibration_id) {
        handle_calibration_read(evt->data.evt_gatt_server_user_read_request.connection,
                                evt->data.evt_gatt_server_user_read_request.characteristic);
        app_mark_data_activity();
      } else if (evt->data.evt_gatt_server_user_read_request.characteristic == gattdb_sync_data_id) {
        handle_sync_data_read(evt->data.evt_gatt_server_user_read_request.connection,
                              evt->data.evt_gatt_server_user_read_request.characteristic);
        app_mark_data_activity();
      } else if (evt->data.evt_gatt_server_user_read_request.characteristic == gattdb_disp_type_id) {
        handle_display_type_read(evt->data.evt_gatt_server_user_read_request.connection,
                                 evt->data.evt_gatt_server_user_read_request.characteristic);
        app_mark_data_activity();
      }
      break;
    // -------------------------------
    // Default event handler.
    default:
      break;
  }
}
