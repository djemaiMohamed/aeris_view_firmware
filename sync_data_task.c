#include "sync_data_task.h"
#include "sensor_task.h"
#include "app.h"
#include "app_assert.h"
#include "app_log.h"
#include "sl_memory_manager.h"
#include "sl_bt_api.h"
#include "gatt_db.h"
#include "nvm3_default.h"
#include "nvm3.h"
#include "sl_sleeptimer.h"
#include <string.h>

// External handles
extern uint8_t connection_handle;
// Defined in app.c
extern uint8_t sync_data_id_notification_enabled;

// Local state
static bool sync_running = false;
static uint64_t boot_tick_count = 0;        // Tick count at boot for uptime calculation (64-bit to avoid 36-hour wrap)
static uint32_t last_known_seq_id_from_phone = SYNC_REQ_FULL_SYNC; // Last seq_id the phone reported (v2 protocol)

// NVM3 circular buffer indices
static uint16_t write_index = 0;            // Next position to write (also determines current batch)
static uint16_t sample_count = 0;           // Number of samples stored
static uint16_t oldest_index = 0;           // Oldest valid sample in circular buffer

// Monotonic sequence tracking (v2 protocol — never resets across reboots)
// oldest_seq_id is derived: oldest_seq_id = next_seq_id - sample_count
static uint32_t next_seq_id = 0;            // Seq ID that will be assigned to the next committed sample
static uint16_t boot_count = 0;             // Incremented on each device boot

// Batch buffering (samples buffered in RAM before NVM3 write)
static sample_batch_t current_batch = {0};

// Sync batch caching (dynamically allocated during sync to save RAM when idle)
// Only allocated during sync_data_task execution, freed after sync completes
static sample_batch_t *cached_batch_ptr = NULL;
static int16_t cached_batch_index = -1;    // -1 = no cache, otherwise cached batch number

// RTOS objects
OS_TCB *sync_data_task_tcb_ptr;
CPU_STK *sync_data_task_stk_ptr;
OS_SEM sync_data_task_sem;

static void sync_data_task(void *p_arg);
static sl_status_t read_sample_from_nvm3(uint16_t index, sample_t *sample);
static sl_status_t read_sample_from_nvm3_cached(uint16_t index, sample_t *sample);
static void invalidate_batch_cache(void);
static sl_status_t send_sync_notification(const uint8_t *data, size_t len);
static sl_status_t send_sync_notification_with_retry(const uint8_t *data, size_t len);

/***************************************************************************//**
 * Get current uptime in seconds using sleeptimer
 ******************************************************************************/
uint32_t sync_data_get_uptime_sec(void)
{
    uint64_t current_ticks = sl_sleeptimer_get_tick_count64();
    uint64_t elapsed_ticks = current_ticks - boot_tick_count;
    uint32_t freq = sl_sleeptimer_get_timer_frequency();
    return (uint32_t)(elapsed_ticks / freq);
}

/***************************************************************************//**
 * Get the stored epoch timestamp (deprecated - not used in simplified protocol)
 ******************************************************************************/
uint32_t sync_data_get_epoch(void)
{
    // Return current uptime (epoch not tracked in simplified protocol)
    return sync_data_get_uptime_sec();
}

/***************************************************************************//**
 * Flush current batch to NVM3 (called when batch is full or before sync)
 * Batch index is derived from write_index to maintain circular buffer alignment
 ******************************************************************************/
static sl_status_t flush_sample_batch_internal(void)
{
    Ecode_t status;
    
    if (current_batch.count == 0) {
        return SL_STATUS_OK;  // Nothing to flush
    }
    
    // Derive batch index from write_index (before incrementing it)
    // This ensures batch storage aligns with sample indexing
    uint16_t batch_index = (write_index / SAMPLES_PER_BATCH) % NUM_BATCHES;
    uint32_t key = NVM3_SAMPLE_BATCH_KEY_BASE + batch_index;
    
    // Write entire batch (800 bytes) as single NVM3 object
    status = nvm3_writeData(nvm3_defaultHandle, key, &current_batch, BATCH_SIZE_BYTES);
    
    if (status != ECODE_NVM3_OK) {
        app_log_error("Failed to flush batch %u key 0x%04lx: 0x%lx\n", 
                      batch_index, (unsigned long)key, (unsigned long)status);
        return SL_STATUS_FAIL;
    }
    
    // Advance write_index by batch size
    write_index = (write_index + SAMPLES_PER_BATCH) % MAX_SAMPLES;
    
    app_log_info("Batch flushed: batch_idx=%u key=0x%04lx count=%u samples (write_idx now=%u)\n",
                 batch_index, (unsigned long)key, current_batch.count, write_index);
    
    // Reset batch count for next samples (don't memset samples, only count matters)
    current_batch.count = 0;
    
    return SL_STATUS_OK;
}

/***************************************************************************//**
 * Public API to flush batch (called before sync)
 ******************************************************************************/
sl_status_t sync_data_flush_batch(void)
{
    return flush_sample_batch_internal();
}

/***************************************************************************//**
 * Get the number of samples currently stored
 ******************************************************************************/
uint16_t sync_data_get_sample_count(void)
{
    return sample_count;
}

uint16_t sync_data_get_boot_count(void)
{
    return boot_count;
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

    if (write_index >= MAX_SAMPLES) {
        app_log_warning("NVM3 write_index out of range (%u), clamping to %u\n",
                        write_index, MAX_SAMPLES - 1);
        write_index %= MAX_SAMPLES;
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

    if (sample_count > MAX_SAMPLES) {
        app_log_warning("NVM3 sample_count out of range (%u), clamping to %u\n",
                        sample_count, MAX_SAMPLES);
        sample_count = MAX_SAMPLES;
    }
    
    // Note: epoch tracking removed in simplified protocol
    // Timestamps are now uptime values, phone calculates absolute timestamps
    
    // Load oldest index
    status = nvm3_getObjectInfo(nvm3_defaultHandle, NVM3_META_KEY_OLDEST_IDX, &type, &len);
    if (status == ECODE_NVM3_OK && len == sizeof(oldest_index)) {
        status = nvm3_readData(nvm3_defaultHandle, NVM3_META_KEY_OLDEST_IDX, &oldest_index, sizeof(oldest_index));
        if (status != ECODE_NVM3_OK) {
            oldest_index = 0;
        }
    } else {
        oldest_index = 0;
    }

    if (oldest_index >= MAX_SAMPLES) {
        app_log_warning("NVM3 oldest_index out of range (%u), clamping to %u\n",
                        oldest_index, MAX_SAMPLES - 1);
        oldest_index %= MAX_SAMPLES;
    }
    
    // Load next_seq_id (monotonic, never resets)
    status = nvm3_getObjectInfo(nvm3_defaultHandle, NVM3_META_KEY_NEXT_SEQ_ID, &type, &len);
    if (status == ECODE_NVM3_OK && len == sizeof(next_seq_id)) {
        status = nvm3_readData(nvm3_defaultHandle, NVM3_META_KEY_NEXT_SEQ_ID, &next_seq_id, sizeof(next_seq_id));
        if (status != ECODE_NVM3_OK) {
            next_seq_id = 0;
        }
    } else {
        next_seq_id = 0;
    }

    // Load boot count and increment it to mark this new boot
    status = nvm3_getObjectInfo(nvm3_defaultHandle, NVM3_META_KEY_BOOT_COUNT, &type, &len);
    if (status == ECODE_NVM3_OK && len == sizeof(boot_count)) {
        status = nvm3_readData(nvm3_defaultHandle, NVM3_META_KEY_BOOT_COUNT, &boot_count, sizeof(boot_count));
        if (status != ECODE_NVM3_OK) {
            boot_count = 0;
        }
    } else {
        boot_count = 0;
    }
    boot_count++;  // New boot — increment generation counter immediately

    // Write boot_count back right away so it survives a crash before the first batch flush
    nvm3_writeData(nvm3_defaultHandle, NVM3_META_KEY_BOOT_COUNT, &boot_count, sizeof(boot_count));

    app_log_info("NVM3 metadata loaded: write_idx=%u, sample_count=%u, oldest_idx=%u, next_seq=%lu, boot=%u\n",
                 write_index, sample_count, oldest_index, (unsigned long)next_seq_id, boot_count);

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
    
    status = nvm3_writeData(nvm3_defaultHandle, NVM3_META_KEY_OLDEST_IDX, &oldest_index, sizeof(oldest_index));
    if (status != ECODE_NVM3_OK) {
        app_log_error("Failed to save oldest_index to NVM3: 0x%lx\n", (unsigned long)status);
        return SL_STATUS_FAIL;
    }

    status = nvm3_writeData(nvm3_defaultHandle, NVM3_META_KEY_NEXT_SEQ_ID, &next_seq_id, sizeof(next_seq_id));
    if (status != ECODE_NVM3_OK) {
        app_log_error("Failed to save next_seq_id to NVM3: 0x%lx\n", (unsigned long)status);
        return SL_STATUS_FAIL;
    }

    status = nvm3_writeData(nvm3_defaultHandle, NVM3_META_KEY_BOOT_COUNT, &boot_count, sizeof(boot_count));
    if (status != ECODE_NVM3_OK) {
        app_log_error("Failed to save boot_count to NVM3: 0x%lx\n", (unsigned long)status);
        return SL_STATUS_FAIL;
    }

    return SL_STATUS_OK;
}

/***************************************************************************//**
 * Get current timestamp (uptime in seconds)
 * Phone will calculate absolute timestamps using device uptime
 ******************************************************************************/
uint32_t sync_data_get_current_timestamp(void)
{
    // Simply return device uptime - phone handles timestamp calculation
    return sync_data_get_uptime_sec();
}

/***************************************************************************//**
 * Check if epoch time has been set by phone (deprecated - no longer used)
 ******************************************************************************/
bool sync_data_epoch_is_valid(void)
{
    // Always return true - no epoch synchronization needed in simplified protocol
    return true;
}

/***************************************************************************//**
 * Save a sensor sample to NVM3 storage (batched)
 ******************************************************************************/
sl_status_t sync_data_save_sample(uint16_t co2, 
                                   int32_t temp_mdeg_c, uint32_t humidity,
                                   uint8_t battery)
{
    sample_t sample;
    
    // Get current device uptime (phone will calculate absolute timestamp)
    uint32_t current_uptime = sync_data_get_uptime_sec();
    
    // Fill sample structure with uptime and sensor data
    sample.timestamp = current_uptime;
    sample.co2 = co2;
    // Convert from milli-degrees to deci-degrees (divide by 10) to match notification task format
    sample.temperature_c = (int16_t)(temp_mdeg_c / 10);
    sample.humidity = (uint16_t)humidity;
    sample.battery_percent = battery;
    
    // Buffer in current batch (RAM only, no NVM3 write yet)
    if (current_batch.count < SAMPLES_PER_BATCH) {
        current_batch.samples[current_batch.count] = sample;
        current_batch.count++;
        
        // Flush batch when full (20 samples)
        if (current_batch.count >= SAMPLES_PER_BATCH) {
            sl_status_t flush_status = flush_sample_batch_internal();
            if (flush_status != SL_STATUS_OK) {
                return flush_status;
            }
            
            // Update sample count (write_index already updated in flush_sample_batch_internal)
            next_seq_id += SAMPLES_PER_BATCH;  // Advance monotonic counter for committed samples
            if (sample_count < MAX_SAMPLES) {
                sample_count += SAMPLES_PER_BATCH;
                if (sample_count > MAX_SAMPLES) {
                    sample_count = MAX_SAMPLES;
                }
            } else {
                oldest_index = (oldest_index + SAMPLES_PER_BATCH) % MAX_SAMPLES;
            }
            
            // Save metadata only on batch flush (98% fewer writes)
            sl_status_t meta_status = save_metadata_to_nvm3();
            if (meta_status != SL_STATUS_OK) {
                return meta_status;
            }
            
            // Repack when NVM3 reports fragmentation
            if (nvm3_repackNeeded(nvm3_defaultHandle)) {
                Ecode_t rep_status = nvm3_repack(nvm3_defaultHandle);
                if (rep_status != ECODE_NVM3_OK) {
                    app_log_warning("NVM3 repack skipped: 0x%lx\n", (unsigned long)rep_status);
                } else {
                    app_log_info("NVM3 repack completed\n");
                }
            }
            
            app_log_info("Batch flushed: %u samples now stored (write_idx=%u)\n",
                         sample_count, write_index);
        }
        
        return SL_STATUS_OK;
    }
    
    return SL_STATUS_FAIL;
}

/***************************************************************************//**
 * Read a sample from NVM3 by index (reads entire batch)
 ******************************************************************************/
static sl_status_t read_sample_from_nvm3(uint16_t index, sample_t *sample)
{
    if (index >= MAX_SAMPLES || !sample) {
        return SL_STATUS_INVALID_PARAMETER;
    }
    
    Ecode_t status;
    uint32_t type;
    size_t len;
    
    // Calculate which batch and offset within batch
    uint16_t batch_index = index / SAMPLES_PER_BATCH;  // 0-149
    uint16_t sample_offset = index % SAMPLES_PER_BATCH;  // 0-19
    
    if (batch_index >= NUM_BATCHES) {
        return SL_STATUS_INVALID_PARAMETER;
    }
    
    // Read entire 800-byte batch
    uint32_t key = NVM3_SAMPLE_BATCH_KEY_BASE + batch_index;
    status = nvm3_getObjectInfo(nvm3_defaultHandle, key, &type, &len);
    
    if (status != ECODE_NVM3_OK) {
        return SL_STATUS_NOT_FOUND;
    }
    
    // Read batch from NVM3
    sample_batch_t batch;
    status = nvm3_readData(nvm3_defaultHandle, key, &batch, BATCH_SIZE_BYTES);
    
    if (status != ECODE_NVM3_OK) {
        app_log_error("Failed to read batch %u from NVM3 key 0x%04lx: 0x%lx\n",
                      batch_index, (unsigned long)key, (unsigned long)status);
        return SL_STATUS_FAIL;
    }
    
    // Extract specific sample from batch
    if (sample_offset >= batch.count) {
        app_log_error("Sample offset %u out of range for batch %u (count=%u)\n",
                      sample_offset, batch_index, batch.count);
        return SL_STATUS_INVALID_PARAMETER;
    }
    
    *sample = batch.samples[sample_offset];
    return SL_STATUS_OK;
}

/***************************************************************************//**
 * Invalidate batch cache and free memory (called after sync completes)
 ******************************************************************************/
static void invalidate_batch_cache(void)
{
    if (cached_batch_ptr != NULL) {
        sl_free(cached_batch_ptr);
        cached_batch_ptr = NULL;
    }
    cached_batch_index = -1;
}

/***************************************************************************//**
 * Read a sample from NVM3 with batch caching (optimized for sync)
 * Caches the entire batch in RAM to avoid re-reading from NVM3 for
 * consecutive samples within the same batch. Cache is dynamically allocated.
 ******************************************************************************/
static sl_status_t read_sample_from_nvm3_cached(uint16_t index, sample_t *sample)
{
    if (index >= MAX_SAMPLES || !sample) {
        return SL_STATUS_INVALID_PARAMETER;
    }
    
    Ecode_t status;
    uint32_t type;
    size_t len;
    
    // Calculate which batch and offset within batch
    uint16_t batch_index = index / SAMPLES_PER_BATCH;
    uint16_t sample_offset = index % SAMPLES_PER_BATCH;
    
    if (batch_index >= NUM_BATCHES) {
        return SL_STATUS_INVALID_PARAMETER;
    }
    
    // Check if batch is already cached
    if (cached_batch_index != (int16_t)batch_index) {
        // Need to load batch from NVM3
        // Allocate cache if not already done
        if (cached_batch_ptr == NULL) {
            cached_batch_ptr = (sample_batch_t *)sl_malloc(sizeof(sample_batch_t));
            if (cached_batch_ptr == NULL) {
                app_log_error("Failed to allocate cached_batch buffer\n");
                return SL_STATUS_ALLOCATION_FAILED;
            }
        }
        
        uint32_t key = NVM3_SAMPLE_BATCH_KEY_BASE + batch_index;
        status = nvm3_getObjectInfo(nvm3_defaultHandle, key, &type, &len);
        
        if (status != ECODE_NVM3_OK) {
            return SL_STATUS_NOT_FOUND;
        }
        
        // Read batch from NVM3
        status = nvm3_readData(nvm3_defaultHandle, key, cached_batch_ptr, BATCH_SIZE_BYTES);
        
        if (status != ECODE_NVM3_OK) {
            app_log_error("Failed to read batch %u from NVM3 key 0x%04lx: 0x%lx\n",
                          batch_index, (unsigned long)key, (unsigned long)status);
            cached_batch_index = -1;
            return SL_STATUS_FAIL;
        }
        
        cached_batch_index = (int16_t)batch_index;
        app_log_debug("Loaded batch %u into cache (key=0x%04lx count=%u)\n",
                      batch_index, (unsigned long)key, cached_batch_ptr->count);
    }
    
    // Extract specific sample from cached batch
    if (sample_offset >= cached_batch_ptr->count) {
        app_log_error("Sample offset %u out of range for batch %u (count=%u)\n",
                      sample_offset, batch_index, cached_batch_ptr->count);
        return SL_STATUS_INVALID_PARAMETER;
    }
    
    *sample = cached_batch_ptr->samples[sample_offset];
    return SL_STATUS_OK;
}

// Send a notification (fire-and-forget with pacing)
static sl_status_t send_sync_notification(const uint8_t *data, size_t len)
{
    sl_status_t sc;
    RTOS_ERR pacing_err;

    sc = sl_bt_gatt_server_send_notification(
        connection_handle,
        gattdb_sync_data_id,
        len,
        data);

    return sc;
}

// Retry wrapper to cope with transient send errors during sync
static sl_status_t send_sync_notification_with_retry(const uint8_t *data, size_t len)
{
    sl_status_t sc = SL_STATUS_FAIL;
    RTOS_ERR err;

    for (uint8_t attempt = 0; attempt < 4; attempt++) {
        sc = send_sync_notification(data, len);
        if (sc == SL_STATUS_OK) {
            app_log_info("Notification sent (len=%u) on attempt %u\n", (unsigned)len, (unsigned)attempt + 1u);
            return sc;
        }
        app_log_warning("Notification send failed (status=0x%04lx) attempt %u/4\n",
                        (unsigned long)sc, (unsigned)attempt + 1u);

        if (sc == SL_STATUS_NO_MORE_RESOURCE) {
            OSTimeDlyHMSM(0u, 0u, 1u, 0u, OS_OPT_TIME_HMSM_STRICT, &err);
            if (err.Code != RTOS_ERR_NONE) {
                app_log_error("Notification backoff delay failed: %d\n", (int)err.Code);
                break;
            }
        }
    }

    app_log_error("Notification send failed after 3 retries (status=0x%04lx)\n",
                  (unsigned long)sc);
    return sc;
}

// ---------------------------------------------------------------------------
// v2 Protocol frame senders (all frames are SYNC_FRAME_SIZE = 20 bytes)
// Frame type byte is always at byte 14.
//
// DATA frame layout (20 bytes):
//   [0-3]  seq_id          — monotonic global sequence ID for this sample
//   [4-7]  uptime_sec      — device uptime when sample was recorded
//   [8-9]  co2_ppm
//   [10-11] temperature_centi_c (signed, 2500 = 25.00 °C)
//   [12-13] humidity_pct
//   [14]   FRAME_DATA (0x01)
//   [15]   battery_pct
//   [16-17] boot_count     — device boot generation when sample was saved
//   [18-19] reserved
//
// SYNC_START frame layout (20 bytes):
//   [0-3]  device_uptime_sec
//   [4-7]  oldest_seq_id   — seq_id of the oldest sample currently in the buffer
//   [8-9]  total_samples
//   [10-11] oldest_index   — buffer index of oldest sample (debug info)
//   [12-13] boot_count
//   [14]   FRAME_SYNC_START (0x02)
//   [15-19] reserved
//
// SYNC_DONE frame layout (20 bytes):
//   [0-1]  samples_sent
//   [2-5]  last_seq_id_sent — highest seq_id included in this session
//   [6-13] reserved
//   [14]   FRAME_SYNC_DONE (0x03)
//   [15-19] reserved
//
// SYNC_ABORT frame layout (20 bytes):
//   [0-1]  samples_sent
//   [2-5]  last_seq_id_sent
//   [6-12] reserved
//   [13]   abort_reason
//   [14]   FRAME_SYNC_ABORT (0x04)
//   [15-19] reserved
// ---------------------------------------------------------------------------

static sl_status_t send_data_frame(const sample_t *sample, uint32_t seq_id)
{
    uint8_t data[SYNC_FRAME_SIZE];
    memset(data, 0, sizeof(data));

    data[0]  = (uint8_t)(seq_id & 0xFF);
    data[1]  = (uint8_t)((seq_id >> 8) & 0xFF);
    data[2]  = (uint8_t)((seq_id >> 16) & 0xFF);
    data[3]  = (uint8_t)((seq_id >> 24) & 0xFF);
    data[4]  = (uint8_t)(sample->timestamp & 0xFF);
    data[5]  = (uint8_t)((sample->timestamp >> 8) & 0xFF);
    data[6]  = (uint8_t)((sample->timestamp >> 16) & 0xFF);
    data[7]  = (uint8_t)((sample->timestamp >> 24) & 0xFF);
    data[8]  = (uint8_t)(sample->co2 & 0xFF);
    data[9]  = (uint8_t)((sample->co2 >> 8) & 0xFF);
    data[10] = (uint8_t)(sample->temperature_c & 0xFF);
    data[11] = (uint8_t)((sample->temperature_c >> 8) & 0xFF);
    data[12] = (uint8_t)(sample->humidity & 0xFF);
    data[13] = (uint8_t)((sample->humidity >> 8) & 0xFF);
    data[14] = FRAME_DATA;
    data[15] = sample->battery_percent;
    data[16] = (uint8_t)(boot_count & 0xFF);
    data[17] = (uint8_t)((boot_count >> 8) & 0xFF);

    sl_status_t sc = send_sync_notification_with_retry(data, sizeof(data));
    if (sc != SL_STATUS_OK) {
        app_log_error("Failed to send data frame (seq=%lu): 0x%04lx\n",
                      (unsigned long)seq_id, (unsigned long)sc);
    }
    return sc;
}

static sl_status_t send_sync_start_frame(uint32_t device_uptime, uint32_t oldest_seq_id,
                                          uint16_t total_samples, uint16_t oldest_idx)
{
    uint8_t data[SYNC_FRAME_SIZE];
    memset(data, 0, sizeof(data));

    data[0]  = (uint8_t)(device_uptime & 0xFF);
    data[1]  = (uint8_t)((device_uptime >> 8) & 0xFF);
    data[2]  = (uint8_t)((device_uptime >> 16) & 0xFF);
    data[3]  = (uint8_t)((device_uptime >> 24) & 0xFF);
    data[4]  = (uint8_t)(oldest_seq_id & 0xFF);
    data[5]  = (uint8_t)((oldest_seq_id >> 8) & 0xFF);
    data[6]  = (uint8_t)((oldest_seq_id >> 16) & 0xFF);
    data[7]  = (uint8_t)((oldest_seq_id >> 24) & 0xFF);
    data[8]  = (uint8_t)(total_samples & 0xFF);
    data[9]  = (uint8_t)((total_samples >> 8) & 0xFF);
    data[10] = (uint8_t)(oldest_idx & 0xFF);
    data[11] = (uint8_t)((oldest_idx >> 8) & 0xFF);
    data[12] = (uint8_t)(boot_count & 0xFF);
    data[13] = (uint8_t)((boot_count >> 8) & 0xFF);
    data[14] = FRAME_SYNC_START;

    sl_status_t sc = send_sync_notification_with_retry(data, sizeof(data));
    if (sc == SL_STATUS_OK) {
        app_log_info("SYNC_START sent: uptime=%lu oldest_seq=%lu total=%u oldest_idx=%u boot=%u\n",
                     (unsigned long)device_uptime, (unsigned long)oldest_seq_id,
                     total_samples, oldest_idx, boot_count);
    }
    return sc;
}

static sl_status_t send_sync_done_frame(uint16_t samples_sent, uint32_t last_seq_id_sent)
{
    uint8_t data[SYNC_FRAME_SIZE];
    memset(data, 0, sizeof(data));

    data[0]  = (uint8_t)(samples_sent & 0xFF);
    data[1]  = (uint8_t)((samples_sent >> 8) & 0xFF);
    data[2]  = (uint8_t)(last_seq_id_sent & 0xFF);
    data[3]  = (uint8_t)((last_seq_id_sent >> 8) & 0xFF);
    data[4]  = (uint8_t)((last_seq_id_sent >> 16) & 0xFF);
    data[5]  = (uint8_t)((last_seq_id_sent >> 24) & 0xFF);
    data[14] = FRAME_SYNC_DONE;

    sl_status_t sc = send_sync_notification_with_retry(data, sizeof(data));
    if (sc == SL_STATUS_OK) {
        app_log_info("SYNC_DONE sent: samples=%u last_seq=%lu\n",
                     samples_sent, (unsigned long)last_seq_id_sent);
    }
    return sc;
}

static sl_status_t send_sync_abort_frame(uint16_t samples_sent, uint32_t last_seq_id_sent, uint8_t reason)
{
    uint8_t data[SYNC_FRAME_SIZE];
    memset(data, 0, sizeof(data));

    data[0]  = (uint8_t)(samples_sent & 0xFF);
    data[1]  = (uint8_t)((samples_sent >> 8) & 0xFF);
    data[2]  = (uint8_t)(last_seq_id_sent & 0xFF);
    data[3]  = (uint8_t)((last_seq_id_sent >> 8) & 0xFF);
    data[4]  = (uint8_t)((last_seq_id_sent >> 16) & 0xFF);
    data[5]  = (uint8_t)((last_seq_id_sent >> 24) & 0xFF);
    data[13] = reason;
    data[14] = FRAME_SYNC_ABORT;

    sl_status_t sc = send_sync_notification_with_retry(data, sizeof(data));
    if (sc == SL_STATUS_OK) {
        app_log_warning("SYNC_ABORT sent: samples=%u last_seq=%lu reason=%u\n",
                        samples_sent, (unsigned long)last_seq_id_sent, reason);
    }
    return sc;
}

/***************************************************************************//**
 * Handle write to sync_data_id characteristic (simplified protocol)
 * Phone sends only the requested start index (4 bytes, little-endian)
 ******************************************************************************/
void handle_sync_data_write(const uint8_t *value, uint16_t length)
{
    RTOS_ERR err;

    if (!value || length != SYNC_REQUEST_SIZE) {
        app_log_error("Sync data write invalid: length=%u (expected %u)\n",
                      length, (unsigned)SYNC_REQUEST_SIZE);
        return;
    }

    // Parse last_known_seq_id from bytes 0-3 (little-endian UInt32).
    // SYNC_REQ_FULL_SYNC (0xFFFFFFFF) means "phone has no data — send all from oldest".
    last_known_seq_id_from_phone = (uint32_t)value[0]        |
                                   ((uint32_t)value[1] << 8)  |
                                   ((uint32_t)value[2] << 16) |
                                   ((uint32_t)value[3] << 24);
    // bytes 4-7 are reserved; ignored.

    app_log_info("Sync data write: last_known_seq_id=%lu\n",
                 (unsigned long)last_known_seq_id_from_phone);

    OSSemPost(&sync_data_task_sem, OS_OPT_POST_1, &err);
    if (err.Code != RTOS_ERR_NONE) {
        app_log_error("Failed to post sync semaphore: %d\n", (int)err.Code);
    }
}

/***************************************************************************//**
 * Handle read from sync_data_id characteristic
 * Returns: total samples available (2 bytes) + device uptime (4 bytes) + padding (2 bytes)
 ******************************************************************************/
void handle_sync_data_read(uint16_t connection, uint16_t characteristic)
{
    uint8_t data[8];
    sl_status_t sc;
    
    // Return current state: sample_count (2 bytes) + current uptime (4 bytes) + padding (2 bytes)
    data[0] = (uint8_t)(sample_count & 0xFF);
    data[1] = (uint8_t)((sample_count >> 8) & 0xFF);
    
    uint32_t current_uptime = sync_data_get_uptime_sec();
    data[2] = (uint8_t)(current_uptime & 0xFF);
    data[3] = (uint8_t)((current_uptime >> 8) & 0xFF);
    data[4] = (uint8_t)((current_uptime >> 16) & 0xFF);
    data[5] = (uint8_t)((current_uptime >> 24) & 0xFF);
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
        app_log_info("Sync data read: count=%u, uptime=%lu, running=%d\n",
                     sample_count, (unsigned long)current_uptime, sync_running);
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
    
    // Record boot tick count for uptime calculation (64-bit to survive the ~36-hour uint32 wrap)
    boot_tick_count = sl_sleeptimer_get_tick_count64();
    
    // Initialize batch buffer (RAM buffering for 10 samples)
    current_batch.count = 0;
    // write_index will track which batch to flush (no separate batch_object_index)
    // Don't memset samples array - only count matters, saves CPU cycles
    
    // Cached batch will be allocated dynamically during sync (saves ~800 bytes when idle)
    cached_batch_ptr = NULL;
    cached_batch_index = -1;
    
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
    
    app_log_info("Sync data task initialized (batching: %u samples per batch, %u batches, stack: %u bytes, cached_batch: dynamic allocation)\n",
                 SAMPLES_PER_BATCH, NUM_BATCHES, SYNC_DATA_TASK_STK_SIZE * sizeof(CPU_STK));
    
    return SL_STATUS_OK;
}

/***************************************************************************//**
 * Sync data task — v2 protocol (seq_id-based, 20-byte frames)
 *
 * Resume algorithm:
 *   oldest_seq_id = next_seq_id - sample_count
 *   resume_seq    = last_known_seq_id_from_phone + 1  (wraps for 0xFFFFFFFF → 0)
 *
 *   if resume_seq <= oldest_seq_id  → buffer advanced past phone; restart from oldest
 *   if resume_seq >= next_seq_id    → phone is up to date; nothing to send
 *   else                            → start_idx = oldest_index + (resume_seq - oldest_seq_id)
 *
 * The phone uses the seq_id in each DATA frame for idempotent deduplication.
 * No last_synced_index or last_sent_valid flag needed.
 ******************************************************************************/
static void sync_data_task(void *p_arg)
{
    (void)p_arg;
    RTOS_ERR err;
    sample_t sample;
    uint16_t samples_sent = 0;

    for (;;) {
        OSSemPend(&sync_data_task_sem, 0, OS_OPT_PEND_BLOCKING, DEF_NULL, &err);
        if (err.Code != RTOS_ERR_NONE) {
            continue;
        }

        if (connection_handle == SL_BT_INVALID_CONNECTION_HANDLE) {
            app_log_warning("Sync requested but no connection\n");
            continue;
        }

        if (!sync_data_id_notification_enabled) {
            app_log_warning("Sync requested but CCCD not set\n");
            continue;
        }

        // ---- Flush any RAM-buffered samples to NVM3 before reading ----
        if (current_batch.count > 0) {
            uint16_t pending = current_batch.count;
            if (flush_sample_batch_internal() != SL_STATUS_OK) {
                app_log_error("Pre-sync batch flush failed\n");
                continue;
            }
            next_seq_id += pending;
            if (sample_count < MAX_SAMPLES) {
                sample_count += pending;
                if (sample_count > MAX_SAMPLES) sample_count = MAX_SAMPLES;
            } else {
                oldest_index = (oldest_index + pending) % MAX_SAMPLES;
            }
            save_metadata_to_nvm3();
            app_log_info("Pre-sync flush: %u samples committed\n", pending);
        }

        app_suspend_sensor_task();
        sync_running = true;
        samples_sent = 0;

        // ---- Compute seq_id window ----
        uint32_t oldest_seq_id = next_seq_id - (uint32_t)sample_count;
        uint32_t resume_seq    = last_known_seq_id_from_phone + 1u; // wraps for 0xFFFFFFFF → 0

        uint16_t start_idx;
        uint32_t remaining;

        if (sample_count == 0u) {
            remaining = 0u;
            start_idx = oldest_index;
        } else if (resume_seq <= oldest_seq_id) {
            // Buffer has advanced past phone's last known seq (or full sync requested).
            app_log_info("Sync: resume_seq=%lu <= oldest_seq=%lu; restarting from oldest\n",
                         (unsigned long)resume_seq, (unsigned long)oldest_seq_id);
            start_idx = oldest_index;
            remaining = (uint32_t)sample_count;
        } else if (resume_seq >= next_seq_id) {
            // Phone already has all samples.
            app_log_info("Sync: phone up to date (resume_seq=%lu >= next_seq=%lu)\n",
                         (unsigned long)resume_seq, (unsigned long)next_seq_id);
            start_idx = oldest_index;
            remaining = 0u;
        } else {
            uint32_t dist = resume_seq - oldest_seq_id;
            start_idx = (uint16_t)((oldest_index + dist) % MAX_SAMPLES);
            remaining  = next_seq_id - resume_seq;
            app_log_info("Sync: resuming from seq=%lu (buf_idx=%u, %lu samples to send)\n",
                         (unsigned long)resume_seq, start_idx, (unsigned long)remaining);
        }

        // ---- Send SYNC_START ----
        uint32_t current_uptime = sync_data_get_uptime_sec();
        sl_status_t send_status = send_sync_start_frame(current_uptime, oldest_seq_id,
                                                         (uint16_t)sample_count, oldest_index);
        if (send_status != SL_STATUS_OK) {
            app_log_error("Failed to send SYNC_START (0x%04lx)\n", (unsigned long)send_status);
            sync_running = false;
            app_resume_sensor_task();
            continue;
        }

        bool aborted = false;
        uint32_t last_seq_id_sent = (oldest_seq_id > 0u) ? (oldest_seq_id - 1u) : 0u;
        uint16_t idx = start_idx;

        invalidate_batch_cache();

        // ---- Stream DATA frames ----
        while (remaining > 0u && !aborted) {
            if (read_sample_from_nvm3_cached(idx, &sample) != SL_STATUS_OK) {
                app_log_error("Failed to read sample at idx=%u; skipping\n", idx);
                idx = (idx + 1u) % MAX_SAMPLES;
                remaining--;
                continue;
            }

            // seq_id of the sample at buffer index idx
            uint32_t dist_from_oldest = (uint32_t)((idx + MAX_SAMPLES - oldest_index) % MAX_SAMPLES);
            uint32_t sample_seq_id    = oldest_seq_id + dist_from_oldest;

            send_status = send_data_frame(&sample, sample_seq_id);
            if (send_status != SL_STATUS_OK) {
                app_log_error("Data frame failed (seq=%lu idx=%u): 0x%04lx\n",
                              (unsigned long)sample_seq_id, idx, (unsigned long)send_status);
                aborted = true;
                break;
            }

            // 5 ms yield so the BLE stack can dequeue the notification
            OSTimeDlyHMSM(0u, 0u, 0u, 5u, OS_OPT_TIME_HMSM_STRICT, &err);
            if (err.Code != RTOS_ERR_NONE) {
                app_log_error("Pacing delay failed: %d\n", (int)err.Code);
            }

            app_log_info("DATA seq=%lu idx=%u co2=%u t=%d h=%u bat=%u (%u sent)\n",
                         (unsigned long)sample_seq_id, idx,
                         sample.co2, sample.temperature_c, sample.humidity,
                         sample.battery_percent, samples_sent + 1u);

            last_seq_id_sent = sample_seq_id;
            samples_sent++;

            if (connection_handle == SL_BT_INVALID_CONNECTION_HANDLE) {
                app_log_warning("Connection lost during sync\n");
                aborted = true;
                break;
            }

            idx = (idx + 1u) % MAX_SAMPLES;
            remaining--;
        }

        invalidate_batch_cache();

        // ---- Send termination frame ----
        if (!aborted && connection_handle != SL_BT_INVALID_CONNECTION_HANDLE) {
            send_sync_done_frame(samples_sent, last_seq_id_sent);
            app_log_info("Sync DONE: %u samples, last_seq=%lu\n",
                         samples_sent, (unsigned long)last_seq_id_sent);
        } else if (connection_handle != SL_BT_INVALID_CONNECTION_HANDLE) {
            send_sync_abort_frame(samples_sent, last_seq_id_sent, 1u);
            app_log_warning("Sync ABORTED after %u samples (last_seq=%lu)\n",
                            samples_sent, (unsigned long)last_seq_id_sent);
        }

        sync_running = false;
        app_resume_sensor_task();
    }
}
