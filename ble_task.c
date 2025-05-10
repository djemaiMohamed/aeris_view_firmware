#include "ble_task.h"
#include "sl_bluetooth.h"
#include "os.h"
#include "sl_bt_api.h"
#include "app_assert.h"
#include "sl_memory_manager.h" // Added for dynamic allocation

// Task control block and stack
static bool    ble_task_running = false;
// Semaphores for synchronization
extern OS_SEM sensor_data_ready_sem;

// Task function prototype
static void ble_task_function(void *p_arg);

/***************************************************************************//**
 * Initialize BLE task.
 ******************************************************************************/
sl_status_t ble_task_init(void)
{
  RTOS_ERR err;

  // Dynamically allocate TCB
  OS_TCB *ble_task_tcb_ptr = (OS_TCB *)sl_malloc(sizeof(OS_TCB));
  app_assert(ble_task_tcb_ptr != DEF_NULL, "Failed to allocate TCB for BLE task");

  // Dynamically allocate stack
  CPU_STK *ble_task_stk_ptr = (CPU_STK *)sl_malloc(BLE_TASK_STK_SIZE * sizeof(CPU_STK));
  app_assert(ble_task_stk_ptr != DEF_NULL, "Failed to allocate stack for BLE task");

  // Create BLE task
  OSTaskCreate(ble_task_tcb_ptr, // Pass pointer to allocated TCB
               "BLE Task",
               ble_task_function,
               DEF_NULL,
               BLE_TASK_PRIO,
               ble_task_stk_ptr, // Pass pointer to allocated stack
               (BLE_TASK_STK_SIZE / 10u),
               BLE_TASK_STK_SIZE,
               0u,
               0u,
               DEF_NULL,
               (OS_OPT_TASK_STK_CLR),
               &err);
  
  // Check if task was created successfully
  if (RTOS_ERR_CODE_GET(err) != RTOS_ERR_NONE) {
    return SL_STATUS_FAIL;
  }
  
  ble_task_running = true;
  return SL_STATUS_OK;
}

/***************************************************************************//**
 * Check if BLE task is running.
 ******************************************************************************/
bool ble_task_is_running(void)
{
  return ble_task_running;
}

/***************************************************************************//**
 * BLE task function.
 ******************************************************************************/
static void ble_task_function(void *p_arg)
{
  RTOS_ERR err;
  sl_bt_msg_t evt;
  
  (void)p_arg; // Unused parameter
  //extern sl_status_t sli_bt_system_start_bluetooth();
  
  // Infinite loop
  while (1) {
    // Process BLE events
    //if (sl_bt_pop_event(&evt)) {
    //  sl_bt_process_event(&evt);
    //}
      // Wait for the semaphore that indicates new sensor data is available
    
    // Delay to allow other tasks to run (10ms)
    OSTimeDlyHMSM(0u, 0u, 0u, 900u,
                  OS_OPT_TIME_HMSM_STRICT,
                  &err);
    
    // Check if error occurred during delay
    if (RTOS_ERR_CODE_GET(err) != RTOS_ERR_NONE) {
      // Handle error (could add application-specific error handling here)
      app_assert(false, "OS delay failed\n");
    }
  }
}
