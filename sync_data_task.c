#include "sync_data_task.h"
#include "sensor_task.h"
#include "app_assert.h"
#include "app_log.h"
#include "sl_memory_manager.h"
#include "sl_bt_api.h"
#include "gatt_db.h"
#include "nvm3_default.h"
#include "sl_sleeptimer.h"
#include <string.h>

// External handles
extern uint8_t connection_handle;
// Defined in app.c
extern uint8_t sync_data_id_notification_enabled;

// Local state
static bool sync_running = false;
static uint32_t epoch_actual = 0;           // Epoch at device boot time
static uint32_t last_sync_timestamp = 0;    // Last sync timestamp from phone
static uint32_t boot_tick_count = 0;        // Tick count at boot for uptime calculation
static bool epoch_valid = false;            // Whether epoch has been set by phone

// Connection parameter tuning removed; rely on default stack behavior.

// NVM3 circular buffer indices
static uint16_t write_index = 0;            // Next position to write
static uint16_t sample_count = 0;           // Number of samples stored

// RTOS objects
OS_TCB *sync_data_task_tcb_ptr;
CPU_STK *sync_data_task_stk_ptr;
OS_SEM sync_data_task_sem;

static void sync_data_task(void *p_arg);
static sl_status_t read_sample_from_nvm3(uint16_t index, sample_t *sample);
static void purge_legacy_nvm3_keys(void);

static uint16_t count_samples_newer_than(uint32_t after_timestamp)
{
    sample_t sample;

    // Calculate the starting index based on sample count and write index
    uint16_t start_index;
    if (sample_count < MAX_SAMPLES) {
        start_index = 0;
    } else {
        start_index = write_index; // Oldest sample in circular buffer
    }

    uint16_t eligible = 0;
    for (uint16_t i = 0; i < sample_count; i++) {
        uint16_t idx = (start_index + i) % MAX_SAMPLES;

        if (read_sample_from_nvm3(idx, &sample) != SL_STATUS_OK) {
            continue;
        }

        uint32_t sample_timestamp = sample.timestamp;
        if (sample_timestamp & 0x80000000) {
            if (!epoch_valid) {
                continue;
            }
            uint32_t sample_uptime = sample_timestamp & 0x7FFFFFFF;
            sample_timestamp = epoch_actual + sample_uptime;
        }

        if (sample_timestamp > after_timestamp) {
            eligible++;
        }
    }

    return eligible;
}

/***************************************************************************//**
 * Get current uptime in seconds using sleeptimer
 ******************************************************************************/
uint32_t sync_data_get_uptime_sec(void)
{
    uint32_t current_ticks = sl_sleeptimer_get_tick_count();
    uint32_t elapsed_ticks = current_ticks - boot_tick_count;
    uint32_t freq = sl_sleeptimer_get_timer_frequency();
    return elapsed_ticks / freq;
}

// Delete any legacy keys that were in the Bluetooth-reserved range to reclaim space
// and prevent overlap with bonding data. This runs once at startup.
static void purge_legacy_nvm3_keys(void)
{
    // Legacy metadata keys
    const nvm3_ObjectKey_t legacy_meta_keys[] = { 0x0F00, 0x0F01, 0x0F02, 0x0F03 };
    for (size_t i = 0; i < (sizeof(legacy_meta_keys) / sizeof(legacy_meta_keys[0])); i++) {
        (void)nvm3_deleteObject(nvm3_defaultHandle, legacy_meta_keys[i]);
    }

    // Legacy sample range starting at 0x1000
    const nvm3_ObjectKey_t legacy_sample_base = 0x1000;
    for (uint16_t i = 0; i < MAX_SAMPLES; i++) {
        (void)nvm3_deleteObject(nvm3_defaultHandle, legacy_sample_base + i);
    }

    app_log_info("Legacy NVM3 keys purged\n");
}

/***************************************************************************//**
 * Get the stored epoch timestamp
 ******************************************************************************/
uint32_t sync_data_get_epoch(void)
{
    return epoch_actual;
}

/***************************************************************************//**
 * Get the number of samples currently stored
 ******************************************************************************/
uint16_t sync_data_get_sample_count(void)
{
    return sample_count;
}

/***************************************************************************//**
 * Load metadata from NVM3 (indices and epoch)
 ******************************************************************************/
static sl_status_t load_metadata_from_nvm3(void)
{
    Ecode_t status;
    uint32_t type;
    size_t len;
    
    // Load write index
    status = nvm3_getObjectInfo(nvm3_defaultHandle, NVM3_META_KEY_WRITE_IDX, &type, &len);
    if (status == ECODE_NVM3_OK && len == sizeof(write_index)) {
        status = nvm3_readData(nvm3_defaultHandle, NVM3_META_KEY_WRITE_IDX, &write_index, sizeof(write_index));
        if (status != ECODE_NVM3_OK) {
            write_index = 0;
        }
    } else {
        write_index = 0;
    }
    
    // Load sample count
    status = nvm3_getObjectInfo(nvm3_defaultHandle, NVM3_META_KEY_READ_IDX, &type, &len);
    if (status == ECODE_NVM3_OK && len == sizeof(sample_count)) {
        status = nvm3_readData(nvm3_defaultHandle, NVM3_META_KEY_READ_IDX, &sample_count, sizeof(sample_count));
        if (status != ECODE_NVM3_OK) {
            sample_count = 0;
        }
    } else {
        sample_count = 0;
    }
    
    // Load epoch
    status = nvm3_getObjectInfo(nvm3_defaultHandle, NVM3_META_KEY_EPOCH, &type, &len);
    if (status == ECODE_NVM3_OK && len == sizeof(epoch_actual)) {
        status = nvm3_readData(nvm3_defaultHandle, NVM3_META_KEY_EPOCH, &epoch_actual, sizeof(epoch_actual));
        if (status != ECODE_NVM3_OK) {
            epoch_actual = 0;
            epoch_valid = false;
        } else {
            // Epoch is valid if it was previously saved (non-zero value)
            epoch_valid = (epoch_actual != 0);
        }
    } else {
        epoch_actual = 0;
        epoch_valid = false;
    }
    
    app_log_info("NVM3 metadata loaded: write_idx=%u, sample_count=%u, epoch=%lu, valid=%d\n", 
                 write_index, sample_count, (unsigned long)epoch_actual, epoch_valid);
    
    return SL_STATUS_OK;
}

/***************************************************************************//**
 * Save metadata to NVM3
 ******************************************************************************/
static sl_status_t save_metadata_to_nvm3(void)
{
    Ecode_t status;
    
    status = nvm3_writeData(nvm3_defaultHandle, NVM3_META_KEY_WRITE_IDX, &write_index, sizeof(write_index));
    if (status != ECODE_NVM3_OK) {
        app_log_error("Failed to save write_index to NVM3: 0x%lx\n", (unsigned long)status);
        return SL_STATUS_FAIL;
    }
    
    status = nvm3_writeData(nvm3_defaultHandle, NVM3_META_KEY_READ_IDX, &sample_count, sizeof(sample_count));
    if (status != ECODE_NVM3_OK) {
        app_log_error("Failed to save sample_count to NVM3: 0x%lx\n", (unsigned long)status);
        return SL_STATUS_FAIL;
    }
    
    status = nvm3_writeData(nvm3_defaultHandle, NVM3_META_KEY_EPOCH, &epoch_actual, sizeof(epoch_actual));
    if (status != ECODE_NVM3_OK) {
        app_log_error("Failed to save epoch to NVM3: 0x%lx\n", (unsigned long)status);
        return SL_STATUS_FAIL;
    }
    
    return SL_STATUS_OK;
}

/***************************************************************************//**
 * Get current absolute timestamp (epoch + uptime)
 ******************************************************************************/
uint32_t sync_data_get_current_timestamp(void)
{
    if (!epoch_valid) {
        return 0;
    }
    return epoch_actual + sync_data_get_uptime_sec();
}

/***************************************************************************//**
 * Check if epoch time has been set by phone
 ******************************************************************************/
bool sync_data_epoch_is_valid(void)
{
    return epoch_valid;
}

/***************************************************************************//**
 * Save a sensor sample to NVM3 storage
 ******************************************************************************/
sl_status_t sync_data_save_sample(uint16_t co2, 
                                   int32_t temp_mdeg_c, uint32_t humidity,
                                   uint8_t battery)
{
    sample_t sample;
    Ecode_t status;
    
    // Calculate absolute timestamp
    uint32_t current_timestamp = sync_data_get_current_timestamp();
    
    // If epoch is not set, we cannot save with valid timestamp
    // Still save with timestamp=0, will be updated on first sync
    if (!epoch_valid) {
        // Store uptime temporarily - will need special handling
        // Use a marker (high bit set) to indicate this is uptime, not absolute
        current_timestamp = sync_data_get_uptime_sec() | 0x80000000;
        app_log_warning("Saving sample without valid epoch (uptime=%lu)\n", 
                       (unsigned long)(current_timestamp & 0x7FFFFFFF));
    }
    
    // Fill sample structure
    sample.timestamp = current_timestamp;
    sample.co2 = co2;
    // Convert from milli-degrees to centi-degrees (divide by 10)
    sample.temperature_c = (int16_t)(temp_mdeg_c / 10);
    sample.humidity = (uint16_t)humidity;
    sample.battery_percent = battery;
    sample.reserved = 0;
    
    // Calculate NVM3 key for this sample
    nvm3_ObjectKey_t key = NVM3_SAMPLE_KEY_BASE + write_index;
    
    // Write sample to NVM3
    status = nvm3_writeData(nvm3_defaultHandle, key, &sample, sizeof(sample));
    if (status != ECODE_NVM3_OK) {
        app_log_error("Failed to write sample to NVM3 key 0x%lx: 0x%lx\n", 
                      (unsigned long)key, (unsigned long)status);
        return SL_STATUS_FAIL;
    }
    
    // Update write index (circular buffer)
    write_index = (write_index + 1) % MAX_SAMPLES;
    
    // Update sample count (cap at MAX_SAMPLES)
    if (sample_count < MAX_SAMPLES) {
        sample_count++;
    }
    
    // Save metadata every sample (could optimize to save less frequently)
    save_metadata_to_nvm3();
    
    app_log_info("Sample saved: ts=%lu, co2=%u, temp_c=%d, humidity=%u (count=%u)\n",
                 (unsigned long)sample.timestamp, co2, sample.temperature_c, sample.humidity,
                 sample_count);
    
    return SL_STATUS_OK;
}

/***************************************************************************//**
 * Read a sample from NVM3 by index
 ******************************************************************************/
static sl_status_t read_sample_from_nvm3(uint16_t index, sample_t *sample)
{
    Ecode_t status;
    nvm3_ObjectKey_t key = NVM3_SAMPLE_KEY_BASE + index;
    uint32_t type;
    size_t len;
    
    // Check if object exists
    status = nvm3_getObjectInfo(nvm3_defaultHandle, key, &type, &len);
    if (status != ECODE_NVM3_OK) {
        return SL_STATUS_NOT_FOUND;
    }
    
    if (len != sizeof(sample_t)) {
        app_log_error("Sample size mismatch at index %u: expected %u, got %u\n",
                      index, (unsigned)sizeof(sample_t), (unsigned)len);
        return SL_STATUS_FAIL;
    }
    
    // Read sample data
    status = nvm3_readData(nvm3_defaultHandle, key, sample, sizeof(sample_t));
    if (status != ECODE_NVM3_OK) {
        app_log_error("Failed to read sample from NVM3 key 0x%lx: 0x%lx\n",
                      (unsigned long)key, (unsigned long)status);
        return SL_STATUS_FAIL;
    }
    
    return SL_STATUS_OK;
}

/***************************************************************************//**
 * Delete a sample from NVM3 (currently unused - samples are kept until overwritten)
 ******************************************************************************/
__attribute__((unused))
static sl_status_t delete_sample_from_nvm3(uint16_t index)
{
    Ecode_t status;
    nvm3_ObjectKey_t key = NVM3_SAMPLE_KEY_BASE + index;
    
    status = nvm3_deleteObject(nvm3_defaultHandle, key);
    if (status != ECODE_NVM3_OK && status != ECODE_NVM3_ERR_KEY_NOT_FOUND) {
        app_log_error("Failed to delete sample from NVM3 key 0x%lx: 0x%lx\n",
                      (unsigned long)key, (unsigned long)status);
        return SL_STATUS_FAIL;
    }
    
    return SL_STATUS_OK;
}

/***************************************************************************//**
 * Send a historical sample as BLE notification
 * Format: 12 bytes = timestamp(4) + co2(2) + temperature(2) + humidity(2) + flags(2)
 ******************************************************************************/
static sl_status_t send_sample_notification(const sample_t *sample, uint32_t timestamp)
{
    sl_status_t sc;
    uint8_t data[12];
    
    // Pack timestamp (4 bytes, little-endian)
    data[0] = (uint8_t)(timestamp & 0xFF);
    data[1] = (uint8_t)((timestamp >> 8) & 0xFF);
    data[2] = (uint8_t)((timestamp >> 16) & 0xFF);
    data[3] = (uint8_t)((timestamp >> 24) & 0xFF);
    
    // Pack CO2 (2 bytes, little-endian)
    data[4] = (uint8_t)(sample->co2 & 0xFF);
    data[5] = (uint8_t)((sample->co2 >> 8) & 0xFF);
    
    // Pack temperature (2 bytes, little-endian, centi-degrees)
    data[6] = (uint8_t)(sample->temperature_c & 0xFF);
    data[7] = (uint8_t)((sample->temperature_c >> 8) & 0xFF);
    
    // Pack humidity (2 bytes, little-endian)
    data[8] = (uint8_t)(sample->humidity & 0xFF);
    data[9] = (uint8_t)((sample->humidity >> 8) & 0xFF);
    
    // Flags: 0x01 = historical data
    data[10] = 0x01;
    // Battery percentage
    data[11] = sample->battery_percent;
    
    // Send notification using sync_data_id characteristic
    sc = sl_bt_gatt_server_send_notification(
        connection_handle,
        gattdb_sync_data_id,
        sizeof(data),
        data
    );
    
    if (sc != SL_STATUS_OK) {
        app_log_error("Failed to send sample notification: 0x%04lx\n", (unsigned long)sc);
    }
    
    return sc;
}

/***************************************************************************//**
 * Send sync complete notification
 ******************************************************************************/
static sl_status_t send_sync_complete_notification(uint16_t samples_sent)
{
    sl_status_t sc;
    uint8_t data[12];
    
    // Clear data
    memset(data, 0, sizeof(data));
    
    // Samples sent count in first 2 bytes
    data[0] = (uint8_t)(samples_sent & 0xFF);
    data[1] = (uint8_t)((samples_sent >> 8) & 0xFF);
    
    // Flags: 0x02 = sync complete
    data[10] = 0x02;
    data[11] = 0x00;
    
    sc = sl_bt_gatt_server_send_notification(
        connection_handle,
        gattdb_sync_data_id,
        sizeof(data),
        data
    );
    
    return sc;
}

/***************************************************************************//**
 * Handle write to sync_data_id characteristic
 ******************************************************************************/
void handle_sync_data_write(const uint8_t *value, uint16_t length)
{
    RTOS_ERR err;
    
    if (!value || length != 8) {
        app_log_error("Sync data write invalid: length=%u (expected 8)\n", length);
        return;
    }
    
    // Parse 8-byte value:
    // MSB 4 bytes (bytes 0-3): current_epoch_timestamp
    // LSB 4 bytes (bytes 4-7): last_sync_timestamp
    uint32_t current_epoch = (uint32_t)value[0] |
                             ((uint32_t)value[1] << 8) |
                             ((uint32_t)value[2] << 16) |
                             ((uint32_t)value[3] << 24);
    
    uint32_t last_sync = (uint32_t)value[4] |
                         ((uint32_t)value[5] << 8) |
                         ((uint32_t)value[6] << 16) |
                         ((uint32_t)value[7] << 24);
    
    app_log_info("Sync data write: current_epoch=%lu, last_sync=%lu\n",
                 (unsigned long)current_epoch, (unsigned long)last_sync);
    
    // Update device epoch (this is the epoch at boot time)
    epoch_actual = current_epoch - sync_data_get_uptime_sec();
    epoch_valid = true;
    last_sync_timestamp = last_sync;
    
    // Save updated epoch to NVM3
    save_metadata_to_nvm3();
    
    // Post semaphore to wake sync task
    OSSemPost(&sync_data_task_sem, OS_OPT_POST_1, &err);
    if (err.Code != RTOS_ERR_NONE) {
        app_log_error("Failed to post sync semaphore: %d\n", (int)err.Code);
    }
}

/***************************************************************************//**
 * Handle read from sync_data_id characteristic
 ******************************************************************************/
void handle_sync_data_read(uint16_t connection, uint16_t characteristic)
{
    uint8_t data[8];
    sl_status_t sc;
    
    // Return current state: sample_count (2 bytes) + current_epoch (4 bytes) + padding (2 bytes)
    data[0] = (uint8_t)(sample_count & 0xFF);
    data[1] = (uint8_t)((sample_count >> 8) & 0xFF);
    
    uint32_t current_epoch = epoch_actual + sync_data_get_uptime_sec();
    data[2] = (uint8_t)(current_epoch & 0xFF);
    data[3] = (uint8_t)((current_epoch >> 8) & 0xFF);
    data[4] = (uint8_t)((current_epoch >> 16) & 0xFF);
    data[5] = (uint8_t)((current_epoch >> 24) & 0xFF);
    data[6] = sync_running ? 1 : 0;
    data[7] = 0;
    
    sc = sl_bt_gatt_server_send_user_read_response(
        connection,
        characteristic,
        0, // Success
        sizeof(data),
        data,
        NULL
    );
    
    if (sc == SL_STATUS_OK) {
        app_log_info("Sync data read: count=%u, epoch=%lu, running=%d\n",
                     sample_count, (unsigned long)current_epoch, sync_running);
    } else {
        app_log_error("Failed to send sync data read response: 0x%04lx\n", (unsigned long)sc);
    }
}

/***************************************************************************//**
 * Check if sync data task is running
 ******************************************************************************/
bool sync_data_task_is_running(void)
{
    return sync_running;
}

/***************************************************************************//**
 * Initialize the sync data task
 ******************************************************************************/
sl_status_t sync_data_task_init(void)
{
    RTOS_ERR err;
    
    // Record boot tick count for uptime calculation
    boot_tick_count = sl_sleeptimer_get_tick_count();

    // Clear any legacy keys that overlapped BT-reserved space to protect bonding data.
    purge_legacy_nvm3_keys();
    
    // Load metadata from NVM3
    load_metadata_from_nvm3();
    
    // Allocate TCB
    sync_data_task_tcb_ptr = (OS_TCB *)sl_malloc(sizeof(OS_TCB));
    app_assert(sync_data_task_tcb_ptr != DEF_NULL, "Failed to allocate TCB for sync_data task");
    
    // Allocate stack
    sync_data_task_stk_ptr = (CPU_STK *)sl_malloc(SYNC_DATA_TASK_STK_SIZE * sizeof(CPU_STK));
    app_assert(sync_data_task_stk_ptr != DEF_NULL, "Failed to allocate stack for sync_data task");
    
    // Create semaphore
    OSSemCreate(&sync_data_task_sem, "sync_data_sem", 0, &err);
    app_assert(err.Code == RTOS_ERR_NONE, "Failed to create sync_data semaphore");
    
    // Create task
    OSTaskCreate(sync_data_task_tcb_ptr,
                 "sync_data_task",
                 sync_data_task,
                 DEF_NULL,
                 SYNC_DATA_TASK_PRIO,
                 sync_data_task_stk_ptr,
                 SYNC_DATA_TASK_STK_SIZE / 10,
                 SYNC_DATA_TASK_STK_SIZE,
                 0u,
                 0u,
                 DEF_NULL,
                 OS_OPT_TASK_STK_CHK | OS_OPT_TASK_STK_CLR,
                 &err);
    app_assert(err.Code == RTOS_ERR_NONE, "Failed to create sync_data task");
    
    app_log_info("Sync data task initialized\n");
    
    return SL_STATUS_OK;
}

/***************************************************************************//**
 * Sync data task - sends historical data to phone on request
 ******************************************************************************/
static void sync_data_task(void *p_arg)
{
    (void)p_arg;
    RTOS_ERR err;
    sample_t sample;
    uint16_t samples_sent = 0;
    
    for (;;) {
        // Wait for sync request
        OSSemPend(&sync_data_task_sem, 0, OS_OPT_PEND_BLOCKING, DEF_NULL, &err);
        if (err.Code != RTOS_ERR_NONE) {
            continue;
        }
        
        // Check connection
        if (connection_handle == SL_BT_INVALID_CONNECTION_HANDLE) {
            app_log_warning("Sync requested but no connection\n");
            continue;
        }

        // Notifications must be enabled by client (CCCD) before we can sync.
        if (!sync_data_id_notification_enabled) {
            app_log_warning("Sync requested but notifications are disabled (CCCD not set)\n");
            continue;
        }
        
        sync_running = true;
        samples_sent = 0;

        // Pre-count how many samples we intend to send so remaining can be accurate,
        // and to avoid logging "complete" when the connection drops mid-sync.
        const uint16_t total_to_send = count_samples_newer_than(last_sync_timestamp);
        
        app_log_info("Starting sync: looking for samples after timestamp %lu\n",
                     (unsigned long)last_sync_timestamp);
        
        // Iterate through all stored samples
        // Calculate the starting index based on sample count and write index
        uint16_t start_index;
        if (sample_count < MAX_SAMPLES) {
            start_index = 0;
        } else {
            start_index = write_index; // Oldest sample in circular buffer
        }
        
        uint8_t retry_count = 0;
        const uint8_t MAX_RETRIES = 3;
        
        bool aborted = false;
        for (uint16_t i = 0; i < sample_count; i++) {
            uint16_t idx = (start_index + i) % MAX_SAMPLES;
            
            if (read_sample_from_nvm3(idx, &sample) != SL_STATUS_OK) {
                continue;
            }
            
            // Get the sample timestamp
            // Check if this is an old-format sample (uptime with marker bit)
            uint32_t sample_timestamp = sample.timestamp;
            if (sample_timestamp & 0x80000000) {
                // This sample was saved before epoch was set
                // Skip it if we can't calculate proper timestamp
                if (!epoch_valid) {
                    app_log_warning("Skipping sample %u: no valid epoch to calculate timestamp\n", i);
                    continue;
                }
                // Calculate approximate absolute timestamp using stored uptime
                // This works if the device hasn't been rebooted since the sample was saved
                uint32_t sample_uptime = sample_timestamp & 0x7FFFFFFF;
                sample_timestamp = epoch_actual + sample_uptime;
                app_log_info("Sample %u: converted uptime %lu to timestamp %lu\n", 
                            i, (unsigned long)sample_uptime, (unsigned long)sample_timestamp);
            }
            
            // Check if sample is newer than last sync
            if (sample_timestamp > last_sync_timestamp) {
                // Send this sample with retry logic
                sl_status_t send_status;
                retry_count = 0;
                
                do {
                    send_status = send_sample_notification(&sample, sample_timestamp);
                    if (send_status == SL_STATUS_OK) {
                        samples_sent++;
                        app_log_info("Sent sample %u: ts=%lu, co2=%u\n",
                                     samples_sent, (unsigned long)sample_timestamp, sample.co2);
                        break;
                    } else if (send_status == SL_STATUS_NO_MORE_RESOURCE) {
                        // BLE buffer is full, wait and retry
                        retry_count++;
                        app_log_warning("BLE buffer full, retry %d/%d\n", retry_count, MAX_RETRIES);
                        OSTimeDlyHMSM(0, 0, 0, 500, OS_OPT_TIME_HMSM_STRICT, &err);
                    } else {
                        // Other error, stop trying
                        app_log_error("Failed to send sample (0x%04lx), stopping sync\n", 
                                     (unsigned long)send_status);
                        retry_count = MAX_RETRIES + 1; // Force exit
                    }
                } while (retry_count < MAX_RETRIES && 
                         connection_handle != SL_BT_INVALID_CONNECTION_HANDLE);
                
                if (retry_count >= MAX_RETRIES) {
                    app_log_error("Max retries exceeded, stopping sync\n");
                    aborted = true;
                    break;
                }
                
                // Increased delay between notifications to avoid overwhelming BLE stack
                // 200ms gives the BLE stack time to actually transmit the notification
                OSTimeDlyHMSM(0, 0, 0, 200, OS_OPT_TIME_HMSM_STRICT, &err);
            }
            
            // Check if connection was lost
            if (connection_handle == SL_BT_INVALID_CONNECTION_HANDLE) {
                app_log_warning("Connection lost during sync\n");
                aborted = true;
                break;
            }
        }
        
        // If we finished without abort and are still connected, signal completion to client.
        if (!aborted && connection_handle != SL_BT_INVALID_CONNECTION_HANDLE) {
            send_sync_complete_notification(samples_sent);
        }

        const uint16_t remaining = (total_to_send > samples_sent) ? (uint16_t)(total_to_send - samples_sent) : 0;
        if (aborted) {
            app_log_warning("Sync aborted: sent %u/%u samples, remaining %u\n",
                            samples_sent, total_to_send, remaining);
        } else {
            app_log_info("Sync complete: sent %u/%u samples, remaining %u\n",
                         samples_sent, total_to_send, remaining);
        }
        
        sync_running = false;
    }
}
