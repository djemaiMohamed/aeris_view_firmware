#include "os.h"
#include "sl_bt_api.h"
#include "app_log.h"
#include "app_assert.h"
#include "bond_task.h"
#include "nvm3_default.h" // For NVM3 API
#include "sl_common.h"    // For SL_FALLTHROUGH

// Global variable definition
app_bond_info_t g_app_current_bond_info;

// External declaration for the semaphore
OS_SEM bond_sem;
// create a scan semaphore
OS_SEM scan_sem;
// Task Control Block
OS_TCB *bond_task_tcb;
// Task Stack
CPU_STK *bond_task_stack;
// variable to hold connection state

extern OS_SEM ble_notification_sem;

// connection handle
extern uint8_t connection_handle;
extern uint8_t advertising_set_handle; // External declaration

// function to initialize the bond info structure
static void bond_info_init(void) {
  g_app_current_bond_info.is_present = DEF_FALSE;
  memset(&g_app_current_bond_info.address, 0, sizeof(bd_addr));
  g_app_current_bond_info.bonding_handle = SL_BT_INVALID_BONDING_HANDLE;
  g_app_current_bond_info.address_type = 0;
  // Initialize other fields if necessary
  g_app_current_bond_info.security_mode = 0;
  g_app_current_bond_info.key_size = 0;
}

// Helper function to read bonding information from NVM3
static bool read_bonding_info_from_nvm3(app_bond_info_t *bond_info) {
    Ecode_t status;
    uint32_t object_type;
    size_t data_len;

    app_log_info("Reading bonding info from NVM3.\n");
    bond_info_init(); // Initialize to default before reading

    // Check if bond is present
    status = nvm3_readData(nvm3_defaultHandle,
                           NVM3_KEY_BOND_PRESENT,
                           &bond_info->is_present,
                           sizeof(bond_info->is_present));

    if (status != ECODE_NVM3_OK || !bond_info->is_present) {
        if (status == ECODE_NVM3_ERR_KEY_NOT_FOUND) {
            app_log_info("No NVM3_KEY_BOND_PRESENT key found in NVM3.\n");
        } else if (!bond_info->is_present) {
            app_log_info("NVM3_KEY_BOND_PRESENT is false.\n");
        } else {
            app_log_error("Failed to read NVM3_KEY_BOND_PRESENT (0x%lx).\n", status);
        }
        bond_info->is_present = DEF_FALSE; // Ensure it's false
        return false; // No bond marked as present or error reading the flag
    }

    // Read address
    status = nvm3_getObjectInfo(nvm3_defaultHandle, NVM3_KEY_BOND_ADDRESS, &object_type, &data_len);
    if (status == ECODE_NVM3_OK && data_len == sizeof(bd_addr)) {
        status = nvm3_readData(nvm3_defaultHandle,
                               NVM3_KEY_BOND_ADDRESS,
                               &bond_info->address,
                               sizeof(bd_addr));
        if (status != ECODE_NVM3_OK) {
            app_log_error("Failed to read bonding address from NVM3 (key: 0x%lx).\n", status);
            bond_info_init(); return false;
        }
    } else {
        app_log_error("Failed to get bonding address info or wrong size from NVM3 (status: 0x%lx, len: %zu).\n", status, data_len);
        bond_info_init(); return false;
    }

    // Read address type
    status = nvm3_getObjectInfo(nvm3_defaultHandle, NVM3_KEY_BOND_ADDRESS_TYPE, &object_type, &data_len);
     if (status == ECODE_NVM3_OK && data_len == sizeof(uint8_t)) {
        status = nvm3_readData(nvm3_defaultHandle,
                               NVM3_KEY_BOND_ADDRESS_TYPE,
                               &bond_info->address_type,
                               sizeof(bond_info->address_type));
        if (status != ECODE_NVM3_OK) {
            app_log_error("Failed to read bonding address type from NVM3 (key: 0x%lx).\n", status);
            bond_info_init(); return false;
        }
    } else {
        app_log_error("Failed to get bonding address type info or wrong size from NVM3 (status: 0x%lx, len: %zu).\n", status, data_len);
        bond_info_init(); return false;
    }

    app_log_info("Successfully read bonding info from NVM3. Address Type: %d\n", bond_info->address_type);
    // bond_info->is_present is already true from the first read.
    return true;
}

// Function to save bonding information to NVM3 (public)
void save_bonding_info_to_nvm3(bd_addr *address, uint8_t address_type, uint8_t bonding_handle) {
    Ecode_t status;
    CPU_BOOLEAN bond_present_flag = DEF_TRUE;

    app_log_info("Saving bonding info to NVM3. Address: %02X:%02X:%02X:%02X:%02X:%02X, Type: %d, Handle: %d\n",
                 address->addr[5], address->addr[4], address->addr[3],
                 address->addr[2], address->addr[1], address->addr[0],
                 address_type, bonding_handle);

    status = nvm3_writeData(nvm3_defaultHandle, NVM3_KEY_BOND_PRESENT, &bond_present_flag, sizeof(bond_present_flag));
    if (status != ECODE_NVM3_OK) {
        app_log_error("Failed to write NVM3_KEY_BOND_PRESENT (0x%lx).\n", status);
        return;
    }

    status = nvm3_writeData(nvm3_defaultHandle, NVM3_KEY_BOND_ADDRESS, address, sizeof(bd_addr));
    if (status != ECODE_NVM3_OK) {
        app_log_error("Failed to write NVM3_KEY_BOND_ADDRESS (0x%lx).\n", status);
        clear_bonding_info_from_nvm3(); // Attempt to clear to avoid inconsistent state
        return;
    }

    status = nvm3_writeData(nvm3_defaultHandle, NVM3_KEY_BOND_ADDRESS_TYPE, &address_type, sizeof(address_type));
    if (status != ECODE_NVM3_OK) {
        app_log_error("Failed to write NVM3_KEY_BOND_ADDRESS_TYPE (0x%lx).\n", status);
        clear_bonding_info_from_nvm3();
        return;
    }
    // Update global g_app_current_bond_info with the new saved bond
    memcpy(&g_app_current_bond_info.address, address, sizeof(bd_addr));
    g_app_current_bond_info.address_type = address_type;
    g_app_current_bond_info.bonding_handle = bonding_handle; // Store handle too
    g_app_current_bond_info.is_present = DEF_TRUE;

    app_log_info("Bonding info saved to NVM3 successfully.\n");
}

// Function to clear bonding information from NVM3 (public)
void clear_bonding_info_from_nvm3(void) {
    Ecode_t status;
    CPU_BOOLEAN bond_present_flag = DEF_FALSE;

    app_log_info("Clearing bonding info from NVM3.\n");

    status = nvm3_writeData(nvm3_defaultHandle, NVM3_KEY_BOND_PRESENT, &bond_present_flag, sizeof(bond_present_flag));
    if (status != ECODE_NVM3_OK && status != ECODE_NVM3_ERR_KEY_NOT_FOUND) {
        app_log_error("Failed to clear NVM3_KEY_BOND_PRESENT (0x%lx).\n", status);
    }
    // Deleting other keys is optional if NVM3_KEY_BOND_PRESENT is false
    nvm3_deleteObject(nvm3_defaultHandle, NVM3_KEY_BOND_ADDRESS);
    nvm3_deleteObject(nvm3_defaultHandle, NVM3_KEY_BOND_ADDRESS_TYPE);

    bond_info_init(); // Reset global bond info structure
}

// Function to create the bond task
void bond_task_init(void) {
  RTOS_ERR err;

  // Initialize the bond info structure
  bond_info_init();
  // Create the semaphore for bonding
  OSSemCreate(&bond_sem, "Bond Sem", 0, &err);
  // create the semaphore for scanning
  OSSemCreate(&scan_sem, "Scan Sem", 0, &err);
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
  app_log_info("Bond task created\n");
}

// Task function
void bond_task(void *p_arg) {
  (void)p_arg; // Unused parameter
  RTOS_ERR err;
  sl_status_t sc;

  while (DEF_TRUE) {
    // Wait on the semaphore
    OSSemPend(&bond_sem,
              0,                    // Wait indefinitely
              OS_OPT_PEND_BLOCKING,
              NULL,
              &err);
    app_assert_status(err.Code);

    app_log_info("Bond task triggered. Checking NVM3 for bonded device.\n");

    if (read_bonding_info_from_nvm3(&g_app_current_bond_info) && g_app_current_bond_info.is_present) {
        app_log_info("Bonded device found in NVM3. Address: %02X:%02X:%02X:%02X:%02X:%02X, Type: %d\n",
                     g_app_current_bond_info.address.addr[5], g_app_current_bond_info.address.addr[4],
                     g_app_current_bond_info.address.addr[3], g_app_current_bond_info.address.addr[2],
                     g_app_current_bond_info.address.addr[1], g_app_current_bond_info.address.addr[0],
                     g_app_current_bond_info.address_type);
        app_log_info("Attempting to connect to bonded device (Type: %d) as central.\n", g_app_current_bond_info.address_type);
        sc = sl_bt_connection_open(g_app_current_bond_info.address,
                                   g_app_current_bond_info.address_type,
                                   sl_bt_gap_phy_1m, // Use 1M PHY, or make configurable
                                   &connection_handle); // Update the global connection_handle

        if (sc == SL_STATUS_OK) {
            app_log_info("Connection request sent to bonded device. Handle will be updated by connection opened event.\n");
            // If successful, sl_bt_evt_connection_opened_id will be triggered.
            // Advertising (if any was running for other purposes) is typically stopped in sl_bt_evt_connection_opened_id.
            //Delay for a short time to allow the connection to be established
            // Triger notification task to send notifications
            // delay is not necessary here as the connection event will be handled in app.c
            //OSTimeDlyHMSM(0, 0, 4, 0, OS_OPT_TIME_HMSM_STRICT, &err);
            //OSSemPost(&ble_notification_sem, OS_OPT_POST_1, &err);
        } else {
            app_log_error("Failed to initiate connection to bonded device (Type: %d), status: 0x%04X\n",
                          g_app_current_bond_info.address_type, sc);
            // Bond task has attempted connection. No further action here regarding advertising.
            // The device remains disconnected. Further reconnection attempts might be triggered by other events
            // or timers if implemented.
        }
    } else { // No valid bond info found or error reading it
      app_log_info("No valid bonding information found in NVM3 by bond_task, or error reading it. Bond task will not attempt connection.\n");
      // If this device also has a peripheral role, app.c's event handlers should manage advertising
      // based on overall system state (e.g., start advertising if disconnected and no bond). Bond_task does not handle this.
    }
  }
}
