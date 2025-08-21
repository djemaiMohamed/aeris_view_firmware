#ifndef CALIBRATION_TASK_H
#define CALIBRATION_TASK_H

#include <stdint.h>
#include <stdbool.h>
#include "sl_status.h"
#include "os.h"

// Task configuration
#define CALIBRATION_TASK_PRIO       21u
#define CALIBRATION_TASK_STK_SIZE   256u

// Semaphore used to trigger calibration from GATT write
extern OS_SEM calibration_task_sem;

// Initialize the calibration task (create task and semaphore)
sl_status_t calibration_task_init(void);

// Query if calibration task is running
bool calibration_task_is_running(void);

// Handle write to run-calibration characteristic; parses target ppm and posts semaphore
void handle_calibration_write(const uint8_t *value, uint16_t length);

#endif // CALIBRATION_TASK_H
