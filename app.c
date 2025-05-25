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
#include <stdbool.h>
#include "nvm3_default.h"

// The advertising set handle allocated from Bluetooth stack.
uint8_t advertising_set_handle = 0xff; // Removed static
static RTOS_ERR err;
// connection handle
uint8_t connection_handle = 0xFF;

//pairing mode
bool pairing_mode = true;

// A semaphore to trigger bonding process
extern OS_SEM bond_sem;
extern OS_SEM sensor_data_ready_sem;
extern OS_SEM *ble_notification_sem;
extern OS_SEM scan_sem;
extern OS_TCB *bond_task_tcb;
extern OS_TCB *notification_task_tcb_ptr;
extern OS_TCB *lcd_task_tcb_ptr;
extern OS_TCB *sensor_task_tcb_ptr;

// Initialize g_app_current_bond_info
extern app_bond_info_t g_app_current_bond_info;

// Function prototypes for NVM3 operations (defined in bond_task.c)
extern void save_bonding_info_to_nvm3(bd_addr *address, uint8_t address_type, uint8_t bonding_handle);
extern void clear_bonding_info_from_nvm3(void);

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
  RTOS_ERR err;
  // Suspend the sensor task
  OSTaskSuspend(sensor_task_tcb_ptr, &err);
  app_assert_status(err.Code);
  // Suspend the LCD task
  OSTaskSuspend(lcd_task_tcb_ptr, &err);
  app_assert_status(err.Code);
  // Suspend the bond task
  OSTaskSuspend(bond_task_tcb, &err);
  app_assert_status(err.Code);
  // Suspend the notification task
  OSTaskSuspend(notification_task_tcb_ptr, &err);
  app_assert_status(err.Code);
  app_log_info("Tasks suspended.\n");
}

// function to resume tasks
static void resume_tasks(void) {
  RTOS_ERR err;
  // Resume the sensor task
  OSTaskResume(sensor_task_tcb_ptr, &err);
  app_assert_status(err.Code);
  // Resume the LCD task
  OSTaskResume(lcd_task_tcb_ptr, &err);
  app_assert_status(err.Code);
  // Resume the bond task
  OSTaskResume(bond_task_tcb, &err);
  app_assert_status(err.Code);
  // Resume the notification task
  OSTaskResume(notification_task_tcb_ptr, &err);
  app_assert_status(err.Code);
  app_log_info("Tasks resumed.\n");
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
      // Clear any potentially partially saved or outdated bonding information
      clear_bonding_info_from_nvm3();
      app_assert_status(sc);
      app_log_info("All previous bondings deleted at startup.\n");
      sl_bt_sm_configure(SL_BT_SM_CONFIGURATION_BONDING_REQUIRED |
                         SL_BT_SM_CONFIGURATION_MITM_REQUIRED, sl_bt_sm_io_capability_displayonly);
      sl_bt_sm_set_bondable_mode(1);
      //sl_bt_sm_store_bonding_configuration(32, 1);
      app_log_info("bondable mode is set");
      int16_t pwr_min, pwr_max;
      sl_bt_system_set_tx_power(0, 0, &pwr_min, &pwr_max);

      // Create an advertising set.
      sc = sl_bt_advertiser_create_set(&advertising_set_handle);
      // Generate data for advertising
      sc = sl_bt_legacy_advertiser_generate_data(advertising_set_handle,
                                                 sl_bt_advertiser_general_discoverable);
      // Set advertising interval to 100ms.
      sc = sl_bt_advertiser_set_timing(
        advertising_set_handle,
        1600, // min. adv. interval (milliseconds * 1.6)
        1600, // max. adv. interval (milliseconds * 1.6)
        0,   // adv. duration
        0);  // max. num. adv. events
      // Start advertising and enable connections.
      sc = sl_bt_legacy_advertiser_start(advertising_set_handle,
                                         sl_bt_advertiser_connectable_scannable);
      break;

    // -------------------------------
    // This event indicates that a new connection was opened.
    case sl_bt_evt_connection_opened_id:
      connection_handle = evt->data.evt_connection_opened.connection;

      sl_bt_connection_set_parameters(evt->data.evt_connection_opened.connection, 160, 200, 4, 600, 0, 0xffff);;

      if(pairing_mode) {
        suspend_tasks();
        app_log_info("Pairing mode enabled.\n");
      } else {
        app_log_info("No pairing mode.\n");
        // check if bonding handle is valid
        if (evt->data.evt_connection_opened.bonding == SL_BT_INVALID_BONDING_HANDLE) {
          app_log_error("Invalid bonding handle. Bonding failed.\n");
          break;
        }
      }
      // inscrease security level
      sc = sl_bt_sm_increase_security(evt->data.evt_connection_opened.connection);
      // check is status is ok
      if (sc != SL_STATUS_OK) {
        app_log_error("Failed to increase security level. Status: 0x%04X\n", sc);
        break;
      }
      app_log_info("Security level increased.\n");
      app_log_info("Connection opened. Handle: %d\n", evt->data.evt_connection_opened.connection);
      // Stop advertising
      sl_bt_advertiser_stop(advertising_set_handle);
      app_log_info("Advertising stopped.\n");
      // Triger notification task to send notifications
      OSSemPost(&ble_notification_sem, OS_OPT_POST_1, &err);
      break;

    case sl_bt_evt_connection_parameters_id:
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
          app_log_info("Authenticated Secure Connections pairing with encryption (BT 4.2 LE Secure Pairing)\r\n");
          break;
        default:
          break;
      }
      uint16_t interval = evt->data.evt_connection_parameters.interval, latency = evt->data.evt_connection_parameters.latency, timeout = evt->data.evt_connection_parameters.timeout;
      app_log_info("connection interval %d, latency %d, timeout %d ms\n", interval, latency, timeout * 10);

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
      connection_handle = SL_BT_INVALID_CONNECTION_HANDLE;
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
      pairing_mode = false;
      app_log_info("Pairing mode disabled.\n");
      app_log_info("Bonding completed. Connection: %d, Security mode: %d, Bonding handle: %d\n",
                   evt->data.evt_sm_bonded.connection,
                   evt->data.evt_sm_bonded.security_mode,
                   evt->data.evt_sm_bonded.bonding);

      // Retrieve connection properties to get the remote device's address
      {
        sl_status_t sc_addr;
        bd_addr remote_address;
        uint8_t remote_address_type;
        sc_addr = sl_bt_connection_get_remote_address(evt->data.evt_sm_bonded.connection, &remote_address, &remote_address_type);

        if (SL_STATUS_OK == sc_addr) {
          app_log_info("Remote address: %02X:%02X:%02X:%02X:%02X:%02X, type: %d\n",
                       remote_address.addr[5], remote_address.addr[4], remote_address.addr[3],
                       remote_address.addr[2], remote_address.addr[1], remote_address.addr[0],
                       remote_address_type);
          // Save bonding information to NVM3
          save_bonding_info_to_nvm3(&remote_address, remote_address_type, evt->data.evt_sm_bonded.bonding);
        } else {
          app_log_error("Failed to get remote address for bonding, sc: 0x%04X\n", sc_addr);
        }
      }
      // 3 seconds delay before resuming tasks
      //OSTimeDlyHMSM(0, 0, 3, 0, OS_OPT_TIME_HMSM_STRICT, &local_err);
      // Resume tasks after bonding is completed
      resume_tasks();
      break;

    case sl_bt_evt_sm_bonding_failed_id:
      app_log_error("Bonding failed. Connection: %d, Reason: 0x%04X\n",
                    evt->data.evt_sm_bonding_failed.connection,
                    evt->data.evt_sm_bonding_failed.reason);
      // Clear any potentially partially saved or outdated bonding information
      clear_bonding_info_from_nvm3();
      // Resume tasks after bonding failure
      resume_tasks();
      app_log_error("Device not secure. Closing connection.\n");
      sl_bt_connection_close(evt->data.evt_sm_bonding_failed.connection);
      sc = sl_bt_legacy_advertiser_start(
          advertising_set_handle,
          sl_bt_advertiser_connectable_scannable);
      app_assert_status(sc);
      app_log_info("Restarted advertising after bonding failure.\n");
      break;

    case sl_bt_evt_gatt_server_characteristic_status_id:
      if (evt->data.evt_gatt_server_characteristic_status.status_flags == sl_bt_gatt_server_client_config) {
        if (evt->data.evt_gatt_server_characteristic_status.client_config_flags & sl_bt_gatt_server_notification) {
          //app_log_info("Notifications enabled by client.\n");
        } else {
          //app_log_info("Notifications disabled by client.\n");
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
