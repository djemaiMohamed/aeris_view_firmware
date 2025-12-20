#ifndef SYNC_DATA_TASK_H
#define SYNC_DATA_TASK_H

#include <stdint.h>
#include <stdbool.h>
#include "sl_status.h"
#include "os.h"

// Task configuration
#define SYNC_DATA_TASK_PRIO       24u
#define SYNC_DATA_TASK_STK_SIZE   192u

// NVM3 key definitions for sample storage
// Using a range of keys for sample data (circular buffer)
#define NVM3_SAMPLE_KEY_BASE      0x1000    // Base key for sample storage
#define NVM3_META_KEY_WRITE_IDX   0x0F00    // Key for write index
#define NVM3_META_KEY_READ_IDX    0x0F01    // Key for read index (last sent)
#define NVM3_META_KEY_EPOCH       0x0F02    // Key for epoch timestamp
#define NVM3_META_KEY_BOOT_TIME   0x0F03    // Key for boot timestamp offset

// Maximum number of samples to store in NVM3 (circular buffer size)
// With 80KB NVM3 and ~20 bytes per sample, we can store ~3500 samples
#define MAX_SAMPLES               3500

// Sample data structure stored in NVM3
typedef struct {
    uint32_t timestamp;       // Absolute Unix timestamp (seconds since 1970)
    uint16_t co2;             // CO2 in ppm
    int16_t temperature_c;    // Temperature in centi-degrees Celsius (e.g., 2500 = 25.00°C)
    uint16_t humidity;        // Humidity in percent
    uint8_t battery_percent;  // Battery percentage (0-100)
    uint8_t reserved;         // Reserved for alignment
} sample_t;

// Semaphore used to trigger sync from GATT write
extern OS_SEM sync_data_task_sem;

/**
 * @brief Initialize the sync data task and NVM3 storage
 * @return SL_STATUS_OK on success, error code otherwise
 */
sl_status_t sync_data_task_init(void);

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
 * @brief Get current absolute timestamp (epoch + uptime)
 * @return Current Unix timestamp, or 0 if epoch not set
 */
uint32_t sync_data_get_current_timestamp(void);

/**
 * @brief Check if epoch time has been set by phone
 * @return true if epoch is valid, false otherwise
 */
bool sync_data_epoch_is_valid(void);

/**
 * @brief Handle write to sync_data_id characteristic
 * @param value  Pointer to 8-byte value (MSB 4 bytes = current epoch, LSB 4 bytes = last sync timestamp)
 * @param length Length of value (should be 8)
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
 * @brief Get the stored epoch timestamp
 * @return Epoch timestamp (seconds since 1970) or 0 if not set
 */
uint32_t sync_data_get_epoch(void);

/**
 * @brief Get the number of samples currently stored
 * @return Number of stored samples
 */
uint16_t sync_data_get_sample_count(void);

#endif // SYNC_DATA_TASK_H
