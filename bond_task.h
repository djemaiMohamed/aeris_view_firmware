\
#ifndef BOND_TASK_H
#define BOND_TASK_H

#include "sl_bt_api.h" // For bd_addr
#include "os.h"        // For CPU_BOOLEAN

// Task configuration
#define BOND_TASK_PRIO       22u
#define BOND_TASK_STK_SIZE   256u

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
