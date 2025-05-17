#include "os.h"
#include "sl_bt_api.h"
#include "app_log.h"
#include "app_assert.h"
#include "bond_task.h"

// Global variable definition
app_bond_info_t g_app_current_bond_info;

// External declaration for the semaphore
OS_SEM bond_sem;
// Task Control Block
OS_TCB *bond_task_tcb;
// Task Stack
CPU_STK *bond_task_stack;
// variable to hold connection state
bool is_connected = false;

extern OS_SEM ble_notification_sem;

// connection handle
extern uint8_t connection_handle;

// function to initialize the bond info structure
static void bond_info_init(void) {
  g_app_current_bond_info.is_present = DEF_FALSE;
  memset(&g_app_current_bond_info.address, 0, sizeof(bd_addr));
  g_app_current_bond_info.bonding_handle = SL_BT_INVALID_BONDING_HANDLE;
  g_app_current_bond_info.address_type = 0;
}

// Function to create the bond task
void bond_task_init(void) {
  RTOS_ERR err;

  // Initialize the bond info structure
  bond_info_init();
  // Create the semaphore for bonding
  OSSemCreate(&bond_sem, "Bond Sem", 0, &err);
  app_assert_status(err.Code);
  // Dynamically allocate TCB
  bond_task_tcb = (OS_TCB *)sl_malloc(sizeof(OS_TCB));
  // Dynamically allocate stack
  bond_task_stack = (CPU_STK *)sl_malloc(BOND_TASK_STK_SIZE * sizeof(CPU_STK));

  OSTaskCreate(bond_task_tcb,
               "Bond Task",
               bond_task,
               DEF_NULL,
               BOND_TASK_PRIO, // Priority
               &bond_task_stack[0],
               (BOND_TASK_STK_SIZE / 10u),
               BOND_TASK_STK_SIZE,
               0u,
               0u,
               DEF_NULL,
               (OS_OPT_TASK_STK_CLR | OS_OPT_TASK_STK_CHK),
               &err);
  app_assert_status(err.Code);
  app_log_info("Bond task created\n");
}

// Task function
void bond_task(void *p_arg) {
  (void)p_arg; // Unused parameter
  RTOS_ERR err;

  while (DEF_TRUE) {
    // Wait on the semaphore
    OSSemPend(&bond_sem,
              0,                    // Wait indefinitely
              OS_OPT_PEND_BLOCKING,
              NULL,
              &err);

    app_log_info("Bond task woken up, checking for existing bonds.\n");

    uint32_t num_bondings = 0;
    size_t total_bondings_actual_len;
    uint8_t bonding_buffer[2];

    sl_status_t sc = sl_bt_sm_get_bonding_handles(0, &num_bondings,
                                                  sizeof(bonding_buffer),
                                                  &total_bondings_actual_len,
                                                  bonding_buffer);

    if (num_bondings > 0) {
      app_log_info("Found %lu bonding(s).\n", num_bondings);

      bool found_valid_bond = false;

      for (uint32_t i = 0; i < num_bondings; i++) {
        uint8_t bonding_handle = bonding_buffer[i];
        app_log_info("Checking bonding handle: %u\n", bonding_handle);

        sc = sl_bt_sm_get_bonding_details(bonding_handle,
                                          &g_app_current_bond_info.address,
                                          &g_app_current_bond_info.address_type,
                                          &g_app_current_bond_info.security_mode,
                                          &g_app_current_bond_info.key_size);

        if (sc == SL_STATUS_OK) {
          if (g_app_current_bond_info.address_type > 1) {
            app_log_error("Invalid address type (%u) for bonding handle %u. Skipping.\n",
                          g_app_current_bond_info.address_type, bonding_handle);
            continue;
          }

          g_app_current_bond_info.bonding_handle = bonding_handle;
          g_app_current_bond_info.is_present = DEF_TRUE;

          //app_log_info("Connecting to bonded device: %02X:%02X:%02X:%02X:%02X:%02X (type %u)\n",
          //             g_app_current_bond_info.address.addr[5],
          //             g_app_current_bond_info.address.addr[4],
          //             g_app_current_bond_info.address.addr[3],
          //             g_app_current_bond_info.address.addr[2],
          //             g_app_current_bond_info.address.addr[1],
          //             g_app_current_bond_info.address.addr[0],
          //             g_app_current_bond_info.address_type);
          sl_bt_sm_set_bondable_mode(0);
          sc = sl_bt_connection_open(g_app_current_bond_info.address,
                                     g_app_current_bond_info.address_type,
                                     sl_bt_gap_phy_1m,
                                     &connection_handle);

          if (sc == SL_STATUS_OK) {
            app_log_info("Connection request sent successfully, handle: %u\n", connection_handle);
            is_connected = true;
            // Signal BLE notification semaphore
            OSSemPost(&ble_notification_sem, OS_OPT_POST_1, &err);
            app_assert(err.Code == RTOS_ERR_NONE, "BLE notification semaphore post failed");
            app_log_info("Connected to bonded device.\n");
          } else {
            app_log_error("Failed to connect to bonded device, status: 0x%04X\n", sc);
            is_connected = false;
          }

          found_valid_bond = true;
          break; // Stop after first successful bonding
        } else {
          app_log_warning("Failed to get bonding details for handle %u, status: 0x%04X\n", bonding_handle, sc);
        }
      }

      if (!found_valid_bond) {
        g_app_current_bond_info.is_present = DEF_FALSE;
        app_log_warning("No valid bonding details found.\n");
      }

    } else {
      g_app_current_bond_info.is_present = DEF_FALSE;
      app_log_info("No existing bonds found.\n");
    }
  }
}
