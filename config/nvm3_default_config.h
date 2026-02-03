#ifndef NVM3_DEFAULT_CONFIG_H
#define NVM3_DEFAULT_CONFIG_H

// <<< Use Configuration Wizard in Context Menu >>>

// <h>NVM3 Default Instance Configuration

#ifndef NVM3_DEFAULT_CACHE_SIZE
// <o NVM3_DEFAULT_CACHE_SIZE> NVM3 Default Instance Cache Size
// <i> Number of NVM3 objects to cache. To reduce access times this number
// <i> should be equal to or higher than the number of NVM3 objects in the
// <i> default NVM3 instance.
// <i> Default: 200
#define NVM3_DEFAULT_CACHE_SIZE  0
#endif

#ifndef NVM3_DEFAULT_MAX_OBJECT_SIZE
// <o NVM3_DEFAULT_MAX_OBJECT_SIZE> NVM3 Default Instance Max Object Size
// <i> Max NVM3 object size that can be stored.
// <i> Default: 254
#define NVM3_DEFAULT_MAX_OBJECT_SIZE  254
#endif

#ifndef NVM3_DEFAULT_REPACK_HEADROOM
// <o NVM3_DEFAULT_REPACK_HEADROOM> NVM3 Default Instance User Repack Headroom
// <i> Headroom determining how many bytes below the forced repack limit the user
// <i> repack limit should be placed. The default is 0, which means the user and
// <i> forced repack limits are equal.
// <i> Default: 0
#define NVM3_DEFAULT_REPACK_HEADROOM  0
#endif

#ifndef NVM3_DEFAULT_NVM_SIZE
// <o NVM3_DEFAULT_NVM_SIZE> NVM3 Default Instance Size
// <i> Size of the NVM3 storage region in flash for all storage (metadata, bonds, and samples).
// <i> Must be aligned to flash page size (8KB).
// <i> Consolidated instance (10 pages × 8KB = 80KB):
// <i>   - 6 metadata keys (write_idx, read_idx, epoch, boot_time, oldest_idx, last_synced_idx)
// <i>   - 60 batch keys (0x9000-0x913B, each ~800 bytes = ~48KB total)
// <i>   - ~32KB headroom for BLE bonding, repack operations, and growth
// <i> Previous: 0x8000 (32KB, separate default instance)
// <i> Previous: 0x12000 (72KB, separate sample instance) - NOW CONSOLIDATED
// <i> Default: 0x14000 (80KB = 10 pages)
#define NVM3_DEFAULT_NVM_SIZE  0x14000
#endif

// </h>

// <<< end of configuration section >>>

#endif // NVM3_DEFAULT_CONFIG_H
