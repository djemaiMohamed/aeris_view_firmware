#ifndef BOND_TASK_H
#define BOND_TASK_H

#include <stdbool.h> // For bool type
#include "sl_bt_api.h" // For bd_addr
#include "os.h"        // For CPU_BOOLEAN

// Task configuration
#define BOND_TASK_PRIO       22u
#define BOND_TASK_STK_SIZE   256u

// NVM3 Keys for storing bonding information
// Ensure these keys are in the user application's designated NVM3 key range
// and do not conflict with other keys used by the SDK or other application modules.
// Using keys within the recommended Bluetooth user range (0x4000-0x5FFF)
#define NVM3_KEY_BOND_DATA_BASE      (0x4000) // Start of Bluetooth user key range
#define NVM3_KEY_BOND_PRESENT        (NVM3_KEY_BOND_DATA_BASE + 0x00) // Now 0x4000
#define NVM3_KEY_BOND_ADDRESS        (NVM3_KEY_BOND_DATA_BASE + 0x01) // Now 0x4001
#define NVM3_KEY_BOND_ADDRESS_TYPE   (NVM3_KEY_BOND_DATA_BASE + 0x02) // Now 0x4002

// Structure to hold information about the bonded device
typedef struct {
  bd_addr address;
  uint8_t bonding_handle;
  uint8_t address_type;
  uint8_t security_mode;
  uint8_t key_size;
  CPU_BOOLEAN is_present; // Using Micrium's boolean type
} app_bond_info_t;

// Global variable to store the information of the (last checked) bonded device
// This will be updated by the Bluetooth event handler in app.c
extern app_bond_info_t g_app_current_bond_info;

// Declaration for the task creation function
void bond_task_init(void);
void bond_task(void *p_arg);

#endif // BOND_TASK_H
