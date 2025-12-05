# Air Quality Monitor: NVM3 Data Sync Implementation

## Goal

Implement a **data persistence and sync mechanism** on the BGM22 device so that:

- Sensor measurements are saved in **NVM3** when the phone is **not connected** or a different phone connects.
- Saved measurements are kept until the phone requests them, or until flash memory is full.
- Each measurement is saved using the **measurement interval**, so no measurement is lost.
- A new **`sync_data` task** will handle sending missing data to the phone upon request.

---

## Architecture Overview

1. **Measurement Logging**
   - At startup:
     - Device stores an **absolute epoch timestamp** (1970) as `actual_timestamp`.
     - Starts taking measurements at the configured interval.
   - Each measurement is saved as:
     ```c
     struct Sample {
         uint32_t uptime_sec;   // seconds since device boot
         uint16_t co2;
         int16_t temperature_c; // always in Celsius
         int16_t humidity;
     };
     ```
     - Stored sequentially in **NVM3**.

2. **Sync Task**
   - Task Name: `sync_data`
   - Function:
     - Waits (pends) on a **semaphore**.
     - Semaphore is posted when **phone writes the `sync_data_id` characteristic**.
     - On wake-up:
       - Device reads `sync_data_id` characteristic:
         - **MSB 4 bytes** → `current_epoch_timestamp` (actual device time)  
         - **LSB 4 bytes** → `last_sync_timestamp`
       - Updates device epoch for calculations.
       - Sends all missed data points where:
         ```
         sample_timestamp = epoch_actual + uptime_sec
         sample_timestamp > last_sync_timestamp
         ```
     - After sending all missing data:
       - Clears sent entries if desired (or keeps circular buffer for historical storage).
   - If **phone does not request sync**:
     - Device continues sending **live measurements** as notifications, while saving each measurement in NVM3.

3. **Timestamp Handling**
   - Device stores **measurements using uptime**: `0s, 300s, 600s…`.
   - `epoch_actual` is updated when phone provides current absolute time via `sync_data_id` characteristic.
   - Calculated sample timestamp:
     ```
     sample_timestamp = epoch_actual + uptime_sec
     ```

4. **BLE Behavior**
   - Characteristic: `sync_data_id`
     - Writable by phone.
     - **8-byte value**:
       - **MSB (bytes 0–3)**: current absolute timestamp  
       - **LSB (bytes 4–7)**: last sync timestamp
   - On write:
     - Device updates `epoch_actual`.
     - Posts `sync_data` task semaphore.
   - Notifications:
     - Live measurement notifications continue as usual if phone does not request historical data.
     - Historical/missed measurements are sent as notifications by the `sync_data` task.

5. **NVM3 Management**
   - Store each measurement sequentially in NVM3.
   - Use a **circular buffer** to handle flash overflow:
     - Define `MAX_SAMPLES`.
     - If buffer full, overwrite oldest samples.
   - Keep **read/write indices** to track stored measurements and sent measurements.

---

## Implementation Steps

### 1. Define Sample Struct
```c
typedef struct {
    uint32_t uptime_sec;   // seconds since boot
    uint16_t co2;
    int16_t temperature_c; // always Celsius
    uint16_t humidity;
} sample_t;
