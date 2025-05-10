#ifndef BLE_TASK_H
#define BLE_TASK_H

#include <stdbool.h>
#include "sl_status.h"

// Task configuration
#define BLE_TASK_PRIO       22u
#define BLE_TASK_STK_SIZE   256u

/**
 * @brief Initialize the BLE task
 *
 * @return SL_STATUS_OK if successful, error code otherwise
 */
sl_status_t ble_task_init(void);

/**
 * @brief Check if BLE task is running
 *
 * @return true if running, false otherwise
 */
bool ble_task_is_running(void);

#endif // BLE_TASK_H
