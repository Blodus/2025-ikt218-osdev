#pragma once
#ifndef BLOCK_DEVICE_H
#define BLOCK_DEVICE_H

#include "types.h"
#include "spinlock.h" // Include for spinlock_t

// --- Error Codes ---
#define BLOCK_ERR_OK          0
#define BLOCK_ERR_PARAMS     -1 // Invalid parameters
#define BLOCK_ERR_TIMEOUT    -2 // Operation timed out
#define BLOCK_ERR_DEV_ERR    -3 // Device reported an error (ERR bit)
#define BLOCK_ERR_DEV_FAULT  -4 // Device fault (DF bit)
#define BLOCK_ERR_NO_DEV     -5 // Device not present or failed IDENTIFY/setup
#define BLOCK_ERR_BOUNDS     -6 // LBA out of bounds
#define BLOCK_ERR_UNSUPPORTED -7 // Feature/command not supported by drive
#define BLOCK_ERR_LOCKED     -8 // Could not acquire lock
#define BLOCK_ERR_INTERNAL   -9 // Internal driver error

// --- Device Structure ---
typedef struct {
    const char *device_name;   // e.g., "hda", "hdb"
    uint16_t io_base;          // e.g., 0x1F0
    uint16_t control_base;     // e.g., 0x3F6
    bool is_slave;             // Master (false) or Slave (true)
    uint32_t sector_size;      // Usually 512
    uint64_t total_sectors;    // Use 64-bit for LBA48 compatibility
    bool initialized;
    bool lba48_supported;
    uint16_t multiple_sector_count; // Max sectors per READ/WRITE MULTIPLE cmd (0 if unsupported)
    spinlock_t *channel_lock;  // Pointer to the channel's lock (primary/secondary)
} block_device_t;

// --- Public API ---

// Initializes ATA channels (locks) - Call once at boot
void ata_channels_init(void);

// Initializes a specific block device structure (hda, hdb, etc.)
int block_device_init(const char *device, block_device_t *dev);

// Reads sectors using best available method (MULTIPLE or single PIO)
// LBA is now uint64_t
int block_device_read(block_device_t *dev, uint64_t lba, void *buffer, size_t count);

// Writes sectors using best available method (MULTIPLE or single PIO)
// LBA is now uint64_t
int block_device_write(block_device_t *dev, uint64_t lba, const void *buffer, size_t count);

#endif /* BLOCK_DEVICE_H */