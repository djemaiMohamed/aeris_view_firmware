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
#include "bond_task.h" // Include new bond task header
#include "notification_task.h"
#include "gatt_db.h"

// The advertising set handle allocated from Bluetooth stack.
static uint8_t advertising_set_handle = 0xff;
static RTOS_ERR err;
extern OS_TCB *sensor_task_tcb_ptr;
extern OS_TCB *lcd_task_tcb_ptr;
// connection handle
uint8_t connection_handle = 0xFF;

// A semaphore to trigger bonding process
extern OS_SEM bond_sem;
extern OS_SEM sensor_data_ready_sem;

// Initialize g_app_current_bond_info
extern app_bond_info_t g_app_current_bond_info;

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

  switch (SL_BT_MSG_ID(evt->header)) {
    // -------------------------------
    // This event indicates the device has started and the radio is ready.
    // Do not call any stack command before receiving this boot event!
    case sl_bt_evt_system_boot_id:
      sc = sl_bt_sm_delete_bondings();
      app_assert_status(sc);
      app_log_info("All previous bondings deleted at startup.\n");
      sl_bt_sm_configure(SL_BT_SM_CONFIGURATION_BONDING_REQUIRED |
                         SL_BT_SM_CONFIGURATION_MITM_REQUIRED, sl_bt_sm_io_capability_displayonly);
      sl_bt_sm_set_bondable_mode(1);
      sl_bt_sm_store_bonding_configuration(32, 1);
      app_log_info("bondable mode is set");
      // Create an advertising set.
      sc = sl_bt_advertiser_create_set(&advertising_set_handle);
      // Generate data for advertising
      sc = sl_bt_legacy_advertiser_generate_data(advertising_set_handle,
                                                 sl_bt_advertiser_general_discoverable);
      // Set advertising interval to 100ms.
      sc = sl_bt_advertiser_set_timing(
        advertising_set_handle,
        160, // min. adv. interval (milliseconds * 1.6)
        160, // max. adv. interval (milliseconds * 1.6)
        0,   // adv. duration
        0);  // max. num. adv. events
      // Start advertising and enable connections.
      sc = sl_bt_legacy_advertiser_start(advertising_set_handle,
                                         sl_bt_advertiser_connectable_scannable);
      // Post to bond_sem to check for existing bonds
      //OSSemPost(&bond_sem, OS_OPT_POST_1, &local_err);
      //app_assert_status(local_err.Code);
      break;

    // -------------------------------
    // This event indicates that a new connection was opened.
    case sl_bt_evt_connection_opened_id:
      app_log_info("Connection opened\n");
      sl_bt_sm_increase_security(evt->data.evt_connection_opened.connection);
      connection_handle = evt->data.evt_connection_opened.connection;
      app_log_info("Connection handle: %d\n", connection_handle);
      break;

    // -------------------------------
    // This event indicates that a connection was closed.
    case sl_bt_evt_connection_closed_id:
      // Connection closed
      app_log_info("Connection closed\n");
      connection_handle = 0xFF;
      // Generate data for advertising
      sc = sl_bt_legacy_advertiser_generate_data(advertising_set_handle,
                                                 sl_bt_advertiser_general_discoverable);
      app_assert_status(sc);

      // Restart advertising after client has disconnected.
      sc = sl_bt_legacy_advertiser_start(advertising_set_handle,
                                         sl_bt_advertiser_connectable_scannable);
      app_assert_status(sc);
      break;

    ///////////////////////////////////////////////////////////////////////////
    // Add additional event handlers here as your application requires!      //
    ///////////////////////////////////////////////////////////////////////////
    case sl_bt_evt_sm_passkey_display_id:
      app_log_info("Passkey: %lu\n", evt->data.evt_sm_passkey_display.passkey);
      OSTaskSuspend(sensor_task_tcb_ptr, &err);
      OSTaskSuspend(lcd_task_tcb_ptr, &err);
      memlcd_display_passkey(evt->data.evt_sm_passkey_display.passkey);
      break;

    case sl_bt_evt_sm_bonded_id:
      app_log_info("Successful bonding");
      OSTaskResume(sensor_task_tcb_ptr, &err);
      OSTaskResume(lcd_task_tcb_ptr, &err);
      // bonding handle
      app_log_info("Bonding handle: %d\n", evt->data.evt_sm_bonded.bonding);
      // Post to bond_sem to update bond information
      //OSSemPost(&bond_sem, OS_OPT_POST_1, &local_err);
      //app_assert_status(local_err.Code);
      break;

    case sl_bt_evt_sm_bonding_failed_id:
      OSTaskResume(sensor_task_tcb_ptr, &err);
      OSTaskResume(lcd_task_tcb_ptr, &err);
      app_log_error("Bonding failed. Reason: 0x%04X, Connection: %d\r\n",
              evt->data.evt_sm_bonding_failed.reason,
              evt->data.evt_sm_bonding_failed.connection);
      app_log_error("Device not secure. Closing connection.\r\n");
      sl_bt_connection_close(evt->data.evt_sm_bonding_failed.connection);
      sc = sl_bt_legacy_advertiser_start(advertising_set_handle,
                                         sl_bt_advertiser_connectable_scannable);
      app_assert_status(sc);
      break;

    case sl_bt_evt_gatt_server_characteristic_status_id:
      if (evt->data.evt_gatt_server_characteristic_status.status_flags == sl_bt_gatt_server_client_config) {
        if (evt->data.evt_gatt_server_characteristic_status.client_config_flags & sl_bt_gatt_server_notification) {
          app_log_info("Notifications enabled by client.\n");
        } else {
          app_log_info("Notifications disabled by client.\n");
        }
      }
      break;

    case sl_bt_evt_gatt_server_attribute_value_id:
      if (evt->data.evt_gatt_server_attribute_value.attribute == gattdb_measurement_interval) {
        handle_measurement_interval_write(evt->data.evt_gatt_server_attribute_value.value.data,
                                          evt->data.evt_gatt_server_attribute_value.value.len);
        // Signal the sensor task to update the measurement interval
        OSSemPost(&sensor_data_ready_sem, OS_OPT_POST_1, &local_err);
      }
      break;

    case sl_bt_evt_gatt_server_user_read_request_id:
      if (evt->data.evt_gatt_server_user_read_request.characteristic == gattdb_measurement_interval) {
        handle_measurement_interval_read(evt->data.evt_gatt_server_user_read_request.connection,
                                         evt->data.evt_gatt_server_user_read_request.characteristic);
      }
      break;
    // -------------------------------
    // Default event handler.
    default:
      break;
  }
}
