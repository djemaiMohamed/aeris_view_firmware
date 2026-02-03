#ifndef SYNC_DATA_TASK_H
#define SYNC_DATA_TASK_H

#include <stdint.h>
#include <stdbool.h>
#include "sl_status.h"
#include "os.h"

// Task configuration
#define SYNC_DATA_TASK_PRIO       55u
#define SYNC_DATA_TASK_STK_SIZE   256u  // Increased to reduce stack overflow risk during sync

// NVM3 key definitions for sample storage (application range >= 0x8000 to avoid BT keys)
// Reserve metadata in the 0x8000 range and batch samples starting at 0x9000.
#define NVM3_META_KEY_WRITE_IDX   0x8000    // Key for write index
#define NVM3_META_KEY_READ_IDX    0x8001    // Key for read index (last sent)
#define NVM3_META_KEY_EPOCH       0x8002    // Key for epoch timestamp
#define NVM3_META_KEY_BOOT_TIME   0x8003    // Key for boot timestamp offset
#define NVM3_META_KEY_OLDEST_IDX  0x8004    // Key for oldest index in circular buffer
#define NVM3_META_KEY_LAST_SYNCED_IDX 0x8005 // Key for last synced sample index

// Batching configuration (10 samples per NVM3 object - reduced from 20 for RAM efficiency)
// Trade-off: More NVM3 writes (5 objects) for ~400 bytes less RAM
#define SAMPLES_PER_BATCH       10                  // Reduced from 20 to save RAM
#define NUM_BATCHES             300                 // Total batches (3000 / 10)
#define NVM3_SAMPLE_BATCH_KEY_BASE  0x9000         // Base key for batch objects
#define NVM3_SAMPLE_BATCH_KEY_MAX   0x912B         // Max batch key (0x9000 + 299)
#define BATCH_SIZE_BYTES        ((SAMPLES_PER_BATCH * sizeof(sample_t)) + 2) // +2 for count

// Sync framing (12-byte payload, byte 10 is frame type)
#define FRAME_DATA        0x01
#define FRAME_SYNC_START  0x02
#define FRAME_SYNC_DONE   0x03
#define FRAME_SYNC_ABORT  0x04

#define SYNC_PROTOCOL_VERSION 0x01

// Maximum number of samples to store in NVM3 (circular buffer size)
// With consolidated 80KB NVM3, batching 10 samples per object = 5 batch objects.
// Total capacity: 5 batches × 10 samples = 50 samples
#define MAX_SAMPLES               3000

// Sample data structure stored in NVM3 batches
typedef struct {
    uint32_t timestamp;       // Device uptime in seconds (not absolute - phone calculates timestamp)
    uint16_t co2;             // CO2 in ppm
    int16_t temperature_c;    // Temperature in centi-degrees Celsius (e.g., 2500 = 25.00°C)
    uint16_t humidity;        // Humidity in percent
    uint8_t battery_percent;  // Battery percentage (0-100)
} sample_t;

// Batch structure: 20 samples + count metadata
typedef struct {
    sample_t samples[SAMPLES_PER_BATCH];
    uint16_t count;  // 0 to SAMPLES_PER_BATCH
} sample_batch_t;

// Semaphore used to trigger sync
extern OS_SEM sync_data_task_sem;

/**
 * @brief Initialize the sync data task and NVM3 storage
 * @return SL_STATUS_OK on success, error code otherwise
 */
sl_status_t sync_data_task_init(void);

/**
 * @brief Flush pending samples to NVM3 (called before sync)
 * @return SL_STATUS_OK on success, error code otherwise
 */
sl_status_t sync_data_flush_batch(void);

/**
 * @brief Check if sync data task is currently sending data
 * @return true if sync is in progress, false otherwise
 */
bool sync_data_task_is_running(void);

/**
 * @brief Save a sensor sample to NVM3 storage
 * @param co2         CO2 value in ppm
 * @param temp_mdeg_c Temperature in milli-degrees Celsius
 * @param humidity    Humidity in percent
 * @param battery     Battery percentage (0-100)
 * @return SL_STATUS_OK on success, error code otherwise
 * @note Timestamp is automatically calculated from epoch + uptime
 */
sl_status_t sync_data_save_sample(uint16_t co2, 
                                   int32_t temp_mdeg_c, uint32_t humidity,
                                   uint8_t battery);

/**
 * @brief Get current device uptime in seconds
 * @return Uptime in seconds since boot (stored as sample timestamp for phone to calculate absolute time)
 */
uint32_t sync_data_get_current_timestamp(void);

/**
 * @brief Check if uptime tracking is valid (always true in simplified protocol)
 * @return true (uptime always available)
 */
bool sync_data_epoch_is_valid(void);

/**
 * @brief Handle write to sync_data_id characteristic (simplified protocol)
 * @param value  Pointer to 4-byte value (start index for sync)
 * @param length Length of value (should be 4)
 * @note Phone sends only the requested start index; timestamps calculated on phone side
 */
void handle_sync_data_write(const uint8_t *value, uint16_t length);

/**
 * @brief Handle read from sync_data_id characteristic
 * @param connection    BLE connection handle
 * @param characteristic GATT characteristic ID
 */
void handle_sync_data_read(uint16_t connection, uint16_t characteristic);

/**
 * @brief Get current device uptime in seconds
 * @return Uptime in seconds since boot
 */
uint32_t sync_data_get_uptime_sec(void);

/**
 * @brief Get the stored epoch timestamp (deprecated - not used in simplified protocol)
 * @return Current device uptime (epoch no longer tracked)
 */
uint32_t sync_data_get_epoch(void);

/**
 * @brief Get the number of samples currently stored
 * @return Number of stored samples
 */
uint16_t sync_data_get_sample_count(void);

#endif // SYNC_DATA_TASK_H
