#ifndef NOTIFICATION_TASK_H
#define NOTIFICATION_TASK_H

#include <stdbool.h>
#include "sl_status.h"

// Task configuration
#define NOTIFICATION_TASK_PRIO       23u
#define NOTIFICATION_TASK_STK_SIZE   256u

/**
 * @brief Initialize the NOTIFICATION task
 *
 * @return SL_STATUS_OK if successful, error code otherwise
 */
sl_status_t notification_task_init(void);

/**
 * @brief Check if NOTIFICATION task is running
 *
 * @return true if running, false otherwise
 */
bool notification_task_is_running(void);

void handle_measurement_interval_write(uint8_t *value, uint16_t length);
void handle_measurement_interval_read(uint16_t connection, uint16_t characteristic);


#endif // NOTIFICATION_TASK_H
