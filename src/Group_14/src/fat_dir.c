/**
 * @file fat_dir.c
 * @brief Directory operations implementation for FAT filesystem driver.
 *
 * Handles VFS operations like open, readdir, unlink, and the core path
 * resolution logic (lookup). Includes helpers for managing directory entries.
 */

 #include "fat_dir.h"    // Our declarations
 #include "fat_core.h"   // Core structures (fat_fs_t, vfs_driver_t)
 #include "fat_alloc.h"  // Cluster allocation (fat_allocate_cluster, fat_free_cluster_chain)
 #include "fs_util.h"    // fs_util_split_path
 #include "fat_utils.h"  // FAT entry access, LBA conversion, name formatting etc.
 #include "fat_lfn.h"    // LFN specific helpers (checksum, reconstruct, generate)
 #include "fat_io.h"     // read_cluster_cached, write_cluster_cached (indirectly via helpers)
 #include "buffer_cache.h" // Buffer cache access (buffer_get, buffer_release, etc.)
 #include "spinlock.h"   // Locking primitives
 #include "terminal.h"   // Logging (printk equivalent)
 #include "sys_file.h"   // O_* flags definitions
 #include "kmalloc.h"    // Kernel memory allocation
 #include "fs_errno.h"   // Filesystem error codes (FS_ERR_*)
 #include "fs_config.h"  // Filesystem limits (FS_MAX_PATH_LENGTH, MAX_FILENAME_LEN)
 #include "types.h"      // struct dirent, uint*_t etc.
 #include <string.h>     // memcpy, memcmp, memset, strlen, strchr, strrchr, strtok
 #include "assert.h"     // KERNEL_ASSERT
 
 // --- Local Definitions (Should ideally be in a system header like <dirent.h>) ---
 #ifndef DT_DIR // Guard against potential future definition
 #define DT_UNKNOWN 0
 #define DT_FIFO    1
 #define DT_CHR     2
 #define DT_DIR     4 // Directory
 #define DT_BLK     6
 #define DT_REG     8 // Regular file
 #define DT_LNK     10
 #define DT_SOCK    12
 #define DT_WHT     14
 #endif
 // --- End Local Definitions ---
 
 // --- Extern Declarations ---
 // Declaration for the global FAT VFS driver structure (defined in fat_core.c)
 extern vfs_driver_t fat_vfs_driver;
 
 // --- Static Helper Prototypes (Only for functions truly local to this file) ---
 static void fat_format_short_name_impl(const uint8_t name_8_3[11], char *out_name);

// --- Corrected Logging Macros (Define properly elsewhere!) ---
// NOTE: Using %lu for uint32_t and size_t, casting arguments to (unsigned long)
// Define KLOG_LEVEL_DEBUG or similar during build to enable debug logs
#ifdef KLOG_LEVEL_DEBUG
#define FAT_DEBUG_LOG(fmt, ...) terminal_printf("[fat_dir:DEBUG] (%s) " fmt "\n", __func__, ##__VA_ARGS__)
#else
#define FAT_DEBUG_LOG(fmt, ...) do {} while(0) // Disabled if not KLOG_LEVEL_DEBUG
#endif
// TEMPORARY FIX: Map INFO/WARN to DEBUG until properly defined globally
#define FAT_INFO_LOG(fmt, ...)  FAT_DEBUG_LOG("[INFO] " fmt, ##__VA_ARGS__) // Mapped to DEBUG
#define FAT_WARN_LOG(fmt, ...)  FAT_DEBUG_LOG("[WARN] " fmt, ##__VA_ARGS__) // Mapped to DEBUG
#define FAT_ERROR_LOG(fmt, ...) terminal_printf("[fat_dir:ERROR] (%s:%d) " fmt "\n", __func__, __LINE__, ##__VA_ARGS__)
// --- End Logging Macros --

 
 // Helper function implementation (previously static)
 static void fat_format_short_name_impl(const uint8_t name_8_3[11], char *out_name) {
     memcpy(out_name, name_8_3, 8);
     int base_len = 8;
     while (base_len > 0 && out_name[base_len - 1] == ' ') base_len--; // Trim trailing spaces from base
     out_name[base_len] = '\0';
     if (name_8_3[8] != ' ') { // If extension exists
         out_name[base_len] = '.';
         base_len++;
         memcpy(out_name + base_len, name_8_3 + 8, 3);
         int ext_len = 3;
         while(ext_len > 0 && out_name[base_len + ext_len - 1] == ' ') ext_len--; // Trim trailing spaces from ext
         out_name[base_len + ext_len] = '\0';
     }
 }
 
 
 /* --- VFS Operation Implementations --- */
 
 /**
  * @brief Opens or creates a file/directory node within the FAT filesystem.
  * @note This function handles the logic for O_CREAT and O_TRUNC flags.
  * @return Pointer to the allocated vnode on success, NULL on failure.
  */
  vnode_t *fat_open_internal(void *fs_context, const char *path, int flags)
{
    FAT_DEBUG_LOG("Enter: path='%s', flags=0x%x", path ? path : "<NULL>", flags);

    fat_fs_t *fs = (fat_fs_t *)fs_context;
    if (!fs || !path) {
        FAT_ERROR_LOG("Invalid parameters: fs=%p, path=%p", fs, path);
        return NULL;
    }

    uintptr_t irq_flags = spinlock_acquire_irqsave(&fs->lock);

    fat_dir_entry_t entry;
    char lfn_buffer[FAT_MAX_LFN_CHARS];
    uint32_t entry_dir_cluster = 0;
    uint32_t entry_offset_in_dir = 0;
    int find_res;
    bool exists = false;
    bool created = false;
    bool truncated = false;
    vnode_t *vnode = NULL;
    fat_file_context_t *file_ctx = NULL;
    int ret_err = FS_SUCCESS;

    // --- 1. Lookup the path ---
    FAT_DEBUG_LOG("Looking up path '%s'...", path);
    find_res = fat_lookup_path(fs, path, &entry, lfn_buffer, sizeof(lfn_buffer),
                               &entry_dir_cluster, &entry_offset_in_dir);
    exists = (find_res == FS_SUCCESS);

    if (exists) {
        FAT_DEBUG_LOG("Lookup success: Attr=0x%02x, Size=%lu, Cluster=%lu, DirClu=%lu, DirOff=%lu",
                      entry.attr,
                      (unsigned long)entry.file_size,
                      (unsigned long)fat_get_entry_cluster(&entry),
                      (unsigned long)entry_dir_cluster,
                      (unsigned long)entry_offset_in_dir);
    } else if (find_res == -FS_ERR_NOT_FOUND) {
        FAT_DEBUG_LOG("Lookup: Path '%s' not found.", path);
    } else {
        // Use %d for int error codes
        FAT_WARN_LOG("Lookup failed for '%s' with error: %d (%s)", path, find_res, fs_strerror(find_res)); // Now uses DEBUG log
    }

    // --- 2. Handle File Creation (O_CREAT) ---
    if (!exists && (flags & O_CREAT)) {
        FAT_INFO_LOG("Handling O_CREAT for '%s'", path); // Now uses DEBUG log
        // *** NOTE: The line below WILL STILL CAUSE A LINKER ERROR if fat_create_file is not defined/linked ***
        ret_err = fat_create_file(fs, path, FAT_ATTR_ARCHIVE, &entry, &entry_dir_cluster, &entry_offset_in_dir);
        if (ret_err == FS_SUCCESS) {
            created = true;
            exists = true;
            FAT_DEBUG_LOG("O_CREAT successful, new entry Size=%lu, Cluster=%lu",
                          (unsigned long)entry.file_size,
                          (unsigned long)fat_get_entry_cluster(&entry));
        } else {
            FAT_ERROR_LOG("fat_create_file failed for '%s', error: %d (%s)", path, ret_err, fs_strerror(ret_err));
            goto open_fail_locked;
        }
    } else if (!exists) {
        FAT_DEBUG_LOG("File not found and O_CREAT not specified for '%s'.", path);
        ret_err = -FS_ERR_NOT_FOUND;
        goto open_fail_locked;
    }

    // --- 3. File/Directory Exists: Perform Checks ---
    bool is_dir = (entry.attr & FAT_ATTR_DIRECTORY);
    if (is_dir && (flags & (O_WRONLY | O_RDWR | O_TRUNC))) {
         FAT_ERROR_LOG("Cannot open directory '%s' with write or truncate flags (0x%x).", path, flags);
         ret_err = -FS_ERR_IS_A_DIRECTORY;
         goto open_fail_locked;
    }

    // --- 4. Handle File Truncation (O_TRUNC) ---
    if (exists && !is_dir && !created && (flags & O_TRUNC)) {
        FAT_INFO_LOG("Handling O_TRUNC for '%s', original size=%lu", path, (unsigned long)entry.file_size); // Now uses DEBUG log
        if (entry.file_size > 0) {
             // *** NOTE: The line below WILL STILL CAUSE A LINKER ERROR if fat_truncate_file is not defined/linked ***
             ret_err = fat_truncate_file(fs, &entry, entry_dir_cluster, entry_offset_in_dir);
             if (ret_err != FS_SUCCESS) {
                  FAT_ERROR_LOG("fat_truncate_file failed for '%s', error: %d (%s)", path, ret_err, fs_strerror(ret_err));
                  goto open_fail_locked;
             }
             truncated = true;
             entry.file_size = 0;
             entry.first_cluster_low  = 0;
             entry.first_cluster_high = 0;
             FAT_DEBUG_LOG("O_TRUNC successful, local entry size set to 0");
        } else {
             FAT_DEBUG_LOG("O_TRUNC specified but file size is already 0 for '%s'. No action needed.", path);
             truncated = true;
        }
    }

    // --- 5. Allocation & Setup --- (Code unchanged from previous version)
    FAT_DEBUG_LOG("Allocating vnode and file context structure...");
    vnode = kmalloc(sizeof(vnode_t));
    file_ctx = kmalloc(sizeof(fat_file_context_t));
    if (!vnode || !file_ctx) {
        FAT_ERROR_LOG("kmalloc failed (vnode=%p, file_ctx=%p). Out of memory.", vnode, file_ctx);
        ret_err = -FS_ERR_OUT_OF_MEMORY;
        goto open_fail_locked;
    }
    memset(vnode, 0, sizeof(*vnode));
    memset(file_ctx, 0, sizeof(*file_ctx));
    FAT_DEBUG_LOG("Allocation successful: vnode=%p, file_ctx=%p", vnode, file_ctx);

    // --- 6. Populate context --- (Code unchanged from previous version)
    uint32_t first_cluster_final = fat_get_entry_cluster(&entry);
    FAT_DEBUG_LOG("Populating context: Assigning file_size = %lu (from entry)", (unsigned long)entry.file_size);
    file_ctx->fs                 = fs;
    file_ctx->first_cluster      = first_cluster_final;
    file_ctx->file_size          = entry.file_size;
    file_ctx->dir_entry_cluster  = entry_dir_cluster;
    file_ctx->dir_entry_offset   = entry_offset_in_dir;
    file_ctx->is_directory       = is_dir;
    file_ctx->dirty              = (created || truncated);
    file_ctx->readdir_current_cluster = file_ctx->first_cluster;
    if (fs->type != FAT_TYPE_FAT32 && file_ctx->first_cluster == 0 && is_dir) {
         file_ctx->readdir_current_cluster = 0;
    }
    file_ctx->readdir_current_offset = 0;
    file_ctx->readdir_last_index = (size_t)-1;
    FAT_DEBUG_LOG("Context populated: first_cluster=%lu, size=%lu, is_dir=%d, dirty=%d",
                  (unsigned long)file_ctx->first_cluster,
                  (unsigned long)file_ctx->file_size,
                  file_ctx->is_directory,
                  file_ctx->dirty);

    // --- 7. Link context to Vnode --- (Code unchanged from previous version)
    vnode->data     = file_ctx;
    vnode->fs_driver = &fat_vfs_driver;

    // --- Success ---
    spinlock_release_irqrestore(&fs->lock, irq_flags);
    FAT_INFO_LOG("Open successful: path='%s', vnode=%p, size=%lu", path ? path : "<NULL>", vnode, (unsigned long)file_ctx->file_size); // Now uses DEBUG log
    return vnode;

// --- Failure Path ---
open_fail_locked:
    FAT_ERROR_LOG("Open failed: path='%s', error=%d (%s)", path ? path : "<NULL>", ret_err, fs_strerror(ret_err));
    if (vnode) kfree(vnode);
    if (file_ctx) kfree(file_ctx);
    spinlock_release_irqrestore(&fs->lock, irq_flags);
    return NULL;
}


int fat_readdir_internal(file_t *dir_file, struct dirent *d_entry_out, size_t entry_index)
{
    FAT_DEBUG_LOG("Enter: dir_file=%p, d_entry_out=%p, entry_index=%lu", dir_file, d_entry_out, (unsigned long)entry_index);

    if (!dir_file || !dir_file->vnode || !dir_file->vnode->data || !d_entry_out) {
        FAT_ERROR_LOG("Invalid parameters: dir_file=%p, vnode=%p, data=%p, d_entry_out=%p",
                      dir_file, dir_file ? dir_file->vnode : NULL,
                      dir_file && dir_file->vnode ? dir_file->vnode->data : NULL, d_entry_out);
        return -FS_ERR_INVALID_PARAM;
    }

    fat_file_context_t *fctx = (fat_file_context_t*)dir_file->vnode->data;
    if (!fctx->fs || !fctx->is_directory) {
        FAT_ERROR_LOG("Context error: fs=%p, is_directory=%d. Not a valid directory context.", fctx->fs, fctx->is_directory);
        return -FS_ERR_NOT_A_DIRECTORY;
    }
    fat_fs_t *fs = fctx->fs;
    FAT_DEBUG_LOG("Context valid: fs=%p, first_cluster=%lu", fs, (unsigned long)fctx->first_cluster);

    uintptr_t irq_flags = spinlock_acquire_irqsave(&fs->lock);

    // --- State Management ---
    FAT_DEBUG_LOG("Checking readdir state: requested_idx=%lu, last_idx=%lu, current_cluster=%lu, current_offset=%lu",
                  (unsigned long)entry_index, (unsigned long)fctx->readdir_last_index,
                  (unsigned long)fctx->readdir_current_cluster, (unsigned long)fctx->readdir_current_offset);

    if (entry_index == 0 || entry_index <= fctx->readdir_last_index) {
        // ... (reset logic unchanged) ...
        if (entry_index != 0) {
             FAT_DEBUG_LOG("Implicit reset: requested index %lu <= last index %lu.", (unsigned long)entry_index, (unsigned long)fctx->readdir_last_index);
        } else {
             FAT_DEBUG_LOG("Resetting scan to start (index 0).");
        }
        fctx->readdir_current_cluster = fctx->first_cluster;
        if (fs->type != FAT_TYPE_FAT32 && fctx->first_cluster == 0) {
            FAT_DEBUG_LOG("Adjusting start cluster for FAT12/16 root directory.");
            fctx->readdir_current_cluster = 0;
        }
        fctx->readdir_current_offset = 0;
        fctx->readdir_last_index = (size_t)-1;
        FAT_DEBUG_LOG("Scan reset: start_cluster=%lu, start_offset=0, last_index=%lu",
                      (unsigned long)fctx->readdir_current_cluster, (unsigned long)fctx->readdir_last_index);

    } else if (entry_index != fctx->readdir_last_index + 1) {
        FAT_WARN_LOG("Non-sequential index requested (%lu requested, %lu expected). Seeking not implemented, failing.", // Now uses DEBUG log
                     (unsigned long)entry_index, (unsigned long)(fctx->readdir_last_index + 1));
        spinlock_release_irqrestore(&fs->lock, irq_flags);
        return -FS_ERR_INVALID_PARAM;
    }

    // --- Allocate buffer --- (Code unchanged)
     uint8_t *sector_buffer = kmalloc(fs->bytes_per_sector);
    if (!sector_buffer) {
        FAT_ERROR_LOG("Failed to allocate %u bytes for sector buffer.", fs->bytes_per_sector);
        spinlock_release_irqrestore(&fs->lock, irq_flags);
        return -FS_ERR_OUT_OF_MEMORY;
    }
    FAT_DEBUG_LOG("Allocated sector buffer at %p (%u bytes).", sector_buffer, fs->bytes_per_sector);

    // --- Init scan variables --- (Code unchanged)
    fat_lfn_entry_t lfn_collector[FAT_MAX_LFN_ENTRIES];
    int lfn_count = 0;
    size_t current_logical_index = fctx->readdir_last_index + 1;
    int ret = -FS_ERR_NOT_FOUND;

    FAT_DEBUG_LOG("Starting scan for logical index %lu (expecting index %lu). Current pos: cluster=%lu, offset=%lu",
                  (unsigned long)current_logical_index, (unsigned long)entry_index,
                  (unsigned long)fctx->readdir_current_cluster, (unsigned long)fctx->readdir_current_offset);


    // --- Directory Scanning Loop ---
    while (true) { // (Loop logic largely unchanged, only logging format adjusted)
        FAT_DEBUG_LOG("Loop iteration: target_idx=%lu, current_logical_idx=%lu, cluster=%lu, offset=%lu",
                      (unsigned long)entry_index, (unsigned long)current_logical_index,
                      (unsigned long)fctx->readdir_current_cluster, (unsigned long)fctx->readdir_current_offset);

        bool is_fat12_16_root = (fs->type != FAT_TYPE_FAT32 && fctx->first_cluster == 0);
        // ... (End of chain / End of root dir checks unchanged) ...
         if (!is_fat12_16_root && fctx->readdir_current_cluster >= fs->eoc_marker) {
            FAT_DEBUG_LOG("End of cluster chain reached (cluster %lu >= EOC %lu).",
                           (unsigned long)fctx->readdir_current_cluster, (unsigned long)fs->eoc_marker);
            ret = -FS_ERR_NOT_FOUND;
            break;
        }
        if (is_fat12_16_root && fctx->readdir_current_offset >= (unsigned long)fs->root_dir_sectors * fs->bytes_per_sector) {
            FAT_DEBUG_LOG("End of FAT12/16 root directory reached (offset %lu >= size %lu).",
                          (unsigned long)fctx->readdir_current_offset, (unsigned long)fs->root_dir_sectors * fs->bytes_per_sector);
             ret = -FS_ERR_NOT_FOUND;
             break;
        }

        // Calculate sector position
        uint32_t sec_size = fs->bytes_per_sector;
        uint32_t sector_offset_in_chain = fctx->readdir_current_offset / sec_size;
        size_t   offset_in_sector       = fctx->readdir_current_offset % sec_size;
        size_t   entries_per_sector     = sec_size / sizeof(fat_dir_entry_t);
        size_t   entry_index_in_sector  = offset_in_sector / sizeof(fat_dir_entry_t);

        FAT_DEBUG_LOG("Reading sector: chain_offset=%lu, offset_in_sec=%lu, entry_idx_in_sec=%lu",
                      (unsigned long)sector_offset_in_chain, (unsigned long)offset_in_sector, (unsigned long)entry_index_in_sector);

        // Read sector
        int read_res = read_directory_sector(fs, fctx->readdir_current_cluster,
                                             sector_offset_in_chain, sector_buffer);
        if (read_res != FS_SUCCESS) {
            FAT_ERROR_LOG("read_directory_sector failed with error %d.", read_res);
            ret = read_res;
            break;
        }
        FAT_DEBUG_LOG("Sector read successful.");

        // Iterate through entries
        for (size_t e_i = entry_index_in_sector; e_i < entries_per_sector; e_i++)
        {
             // ... (Entry processing logic unchanged, only logging formats adjusted) ...
            fat_dir_entry_t *dent = (fat_dir_entry_t*)(sector_buffer + e_i * sizeof(fat_dir_entry_t));
            FAT_DEBUG_LOG("Processing entry at sector_offset %lu: Name[0]=0x%02x, Attr=0x%02x",
                          (unsigned long)(e_i * sizeof(fat_dir_entry_t)), dent->name[0], dent->attr);

            fctx->readdir_current_offset += sizeof(fat_dir_entry_t);

            if (dent->name[0] == FAT_DIR_ENTRY_UNUSED) { /* ... */ ret = -FS_ERR_NOT_FOUND; goto readdir_done;}
            if (dent->name[0] == FAT_DIR_ENTRY_DELETED || dent->name[0] == FAT_DIR_ENTRY_KANJI) { /* ... */ lfn_count = 0; continue;}
            if ((dent->attr & FAT_ATTR_VOLUME_ID) && !(dent->attr & FAT_ATTR_LONG_NAME)) { /* ... */ lfn_count = 0; continue;}

            if ((dent->attr & FAT_ATTR_LONG_NAME_MASK) == FAT_ATTR_LONG_NAME) {
                // ... (LFN handling unchanged, just log format fixed) ...
                 fat_lfn_entry_t *lfn_ent = (fat_lfn_entry_t*)dent;
                FAT_DEBUG_LOG("Found LFN entry: Attr=0x%02x, Checksum=0x%02x", lfn_ent->attr, lfn_ent->checksum);
                if (lfn_count < FAT_MAX_LFN_ENTRIES) {
                    lfn_collector[lfn_count++] = *lfn_ent;
                    FAT_DEBUG_LOG("Stored LFN entry %d", lfn_count);
                } else {
                    FAT_WARN_LOG("LFN entry sequence exceeded buffer (%d entries). Discarding LFN.", FAT_MAX_LFN_ENTRIES); // Uses DEBUG
                    lfn_count = 0;
                }
                continue;
            }
            else {
                // --- Found an 8.3 Entry ---
                FAT_DEBUG_LOG("Found 8.3 entry: Name='%.11s', Attr=0x%02x", dent->name, dent->attr);

                if (current_logical_index == entry_index) {
                    FAT_INFO_LOG("Target logical index %lu found!", (unsigned long)entry_index); // Uses DEBUG
                    // ... (Name reconstruction unchanged) ...
                    char final_name[FAT_MAX_LFN_CHARS];
                    final_name[0] = '\0';
                    if (lfn_count > 0) { /* ... checksum check ... */
                        FAT_DEBUG_LOG("Attempting to reconstruct LFN from %d collected entries.", lfn_count);
                        uint8_t expected_sum = fat_calculate_lfn_checksum(dent->name);
                         if (lfn_collector[0].checksum == expected_sum) {
                            fat_reconstruct_lfn(lfn_collector, lfn_count, final_name, sizeof(final_name));
                            if(final_name[0] != '\0') { FAT_DEBUG_LOG("LFN reconstruction successful: '%s'", final_name); }
                            else { FAT_WARN_LOG("LFN reconstruction failed. Using 8.3 name.",0); /* Uses DEBUG */}
                         } else {
                              FAT_WARN_LOG("LFN checksum mismatch. Discarding LFN.",0); // Uses DEBUG
                              lfn_count = 0;
                         }
                    } else { FAT_DEBUG_LOG("No preceding LFN entries found."); }
                    if (final_name[0] == '\0') { /* ... format 8.3 name ... */ FAT_DEBUG_LOG("Using formatted 8.3 name: '%s'", final_name);}

                    // Populate output dirent
                    FAT_DEBUG_LOG("Populating output dirent: name='%s', cluster=%lu, attr=0x%02x",
                                  final_name, (unsigned long)fat_get_entry_cluster(dent), dent->attr);
                    // ... (strncpy, set ino, set type unchanged) ...
                     strncpy(d_entry_out->d_name, final_name, sizeof(d_entry_out->d_name) - 1);
                    d_entry_out->d_name[sizeof(d_entry_out->d_name) - 1] = '\0';
                    d_entry_out->d_ino = fat_get_entry_cluster(dent);
                    d_entry_out->d_type = (dent->attr & FAT_ATTR_DIRECTORY) ? DT_DIR : DT_REG;


                    // Update state
                    fctx->readdir_last_index = entry_index;
                    FAT_DEBUG_LOG("Updated context state: last_index=%lu, current_cluster=%lu, current_offset=%lu",
                                  (unsigned long)fctx->readdir_last_index, (unsigned long)fctx->readdir_current_cluster,
                                  (unsigned long)fctx->readdir_current_offset);
                    ret = FS_SUCCESS;
                    goto readdir_done;
                }

                // Not the target entry
                FAT_DEBUG_LOG("Logical index %lu does not match target %lu. Incrementing logical index.",
                              (unsigned long)current_logical_index, (unsigned long)entry_index);
                current_logical_index++;
                lfn_count = 0;
            }
        } // End loop through entries in sector

        // --- Move to next sector/cluster --- (Logic unchanged)
         if (!is_fat12_16_root && (fctx->readdir_current_offset % fs->cluster_size_bytes == 0) && fctx->readdir_current_offset > 0)
        {
             FAT_DEBUG_LOG("End of cluster %lu reached (offset %lu). Finding next cluster.",
                           (unsigned long)fctx->readdir_current_cluster, (unsigned long)fctx->readdir_current_offset);
            uint32_t next_c;
            int get_next_res = fat_get_next_cluster(fs, fctx->readdir_current_cluster, &next_c);
            if (get_next_res != FS_SUCCESS) { /* ... error handling ... */ ret = get_next_res; break;}
            FAT_DEBUG_LOG("Next cluster in chain is %lu.", (unsigned long)next_c);
            if (next_c >= fs->eoc_marker) { /* ... end of chain ... */ ret = -FS_ERR_NOT_FOUND; break;}
            fctx->readdir_current_cluster = next_c;
            fctx->readdir_current_offset = 0;
            FAT_DEBUG_LOG("Moved to next cluster: cluster=%lu, offset=0", (unsigned long)fctx->readdir_current_cluster);
        }

    } // End while(true) loop

// --- Cleanup and Return ---
readdir_done:
    FAT_DEBUG_LOG("Exiting: Releasing lock, freeing buffer %p, returning status %d (%s).",
                   sector_buffer, ret, fs_strerror(ret));
    kfree(sector_buffer);
    spinlock_release_irqrestore(&fs->lock, irq_flags);
    return ret;
}

 
 /**
  * @brief Deletes a file (not a directory) from the FAT filesystem.
  * @param fs_context Filesystem instance context.
  * @param path Absolute path to the file to delete.
  * @return FS_SUCCESS on success, negative FS_ERR_* code on failure.
  */
 int fat_unlink_internal(void *fs_context, const char *path)
 {
     fat_fs_t *fs = (fat_fs_t*)fs_context;
     if (!fs || !path) return -FS_ERR_INVALID_PARAM;
 
     uintptr_t irq_flags = spinlock_acquire_irqsave(&fs->lock);
     int ret = FS_SUCCESS; // Assume success initially
 
     // 1. Split path into parent directory path and final component name
     char parent_path[FS_MAX_PATH_LENGTH];
     char component_name[MAX_FILENAME_LEN + 1];
     if (fs_util_split_path(path, parent_path, sizeof(parent_path), component_name, sizeof(component_name)) != 0) {
         ret = -FS_ERR_NAMETOOLONG;
         goto unlink_fail_locked;
     }
      if (strlen(component_name) == 0 || strcmp(component_name, ".") == 0 || strcmp(component_name, "..") == 0) {
         ret = -FS_ERR_INVALID_PARAM; // Cannot unlink empty name, ".", or ".."
         goto unlink_fail_locked;
     }
 
     // 2. Lookup the parent directory
     fat_dir_entry_t parent_entry;
     uint32_t parent_entry_dir_cluster, parent_entry_offset; // Not needed here
     int parent_res = fat_lookup_path(fs, parent_path, &parent_entry, NULL, 0,
                                      &parent_entry_dir_cluster, &parent_entry_offset);
     if (parent_res != FS_SUCCESS) {
         ret = parent_res; // Parent not found or other error
         goto unlink_fail_locked;
     }
     if (!(parent_entry.attr & FAT_ATTR_DIRECTORY)) {
         ret = -FS_ERR_NOT_A_DIRECTORY; // Parent path is not a directory
         goto unlink_fail_locked;
     }
     uint32_t parent_cluster = fat_get_entry_cluster(&parent_entry);
     if (fs->type != FAT_TYPE_FAT32 && strcmp(parent_path, "/") == 0) {
          parent_cluster = 0; // Handle FAT12/16 root
     }
 
     // 3. Find the actual entry within the parent directory using the component name
     fat_dir_entry_t entry_to_delete;
     uint32_t entry_offset;            // Offset of the 8.3 entry
     uint32_t first_lfn_offset = (uint32_t)-1; // Offset of the first LFN entry (if any)
     int find_res = fat_find_in_dir(fs, parent_cluster, component_name,
                                    &entry_to_delete, NULL, 0, // Don't need LFN name buffer here
                                    &entry_offset, &first_lfn_offset);
 
     if (find_res != FS_SUCCESS) {
         ret = find_res; // File/component not found in parent directory, or I/O error
         goto unlink_fail_locked;
     }
 
     // --- Perform Checks ---
     // Check if it's a directory - cannot unlink directories with this function
     if (entry_to_delete.attr & FAT_ATTR_DIRECTORY) {
         ret = -FS_ERR_IS_A_DIRECTORY; // Use rmdir instead
         goto unlink_fail_locked;
     }
     // Check if it's read-only
     if (entry_to_delete.attr & FAT_ATTR_READ_ONLY) {
         ret = -FS_ERR_PERMISSION_DENIED;
         goto unlink_fail_locked;
     }
 
     // --- Free cluster chain ---
     uint32_t file_cluster = fat_get_entry_cluster(&entry_to_delete);
     if (file_cluster >= 2) { // Only free if file actually has clusters allocated
         int free_res = fat_free_cluster_chain(fs, file_cluster);
         if (free_res != FS_SUCCESS) {
             // DIAG_PRINTK("[FAT unlink] Warning: Error freeing cluster chain for '%s' (err %d).\n", path, free_res);
             // Non-fatal? Continue to mark entry deleted anyway, but report error?
             ret = free_res; // Report the error, but proceed with marking entry deleted
             // Alternatively: goto unlink_fail_locked; // Make cluster free failure fatal
         }
     }
 
     // --- Mark directory entries as deleted ---
     size_t num_entries_to_mark = 1; // Start with the 8.3 entry
     uint32_t mark_start_offset = entry_offset; // Offset of the 8.3 entry
 
     // If LFN entries preceded the 8.3 entry, mark them too
     if (first_lfn_offset != (uint32_t)-1 && first_lfn_offset < entry_offset) {
         num_entries_to_mark = ((entry_offset - first_lfn_offset) / sizeof(fat_dir_entry_t)) + 1;
         mark_start_offset = first_lfn_offset; // Start marking from the first LFN entry
     }
 
     int mark_res = mark_directory_entries_deleted(fs, parent_cluster,
                                                 mark_start_offset,
                                                 num_entries_to_mark,
                                                 FAT_DIR_ENTRY_DELETED); // Use 0xE5 marker
     if (mark_res != FS_SUCCESS) {
         // DIAG_PRINTK("[FAT unlink] Error marking directory entry deleted for '%s' (err %d).\n", path, mark_res);
         ret = mark_res; // Report marking error
         goto unlink_fail_locked; // Make marking failure fatal
     }
 
     // --- Flush changes ---
     buffer_cache_sync(); // Ensure deletion markers are written to disk
 
     // DIAG_PRINTK("[FAT unlink] Successfully unlinked '%s'.\n", path);
     // Fall through to return FS_SUCCESS unless an error occurred and wasn't fatal
 
 unlink_fail_locked:
     spinlock_release_irqrestore(&fs->lock, irq_flags);
     return ret; // Return final status
 }
 
 
 /* --- Internal Helper Implementations --- */
 
 /**
  * @brief Looks up a single path component (filename or dirname) within a given directory cluster.
  * @param fs Filesystem instance.
  * @param dir_cluster Starting cluster of the directory to search (0 for FAT12/16 root).
  * @param component The name component to search for.
  * @param entry_out Pointer to store the found 8.3 directory entry.
  * @param lfn_out Optional buffer to store the reconstructed LFN name.
  * @param lfn_max_len Size of the lfn_out buffer.
  * @param entry_offset_in_dir_out Pointer to store the byte offset of the found 8.3 entry within the directory cluster chain.
  * @param first_lfn_offset_out Optional pointer to store the byte offset of the first LFN entry associated with the found 8.3 entry.
  * @return FS_SUCCESS on success, negative FS_ERR_* code on failure (e.g., NOT_FOUND, IO).
  */
 int fat_find_in_dir(fat_fs_t *fs,
                    uint32_t dir_cluster,
                    const char *component,
                    fat_dir_entry_t *entry_out,
                    char *lfn_out, size_t lfn_max_len,
                    uint32_t *entry_offset_in_dir_out,
                    uint32_t *first_lfn_offset_out)
{
    KERNEL_ASSERT(fs != NULL && component != NULL && entry_out != NULL && entry_offset_in_dir_out != NULL,
                  "NULL pointer passed to fat_find_in_dir for required arguments");
    KERNEL_ASSERT(strlen(component) > 0, "Component name cannot be empty");

    // --- Added Entry Log ---
    terminal_printf("[FAT FIND DEBUG] Enter: Searching for '%s' in dir_cluster %u\n", component, (unsigned long)dir_cluster);

    uint32_t current_cluster = dir_cluster;
    bool scanning_fixed_root = (fs->type != FAT_TYPE_FAT32 && dir_cluster == 0);
    uint32_t current_byte_offset = 0; // Offset within the current directory cluster chain

    // Clear LFN output buffer if provided
    if (lfn_out && lfn_max_len > 0) lfn_out[0] = '\0';
    // Initialize optional LFN start offset output
    if (first_lfn_offset_out) *first_lfn_offset_out = (uint32_t)-1;

    // Allocate sector buffer
    uint8_t *sector_data = kmalloc(fs->bytes_per_sector);
    if (!sector_data) {
        terminal_printf("[FAT FIND DEBUG] ERROR: Failed to allocate sector buffer (%u bytes)\n", fs->bytes_per_sector);
        return -FS_ERR_OUT_OF_MEMORY;
    }
     terminal_printf("[FAT FIND DEBUG] Allocated sector buffer at %p\n", sector_data);


    fat_lfn_entry_t lfn_collector[FAT_MAX_LFN_ENTRIES]; // Buffer to collect LFN entries
    int lfn_count = 0;
    uint32_t current_lfn_start_offset = (uint32_t)-1; // Track start offset of current LFN sequence
    int ret = -FS_ERR_NOT_FOUND; // Default return if not found

    // Main loop iterating through directory sectors/clusters
    while (true) {
         terminal_printf("[FAT FIND DEBUG] Loop: current_cluster=%u, current_byte_offset=%u\n", (unsigned long)current_cluster, (unsigned long)current_byte_offset);
        // Check for end of cluster chain (only applies to non-root dirs or FAT32 root)
        if (current_cluster >= fs->eoc_marker && !scanning_fixed_root) {
             terminal_printf("[FAT FIND DEBUG] End of cluster chain reached (cluster %u >= EOC %u).\n", (unsigned long)current_cluster, (unsigned long)fs->eoc_marker);
            ret = -FS_ERR_NOT_FOUND;
            break; // End of directory
        }

        // Calculate sector offset within the chain
        uint32_t sector_offset_in_chain = current_byte_offset / fs->bytes_per_sector;
        size_t entries_per_sector = fs->bytes_per_sector / sizeof(fat_dir_entry_t);

        terminal_printf("[FAT FIND DEBUG] Reading sector: chain_offset=%u\n", (unsigned long)sector_offset_in_chain);
        // Read the directory sector
        int read_res = read_directory_sector(fs, current_cluster, sector_offset_in_chain, sector_data);
        if (read_res != FS_SUCCESS) {
            terminal_printf("[FAT FIND DEBUG] ERROR: read_directory_sector failed (err %d)\n", read_res);
            ret = read_res; // Propagate error (-FS_ERR_IO or other)
            break;          // Cannot read directory sector
        }
         terminal_printf("[FAT FIND DEBUG] Sector read success. Processing %u entries...\n", (unsigned long)entries_per_sector);


        // Iterate through entries in the loaded sector
        for (size_t e_idx = 0; e_idx < entries_per_sector; e_idx++) {
            fat_dir_entry_t *de = (fat_dir_entry_t*)(sector_data + e_idx * sizeof(fat_dir_entry_t));
            uint32_t entry_abs_offset = current_byte_offset + (uint32_t)(e_idx * sizeof(fat_dir_entry_t));

             // --- Added Log for each entry ---
             terminal_printf("[FAT FIND DEBUG] Entry %u (abs_offset %u): Name[0]=0x%02x, Attr=0x%02x\n",
                             (unsigned long)e_idx, (unsigned long)entry_abs_offset, de->name[0], de->attr);


            // Check for end-of-directory marker (0x00)
            if (de->name[0] == FAT_DIR_ENTRY_UNUSED) {
                terminal_printf("[FAT FIND DEBUG] Found UNUSED entry marker (0x00). End of directory.\n");
                ret = -FS_ERR_NOT_FOUND;
                goto find_done; // End of used entries
            }
            // Skip deleted (0xE5) and Kanji (0x05) entries
            if (de->name[0] == FAT_DIR_ENTRY_DELETED || de->name[0] == FAT_DIR_ENTRY_KANJI) {
                 terminal_printf("[FAT FIND DEBUG] Skipping DELETED (0xE5) or KANJI (0x05) entry.\n");
                lfn_count = 0; // Reset LFN state
                current_lfn_start_offset = (uint32_t)-1;
                continue;
            }
             // Skip volume label entries that are not part of an LFN sequence
             if ((de->attr & FAT_ATTR_VOLUME_ID) && !(de->attr & FAT_ATTR_LONG_NAME)) {
                 terminal_printf("[FAT FIND DEBUG] Skipping VOLUME ID entry.\n");
                lfn_count = 0;
                current_lfn_start_offset = (uint32_t)-1;
                continue;
            }

            // Process LFN entry
            if ((de->attr & FAT_ATTR_LONG_NAME_MASK) == FAT_ATTR_LONG_NAME) {
                terminal_printf("[FAT FIND DEBUG] Processing LFN entry...\n");
                if (lfn_count == 0) {
                    // Record the starting offset of this LFN sequence
                    current_lfn_start_offset = entry_abs_offset;
                    terminal_printf("[FAT FIND DEBUG]   Started new LFN sequence at offset %u.\n", (unsigned long)current_lfn_start_offset);
                }
                if (lfn_count < FAT_MAX_LFN_ENTRIES) {
                    // Collect the LFN entry
                    lfn_collector[lfn_count++] = *((fat_lfn_entry_t*)de);
                     terminal_printf("[FAT FIND DEBUG]   Collected LFN part %d.\n", lfn_count);
                } else {
                    // LFN sequence too long for buffer, discard collected entries
                    terminal_printf("[FAT FIND DEBUG]   WARNING: LFN sequence too long (>%d), discarding.\n", FAT_MAX_LFN_ENTRIES);
                    lfn_count = 0;
                    current_lfn_start_offset = (uint32_t)-1;
                }
            } else {
                // --- Process 8.3 Entry ---
                 terminal_printf("[FAT FIND DEBUG] Processing 8.3 entry: Name='%.11s'\n", de->name); // Log raw 8.3 name
                bool match = false;
                char reconstructed_lfn_buf[FAT_MAX_LFN_CHARS]; // Temp buffer for reconstructed name

                // Check if the LFN sequence collected matches the component
                if (lfn_count > 0) {
                    terminal_printf("[FAT FIND DEBUG]   Checking collected LFN (%d parts)...\n", lfn_count);
                    uint8_t expected_sum = fat_calculate_lfn_checksum(de->name);
                    terminal_printf("[FAT FIND DEBUG]   Expected checksum: 0x%02x, First LFN checksum: 0x%02x\n", expected_sum, lfn_collector[0].checksum);
                    // Verify checksum before reconstructing
                    if (lfn_collector[0].checksum == expected_sum) {
                        fat_reconstruct_lfn(lfn_collector, lfn_count, reconstructed_lfn_buf, sizeof(reconstructed_lfn_buf));
                         terminal_printf("[FAT FIND DEBUG]   LFN reconstructed: '%s'. Comparing with '%s'.\n", reconstructed_lfn_buf, component);
                        // Compare reconstructed LFN with the target component name
                        if (fat_compare_lfn(component, reconstructed_lfn_buf) == 0) {
                            match = true;
                             terminal_printf("[FAT FIND DEBUG]   LFN MATCH!\n");
                            // Copy LFN name to output buffer if requested
                            if (lfn_out && lfn_max_len > 0) {
                                strncpy(lfn_out, reconstructed_lfn_buf, lfn_max_len - 1);
                                lfn_out[lfn_max_len - 1] = '\0';
                            }
                        }
                    } else {
                        // Checksum mismatch, invalidate collected LFN entries
                         terminal_printf("[FAT FIND DEBUG]   LFN checksum mismatch! Discarding LFN.\n");
                        lfn_count = 0;
                        current_lfn_start_offset = (uint32_t)-1;
                    }
                }

                // If LFN didn't match (or wasn't present), check the 8.3 name
                if (!match) {
                     terminal_printf("[FAT FIND DEBUG]   Checking 8.3 name match against '%s'...\n", component);
                    if (fat_compare_8_3(component, de->name) == 0) {
                        match = true;
                         terminal_printf("[FAT FIND DEBUG]   8.3 MATCH!\n");
                        // Clear LFN output buffer as 8.3 name matched
                        if (lfn_out && lfn_max_len > 0) lfn_out[0] = '\0';
                        // Invalidate LFN start offset as 8.3 matched directly
                        current_lfn_start_offset = (uint32_t)-1;
                    }
                }

                // --- If either LFN or 8.3 name matched: ---
                if (match) {
                    terminal_printf("[FAT FIND DEBUG] ---> MATCH FOUND for '%s' <---\n", component);

                    // --- Added Debug Prints Around Memcpy ---
                    uint32_t raw_size_from_buffer = *(uint32_t*)((uint8_t*)de + 28); // Read bytes 28-31 from buffer
                    terminal_printf("[FAT FIND DEBUG] MATCH: Raw size from buffer (offset 28) = %u\n", (unsigned long)raw_size_from_buffer);
                    terminal_printf("[FAT FIND DEBUG] MATCH: Raw first_cluster_low = %u, high = %u\n", (unsigned int)de->first_cluster_low, (unsigned int)de->first_cluster_high);
                    terminal_printf("[FAT FIND DEBUG] MATCH: Raw attr = 0x%02x\n", de->attr);

                    memcpy(entry_out, de, sizeof(fat_dir_entry_t)); // Copy the found 8.3 entry

                    terminal_printf("[FAT FIND DEBUG] MATCH: After memcpy, entry_out->file_size = %u\n", (unsigned long)entry_out->file_size);
                    terminal_printf("[FAT FIND DEBUG] MATCH: After memcpy, entry_out->first_cluster_low = %u, high = %u\n", (unsigned int)entry_out->first_cluster_low, (unsigned int)entry_out->first_cluster_high);
                    terminal_printf("[FAT FIND DEBUG] MATCH: After memcpy, entry_out->attr = 0x%02x\n", entry_out->attr);
                    // --- End Added Debug Prints ---


                    *entry_offset_in_dir_out = entry_abs_offset;    // Record offset of 8.3 entry
                    // Record the start offset of the LFN sequence if applicable
                    if (first_lfn_offset_out) {
                        *first_lfn_offset_out = current_lfn_start_offset;
                        terminal_printf("[FAT FIND DEBUG] MATCH: Stored first LFN offset = %u\n", (unsigned long)*first_lfn_offset_out);
                    }
                    ret = FS_SUCCESS; // Set success status
                    goto find_done;   // Exit function
                }

                // If no match, reset LFN state for the next entry
                 terminal_printf("[FAT FIND DEBUG]   No match for this 8.3 entry. Resetting LFN state.\n");
                lfn_count = 0;
                current_lfn_start_offset = (uint32_t)-1;
            }
        } // End loop through entries in sector

        // Advance byte offset to the start of the next sector
        current_byte_offset += fs->bytes_per_sector;
         terminal_printf("[FAT FIND DEBUG] Advanced to next sector offset: %u\n", (unsigned long)current_byte_offset);


        // Check if we need to move to the next cluster in the chain
        if (!scanning_fixed_root && (current_byte_offset % fs->cluster_size_bytes == 0)) {
             terminal_printf("[FAT FIND DEBUG] End of cluster %u reached. Finding next...\n", (unsigned long)current_cluster);
            uint32_t next_c;
            int get_next_res = fat_get_next_cluster(fs, current_cluster, &next_c);
            if (get_next_res != FS_SUCCESS) {
                 terminal_printf("[FAT FIND DEBUG] ERROR: fat_get_next_cluster failed (err %d)\n", get_next_res);
                ret = get_next_res; // Propagate I/O error
                break;
            }
            terminal_printf("[FAT FIND DEBUG] Next cluster in chain: %u\n", (unsigned long)next_c);
            if (next_c >= fs->eoc_marker) { // End of chain
                 terminal_printf("[FAT FIND DEBUG] Next cluster is EOC marker. Ending scan.\n");
                ret = -FS_ERR_NOT_FOUND;
                break;
            }
            current_cluster = next_c;     // Move to next cluster
            current_byte_offset = 0;      // Reset offset for the new cluster
        }
        // Otherwise, the loop continues to read the next sector of the current cluster
    } // End while(true) main loop

find_done:
     terminal_printf("[FAT FIND DEBUG] Exit: Freeing buffer %p, returning status %d (%s)\n", sector_data, ret, fs_strerror(ret));
    kfree(sector_data); // Free allocated sector buffer
    return ret; // Return final status
}
 
 
 /**
  * @brief Resolves a full absolute path (e.g., "/dir/subdir/file.txt") to its final directory entry.
  * @param fs Filesystem instance.
  * @param path The absolute path string.
  * @param entry_out Pointer to store the final 8.3 directory entry found.
  * @param lfn_out Optional buffer to store the reconstructed LFN name of the final component.
  * @param lfn_max_len Size of the lfn_out buffer.
  * @param entry_dir_cluster_out Pointer to store the starting cluster of the directory *containing* the final entry.
  * @param entry_offset_in_dir_out Pointer to store the byte offset of the final 8.3 entry within its containing directory.
  * @return FS_SUCCESS on success, negative FS_ERR_* code on failure.
  */
 int fat_lookup_path(fat_fs_t *fs, const char *path,
                    fat_dir_entry_t *entry_out,
                    char *lfn_out, size_t lfn_max_len,
                    uint32_t *entry_dir_cluster_out,
                    uint32_t *entry_offset_in_dir_out)
{
    KERNEL_ASSERT(fs != NULL && path != NULL && entry_out != NULL && entry_dir_cluster_out != NULL && entry_offset_in_dir_out != NULL,
                  "NULL pointer passed to fat_lookup_path for required arguments");

    // --- REMOVED INCORRECT PATH CHECK AND FALLBACK ---
    // The VFS should provide a path relative to the mount point.
    // If the mount point is '/', the path might be "file.txt" or "dir/file.txt".
    // If the mount point is '/mnt/fat', the path might be "file.txt".
    // We should handle paths that don't start with '/' correctly.

    terminal_printf("[FAT lookup] Received path from VFS: '%s'\n", path);

    // Handle the root directory case specifically if path is empty or "/"
    // (VFS might pass "/" or "" for the root of the mounted FS)
    if (path[0] == '\0' || strcmp(path, "/") == 0) {
         terminal_printf("[FAT lookup] Handling as root directory.\n");
        memset(entry_out, 0, sizeof(*entry_out));
        entry_out->attr = FAT_ATTR_DIRECTORY;
        *entry_offset_in_dir_out = 0; // Root has no offset within a parent

        if (fs->type == FAT_TYPE_FAT32) {
            // FAT32 root directory is a regular cluster chain
            entry_out->first_cluster_low  = (uint16_t)(fs->root_cluster & 0xFFFF);
            entry_out->first_cluster_high = (uint16_t)((fs->root_cluster >> 16) & 0xFFFF);
            *entry_dir_cluster_out = 0; // Conventionally, root's "parent" is 0
        } else {
            // FAT12/16 root directory is in a fixed area, represented by cluster 0
            entry_out->first_cluster_low  = 0;
            entry_out->first_cluster_high = 0;
            *entry_dir_cluster_out = 0; // Root's "parent" is 0
        }

        // Set LFN output if requested
        if (lfn_out && lfn_max_len > 0) {
            strncpy(lfn_out, "/", lfn_max_len -1);
            lfn_out[lfn_max_len - 1] = '\0';
        }
        return FS_SUCCESS;
    }

    // --- Tokenize the path ---
    // Make a mutable copy of the path for strtok
    char *path_copy = kmalloc(strlen(path) + 1);
    if (!path_copy) return -FS_ERR_OUT_OF_MEMORY;
    strcpy(path_copy, path);

    // --- ADJUSTED TOKENIZATION START ---
    // strtok works fine whether path starts with '/' or not, as long as delimiter is '/'
    char *component = strtok(path_copy, "/");
    // If the original path started with '/', the first token will be the first directory/file.
    // If the original path was relative (e.g., "hello.elf"), the first token will be "hello.elf".

    // --- Path Traversal ---
    uint32_t current_dir_cluster; // Cluster of the directory currently being searched
    fat_dir_entry_t current_entry; // Entry found in the *previous* step (starts as root)

    // Initialize starting point based on FAT type
    if (fs->type == FAT_TYPE_FAT32) {
        current_dir_cluster = fs->root_cluster;
    } else {
        current_dir_cluster = 0; // FAT12/16 root directory cluster is 0
    }
    memset(&current_entry, 0, sizeof(current_entry)); // Initialize fake root entry
    current_entry.attr = FAT_ATTR_DIRECTORY;

    uint32_t previous_dir_cluster = 0; // Track the parent dir cluster
    int ret = -FS_ERR_NOT_FOUND; // Default if loop finishes unexpectedly

    // Loop through each path component
    while (component != NULL) {
        // Skip empty components resulting from "//" - strtok handles this automatically
        // (it won't return an empty token unless the string itself is empty or just delimiters)

        terminal_printf("[FAT lookup] Looking up component: '%s' in cluster %u\n", component, (unsigned long)current_dir_cluster);

        // Handle "." (current directory) - just continue to next component
        if (strcmp(component, ".") == 0) {
             terminal_printf("[FAT lookup] Skipping '.' component.\n");
            component = strtok(NULL, "/");
            continue;
        }
        // Handle ".." (parent directory) - Currently NOT SUPPORTED
        if (strcmp(component, "..") == 0) {
            terminal_printf("[FAT lookup] Error: '..' component not yet supported.\n");
            ret = -FS_ERR_NOT_SUPPORTED;
            goto lookup_done;
        }

        // Before searching the current_dir_cluster, store it as the potential parent
        previous_dir_cluster = current_dir_cluster;

        // Find the current component within the current directory cluster
        uint32_t component_entry_offset; // Offset of the found entry within previous_dir_cluster
        int find_comp_res = fat_find_in_dir(fs, current_dir_cluster, component,
                                             &current_entry, // Store the found entry here
                                             lfn_out, lfn_max_len, // Pass LFN buffer for the final component
                                             &component_entry_offset, NULL); // Get offset, don't need LFN start offset here

        if (find_comp_res != FS_SUCCESS) {
            terminal_printf("[FAT lookup] Component '%s' not found in cluster %u (err %d).\n", component, (unsigned long)current_dir_cluster, find_comp_res);
            ret = find_comp_res; // Component not found, or I/O error during find
            goto lookup_done;
        }
         terminal_printf("[FAT lookup] Found component '%s'. Attr=0x%02x, Size=%u, Cluster=%u\n",
                        component, current_entry.attr, (unsigned long)current_entry.file_size, (unsigned long)fat_get_entry_cluster(&current_entry));


        // Get the next path component
        char* next_component = strtok(NULL, "/");

        // Check if this was the LAST component in the path
        if (next_component == NULL) {
             terminal_printf("[FAT lookup] Component '%s' is the final component.\n", component);
            // We found the final entry!
            memcpy(entry_out, &current_entry, sizeof(*entry_out)); // Copy the final entry data
            *entry_dir_cluster_out = previous_dir_cluster;     // Store the parent directory cluster
            *entry_offset_in_dir_out = component_entry_offset; // Store the offset within the parent
            ret = FS_SUCCESS; // Report success
            goto lookup_done; // Exit
        }

        // If not the last component, it MUST be a directory to continue traversal
        if (!(current_entry.attr & FAT_ATTR_DIRECTORY)) {
            terminal_printf("[FAT lookup] Error: Component '%s' is not a directory, but path continues.\n", component);
            ret = -FS_ERR_NOT_A_DIRECTORY; // Path component is not a directory
            goto lookup_done;
        }

        // Update current_dir_cluster to the cluster of the directory we just found
        current_dir_cluster = fat_get_entry_cluster(&current_entry);
        terminal_printf("[FAT lookup] Descending into directory '%s' (cluster %lu). Next component: '%s'\n", component, (unsigned long)current_dir_cluster, next_component);


        // Handle potential issue: If current_dir_cluster is 0 (FAT12/16 root) but we are not at the root level, it's an error
        if (fs->type != FAT_TYPE_FAT32 && current_dir_cluster == 0 && previous_dir_cluster != 0) {
             // This should only happen if the directory entry is corrupted or points back to root incorrectly.
             terminal_printf("[FAT lookup] Error: Invalid traversal into FAT12/16 root (cluster 0) from non-root parent.\n");
             ret = -FS_ERR_INVALID_FORMAT; // Indicate potential FS corruption
             goto lookup_done;
         }

        // Move to the next component for the next iteration
        component = next_component;

    } // End while(component != NULL)

lookup_done:
    terminal_printf("[FAT lookup] Exit: Path='%s', returning status %d (%s)\n", path, ret, fs_strerror(ret));
    kfree(path_copy); // Free the mutable path copy
    return ret; // Return final status
}
 
 /**
  * @brief Reads a specific sector from a directory structure (root or sub-directory).
  * (This definition should NOT be static if declared in fat_dir.h)
  */
 int read_directory_sector(fat_fs_t *fs, uint32_t cluster,
                           uint32_t sector_offset_in_chain,
                           uint8_t* buffer)
 {
     KERNEL_ASSERT(fs != NULL && buffer != NULL, "FS context and buffer cannot be NULL in read_directory_sector");
     KERNEL_ASSERT(fs->bytes_per_sector > 0, "Invalid bytes_per_sector in FS context");
 
     uint32_t lba; // LBA of the target sector
     int ret = FS_SUCCESS;
 
     // --- Handle FAT12/16 Root Directory (Fixed Area) ---
     if (cluster == 0 && fs->type != FAT_TYPE_FAT32) {
         KERNEL_ASSERT(fs->root_dir_sectors > 0, "FAT12/16 root dir sector count is zero");
         // Check if requested sector is within the fixed root directory area
         if (sector_offset_in_chain >= fs->root_dir_sectors) {
             // DIAG_PRINTK("[FAT read_dir_sector] Sector offset %u out of bounds for FAT12/16 root (size %u)\n",
             //               sector_offset_in_chain, fs->root_dir_sectors);
             return -FS_ERR_NOT_FOUND; // Or FS_ERR_INVALID_PARAM? NOT_FOUND seems better.
         }
         // Calculate LBA: Root dir starts immediately after the last FAT
         lba = fs->root_dir_start_lba + sector_offset_in_chain;
         // DIAG_PRINTK("[FAT read_dir_sector] FAT12/16 Root: Reading LBA %u (offset %u)\n", lba, sector_offset_in_chain);
 
     // --- Handle Subdirectories or FAT32 Root Directory (Cluster Chain) ---
     } else if (cluster >= 2) { // Cluster 0 and 1 are reserved
         KERNEL_ASSERT(fs->sectors_per_cluster > 0, "Invalid sectors_per_cluster in FS context");
 
         uint32_t current_scan_cluster = cluster; // Start from the given directory cluster
         uint32_t sectors_per_cluster = fs->sectors_per_cluster;
         uint32_t cluster_hop_count = sector_offset_in_chain / sectors_per_cluster; // How many clusters to traverse
         uint32_t sector_in_final_cluster = sector_offset_in_chain % sectors_per_cluster; // Sector index within the target cluster
 
         // Traverse the cluster chain to find the cluster containing the desired sector
         for (uint32_t i = 0; i < cluster_hop_count; i++) {
             uint32_t next_cluster;
             ret = fat_get_next_cluster(fs, current_scan_cluster, &next_cluster);
             if (ret != FS_SUCCESS) {
                 // DIAG_PRINTK("[FAT read_dir_sector] Failed to get next cluster after %u (hop %u)\n", current_scan_cluster, i);
                 return ret; // Propagate I/O error from FAT read
             }
             if (next_cluster >= fs->eoc_marker) {
                 // DIAG_PRINTK("[FAT read_dir_sector] Reached end of chain prematurely after cluster %u (requesting offset %u)\n",
                 //               current_scan_cluster, sector_offset_in_chain);
                 return -FS_ERR_NOT_FOUND; // Requested sector is beyond the end of the chain
             }
             current_scan_cluster = next_cluster; // Move to the next cluster
         }
 
         // Calculate the LBA of the first sector of the target cluster
         uint32_t cluster_start_lba = fat_cluster_to_lba(fs, current_scan_cluster);
         if (cluster_start_lba == 0) { // fat_cluster_to_lba returns 0 on error/invalid cluster
             // DIAG_PRINTK("[FAT read_dir_sector] Invalid LBA for cluster %u\n", current_scan_cluster);
             return -FS_ERR_IO; // Treat as I/O error
         }
         // Add the offset within the cluster to get the final LBA
         lba = cluster_start_lba + sector_in_final_cluster;
         // DIAG_PRINTK("[FAT read_dir_sector] Cluster Chain: Reading LBA %u (cluster %u, sec_in_clus %u)\n",
         //               lba, current_scan_cluster, sector_in_final_cluster);
 
     } else {
         // Invalid starting cluster number (0 or 1 for non-FAT12/16 root)
         // DIAG_PRINTK("[FAT read_dir_sector] Invalid start cluster %u provided\n", cluster);
         return -FS_ERR_INVALID_PARAM;
     }
 
     // --- Read the sector using the buffer cache ---
     // DIAG_PRINTK("[FAT read_dir_sector] Accessing buffer cache for LBA %u\n", lba); // DIAGNOSTIC PRINTK
     buffer_t* b = buffer_get(fs->disk_ptr->blk_dev.device_name, lba);
     if (!b) {
         // DIAG_PRINTK("[FAT read_dir_sector] buffer_get failed for LBA %u\n", lba); // DIAGNOSTIC PRINTK
         return -FS_ERR_IO; // Buffer cache failed (likely underlying disk read error)
     }
 
     // Copy data from buffer cache to output buffer
     memcpy(buffer, b->data, fs->bytes_per_sector);
 
     // Release the buffer cache block
     buffer_release(b);
 
     // DIAG_PRINTK("[FAT read_dir_sector] Read successful for LBA %u\n", lba); // DIAGNOSTIC PRINTK
     return FS_SUCCESS;
 }
 
 /**
  * @brief Updates an existing 8.3 directory entry on disk.
  * (This definition should NOT be static if declared in fat_dir.h)
  */
 int update_directory_entry(fat_fs_t *fs,
                            uint32_t dir_cluster,
                            uint32_t dir_offset, // Byte offset of the 8.3 entry
                            const fat_dir_entry_t *new_entry)
 {
     KERNEL_ASSERT(fs != NULL && new_entry != NULL, "FS context and new entry cannot be NULL in update_directory_entry");
     KERNEL_ASSERT(fs->bytes_per_sector > 0, "Invalid bytes_per_sector");
 
     size_t sector_size = fs->bytes_per_sector;
     uint32_t sector_offset_in_chain = dir_offset / sector_size;
     size_t offset_in_sector = dir_offset % sector_size;
 
     // Ensure the entry doesn't cross a sector boundary (shouldn't happen with standard offsets)
     KERNEL_ASSERT(offset_in_sector % sizeof(fat_dir_entry_t) == 0, "Directory entry offset misaligned");
     KERNEL_ASSERT(offset_in_sector + sizeof(fat_dir_entry_t) <= sector_size,
                   "Directory entry update crosses sector boundary - Should not happen!");
 
     // --- Determine LBA of the sector containing the entry ---
     uint32_t lba;
     int ret = FS_SUCCESS;
     if (dir_cluster == 0 && fs->type != FAT_TYPE_FAT32) { // FAT12/16 Root
          if (sector_offset_in_chain >= fs->root_dir_sectors) return -FS_ERR_INVALID_PARAM;
          lba = fs->root_dir_start_lba + sector_offset_in_chain;
     } else if (dir_cluster >= 2) { // Subdirectory or FAT32 Root
          uint32_t current_cluster = dir_cluster;
          uint32_t sectors_per_cluster = fs->sectors_per_cluster;
          uint32_t cluster_hop_count = sector_offset_in_chain / sectors_per_cluster;
          uint32_t sector_in_final_cluster = sector_offset_in_chain % sectors_per_cluster;
 
          for (uint32_t i = 0; i < cluster_hop_count; i++) {
              uint32_t next_cluster;
              ret = fat_get_next_cluster(fs, current_cluster, &next_cluster);
              if (ret != FS_SUCCESS) return ret; // Propagate I/O error
              if (next_cluster >= fs->eoc_marker) return -FS_ERR_INVALID_PARAM; // Offset beyond chain end
              current_cluster = next_cluster;
          }
          uint32_t cluster_lba = fat_cluster_to_lba(fs, current_cluster);
          if (cluster_lba == 0) return -FS_ERR_IO; // Invalid cluster mapping
          lba = cluster_lba + sector_in_final_cluster;
     } else {
         return -FS_ERR_INVALID_PARAM; // Invalid dir_cluster value
     }
 
     // --- Read-Modify-Write the sector via Buffer Cache ---
     buffer_t* b = buffer_get(fs->disk_ptr->blk_dev.device_name, lba);
     if (!b) return -FS_ERR_IO; // Failed to read sector
 
     // Overwrite the specific entry within the sector buffer
     memcpy(b->data + offset_in_sector, new_entry, sizeof(fat_dir_entry_t));
 
     // Mark buffer dirty and release it (write-back handled by cache sync/policy)
     buffer_mark_dirty(b);
     buffer_release(b);
 
     return FS_SUCCESS;
 }
 
 
 /**
  * @brief Marks one or more consecutive directory entries as deleted (using 0xE5 marker).
  * (This definition should NOT be static if declared in fat_dir.h)
  */
 int mark_directory_entries_deleted(fat_fs_t *fs,
                                    uint32_t dir_cluster,
                                    uint32_t first_entry_offset, // Byte offset of the first entry to mark
                                    size_t num_entries,
                                    uint8_t marker) // Typically FAT_DIR_ENTRY_DELETED (0xE5)
 {
     KERNEL_ASSERT(fs != NULL && num_entries > 0, "FS context must be valid and num_entries > 0");
     KERNEL_ASSERT(fs->bytes_per_sector > 0, "Invalid bytes_per_sector");
 
     size_t sector_size = fs->bytes_per_sector;
     int result = FS_SUCCESS;
     size_t entries_marked = 0;
     uint32_t current_offset = first_entry_offset; // Start from the first entry's offset
 
     // Loop until all requested entries are marked
     while (entries_marked < num_entries) {
         uint32_t sector_offset_in_chain = current_offset / sector_size;
         size_t offset_in_sector = current_offset % sector_size;
         KERNEL_ASSERT(offset_in_sector % sizeof(fat_dir_entry_t) == 0, "Entry offset misaligned in mark");
 
         // --- Determine LBA of the current sector ---
         uint32_t lba;
          if (dir_cluster == 0 && fs->type != FAT_TYPE_FAT32) { // FAT12/16 Root
              if (sector_offset_in_chain >= fs->root_dir_sectors) { result = -FS_ERR_INVALID_PARAM; break; }
              lba = fs->root_dir_start_lba + sector_offset_in_chain;
          } else if (dir_cluster >= 2) { // Subdirectory or FAT32 Root
              uint32_t current_data_cluster = dir_cluster;
              uint32_t sectors_per_cluster = fs->sectors_per_cluster;
              uint32_t cluster_hop_count = sector_offset_in_chain / sectors_per_cluster;
              uint32_t sector_in_final_cluster = sector_offset_in_chain % sectors_per_cluster;
              // Traverse chain to find the correct cluster
              for (uint32_t i = 0; i < cluster_hop_count; i++) {
                  uint32_t next_cluster;
                  result = fat_get_next_cluster(fs, current_data_cluster, &next_cluster);
                  if (result != FS_SUCCESS) goto mark_fail; // Propagate IO error
                  if (next_cluster >= fs->eoc_marker) { result = -FS_ERR_INVALID_PARAM; goto mark_fail; } // Offset beyond chain end
                  current_data_cluster = next_cluster;
              }
              uint32_t cluster_lba = fat_cluster_to_lba(fs, current_data_cluster);
              if (cluster_lba == 0) { result = -FS_ERR_IO; break; } // Invalid cluster mapping
              lba = cluster_lba + sector_in_final_cluster;
          } else {
              result = -FS_ERR_INVALID_PARAM; break; // Invalid dir_cluster
          }
 
         // --- Read-Modify-Write the sector via Buffer Cache ---
         buffer_t* b = buffer_get(fs->disk_ptr->blk_dev.device_name, lba);
         if (!b) { result = -FS_ERR_IO; break; } // Failed to read sector
 
         bool buffer_dirtied = false;
         // Mark entries within the current sector
         while (entries_marked < num_entries && offset_in_sector < sector_size) {
             fat_dir_entry_t* entry_ptr = (fat_dir_entry_t*)(b->data + offset_in_sector);
             // Only modify the first byte (the deletion marker)
             entry_ptr->name[0] = marker;
             buffer_dirtied = true;
 
             // Advance to the next entry's position
             offset_in_sector += sizeof(fat_dir_entry_t);
             current_offset   += sizeof(fat_dir_entry_t);
             entries_marked++;
         }
 
         // Mark buffer dirty if modified and release
         if (buffer_dirtied) {
             buffer_mark_dirty(b);
         }
         buffer_release(b);
 
         // If an error occurred (like buffer_get fail), break the outer loop
         if (result != FS_SUCCESS) break;
 
     } // end while (entries_marked < num_entries)
 
 mark_fail: // Label for goto on inner loop errors
     return result;
 }
 
 
 /**
  * @brief Writes one or more consecutive directory entries (LFN or 8.3) to disk.
  * (This definition should NOT be static if declared in fat_dir.h)
  */
 int write_directory_entries(fat_fs_t *fs, uint32_t dir_cluster,
                             uint32_t dir_offset,
                             const void *entries_buf,
                             size_t num_entries)
 {
     KERNEL_ASSERT(fs != NULL && entries_buf != NULL, "FS context and entry buffer cannot be NULL in write_directory_entries");
     if (num_entries == 0) return FS_SUCCESS; // Nothing to write
     KERNEL_ASSERT(fs->bytes_per_sector > 0, "Invalid bytes_per_sector");
 
     size_t total_bytes = num_entries * sizeof(fat_dir_entry_t);
     size_t sector_size = fs->bytes_per_sector;
     const uint8_t *src_buf = (const uint8_t *)entries_buf; // Pointer to input data
     size_t bytes_written = 0;
     int result = FS_SUCCESS;
 
     // Loop writing data, potentially across multiple sectors
     while (bytes_written < total_bytes) {
         uint32_t current_abs_offset = dir_offset + (uint32_t)bytes_written;
         uint32_t sector_offset_in_chain = current_abs_offset / sector_size;
         size_t offset_in_sector = current_abs_offset % sector_size;
         KERNEL_ASSERT(offset_in_sector % sizeof(fat_dir_entry_t) == 0, "Write offset misaligned");
 
         // --- Determine LBA of the current sector ---
         uint32_t lba;
          if (dir_cluster == 0 && fs->type != FAT_TYPE_FAT32) { // FAT12/16 Root
              if (sector_offset_in_chain >= fs->root_dir_sectors) { result = -FS_ERR_INVALID_PARAM; break; } // Offset out of bounds
              lba = fs->root_dir_start_lba + sector_offset_in_chain;
          } else if (dir_cluster >= 2) { // Subdirectory or FAT32 Root
              uint32_t current_data_cluster = dir_cluster;
              uint32_t sectors_per_cluster = fs->sectors_per_cluster;
              uint32_t cluster_hop_count = sector_offset_in_chain / sectors_per_cluster;
              uint32_t sector_in_final_cluster = sector_offset_in_chain % sectors_per_cluster;
              // Traverse chain
              for (uint32_t i = 0; i < cluster_hop_count; i++) {
                  uint32_t next_cluster;
                  result = fat_get_next_cluster(fs, current_data_cluster, &next_cluster);
                  if (result != FS_SUCCESS) goto write_fail; // Propagate IO error
                  if (next_cluster >= fs->eoc_marker) { result = -FS_ERR_INVALID_PARAM; goto write_fail; } // Offset beyond chain end
                  current_data_cluster = next_cluster;
              }
              uint32_t cluster_lba = fat_cluster_to_lba(fs, current_data_cluster);
               if (cluster_lba == 0) { result = -FS_ERR_IO; break; } // Invalid cluster mapping
              lba = cluster_lba + sector_in_final_cluster;
          } else {
              result = -FS_ERR_INVALID_PARAM; break; // Invalid dir_cluster
          }
 
         // --- Read-Modify-Write the sector via Buffer Cache ---
         buffer_t* b = buffer_get(fs->disk_ptr->blk_dev.device_name, lba);
         if (!b) { result = -FS_ERR_IO; break; } // Failed to read sector
 
         // Calculate how many bytes to write into *this* sector buffer
         size_t bytes_to_write_this_sector = sector_size - offset_in_sector;
         size_t bytes_remaining_total = total_bytes - bytes_written;
         if (bytes_to_write_this_sector > bytes_remaining_total) {
             bytes_to_write_this_sector = bytes_remaining_total; // Don't write past the end of input buffer
         }
         KERNEL_ASSERT(bytes_to_write_this_sector > 0, "Zero bytes to write calculation error");
 
         // Copy data from input buffer to the sector buffer
         memcpy(b->data + offset_in_sector, src_buf + bytes_written, bytes_to_write_this_sector);
 
         // Mark buffer dirty and release
         buffer_mark_dirty(b);
         buffer_release(b);
 
         // Update progress
         bytes_written += bytes_to_write_this_sector;
 
     } // end while (bytes_written < total_bytes)
 
 write_fail: // Label for goto on inner loop errors
     return result;
 }
 
 
 /**
  * @brief Finds a sequence of free slots (marked 0x00 or 0xE5) in a directory.
  * If no slots are found in the existing chain and the directory is
  * extendable (sub-directory or FAT32 root), it attempts to allocate
  * and link a new cluster.
  * (This definition should NOT be static if declared in fat_dir.h)
  */
 int find_free_directory_slot(fat_fs_t *fs, uint32_t parent_dir_cluster,
                              size_t needed_slots,
                              uint32_t *out_slot_cluster,
                              uint32_t *out_slot_offset)
 {
     KERNEL_ASSERT(fs != NULL && needed_slots > 0 && out_slot_cluster != NULL && out_slot_offset != NULL,
                   "Invalid arguments passed to find_free_directory_slot");
     // Basic sanity check - unlikely to need more slots than fit in one cluster
     KERNEL_ASSERT(needed_slots <= (fs->bytes_per_sector / sizeof(fat_dir_entry_t)) * fs->sectors_per_cluster,
                   "Requesting excessively large number of contiguous slots");
     KERNEL_ASSERT(fs->bytes_per_sector > 0, "Invalid bytes_per_sector");
 
     // DIAG_PRINTK("[FAT find_free_directory_slot] Searching %u slots in dir cluster %u.\n", needed_slots, parent_dir_cluster);
 
     uint32_t current_cluster = parent_dir_cluster;
     uint32_t last_valid_cluster = parent_dir_cluster; // Track the last cluster number seen before EOC/error
     bool scanning_fixed_root = (fs->type != FAT_TYPE_FAT32 && parent_dir_cluster == 0);
     uint32_t current_byte_offset = 0;
     size_t contiguous_free_count = 0;
     uint32_t potential_start_offset = 0;
     int ret = -FS_ERR_NO_SPACE; // Default outcome: no space found
 
     // Allocate buffer for scanning sectors
     uint8_t *sector_data = kmalloc(fs->bytes_per_sector);
     if (!sector_data) return -FS_ERR_OUT_OF_MEMORY;
 
     // --- Phase 1: Scan Existing Directory Structure ---
     while (true) {
         // Check for end of cluster chain (only applies if not scanning fixed root)
         if (current_cluster >= fs->eoc_marker && !scanning_fixed_root) {
             ret = -FS_ERR_NO_SPACE; // Reached end of chain without finding enough space
             break;
         }
 
         uint32_t sector_offset_in_chain = current_byte_offset / fs->bytes_per_sector;
         size_t entries_per_sector = fs->bytes_per_sector / sizeof(fat_dir_entry_t);
 
         // Read the current directory sector
         int read_res = read_directory_sector(fs, current_cluster, sector_offset_in_chain, sector_data);
         if (read_res != FS_SUCCESS) {
             ret = read_res; // Propagate I/O error or other read failures
             goto find_slot_done; // Exit immediately on read error
         }
 
         // Scan entries within the loaded sector
         for (size_t e_idx = 0; e_idx < entries_per_sector; e_idx++) {
             fat_dir_entry_t *de = (fat_dir_entry_t*)(sector_data + e_idx * sizeof(fat_dir_entry_t));
             uint32_t entry_abs_offset = current_byte_offset + (uint32_t)(e_idx * sizeof(fat_dir_entry_t));
 
             // Check if entry is free (Unused 0x00 or Deleted 0xE5)
             if (de->name[0] == FAT_DIR_ENTRY_UNUSED || de->name[0] == FAT_DIR_ENTRY_DELETED) {
                 if (contiguous_free_count == 0) {
                     // Mark the start of a potential contiguous block
                     potential_start_offset = entry_abs_offset;
                 }
                 contiguous_free_count++;
                 // Check if we have found enough contiguous slots
                 if (contiguous_free_count >= needed_slots) {
                     *out_slot_cluster = current_cluster; // Cluster where the block starts
                     *out_slot_offset = potential_start_offset; // Byte offset where the block starts
                     ret = FS_SUCCESS;
                     goto find_slot_done; // Success! Found space.
                 }
             } else {
                 // Entry is in use, reset the contiguous count
                 contiguous_free_count = 0;
             }
 
             // Check for the absolute end-of-directory marker (0x00)
             // If we see this, there are no more used entries after this point.
             if (de->name[0] == FAT_DIR_ENTRY_UNUSED) {
                 // If we haven't found enough contiguous slots by now, we won't find them
                 // in the rest of this cluster chain segment (unless we extend).
                 ret = -FS_ERR_NO_SPACE;
                 goto find_slot_check_extend; // Proceed to check if directory can be extended
             }
         } // End loop through entries in sector
 
         // Advance byte offset to the start of the next sector
         current_byte_offset += fs->bytes_per_sector;
 
         // Check if we need to move to the next cluster
         if (!scanning_fixed_root && (current_byte_offset % fs->cluster_size_bytes == 0)) {
             last_valid_cluster = current_cluster; // Remember the cluster we just finished scanning
             uint32_t next_c;
             int get_next_res = fat_get_next_cluster(fs, current_cluster, &next_c);
             if (get_next_res != FS_SUCCESS) {
                 ret = get_next_res; // Propagate I/O error reading FAT
                 goto find_slot_done; // Exit on FAT read error
             }
             if (next_c >= fs->eoc_marker) { // Reached the end of the cluster chain
                 ret = -FS_ERR_NO_SPACE;
                 break; // Exit the main loop, proceed to check extension
             }
             current_cluster = next_c;     // Move to the next cluster
             current_byte_offset = 0;      // Reset offset for the new cluster
             contiguous_free_count = 0;    // Reset count when crossing cluster boundary (simplification)
                                           // Note: FAT spec allows entries to span clusters, but handling
                                           // finding free slots across cluster boundaries is complex.
                                           // This implementation finds slots *within* a cluster. If more
                                           // robust cross-cluster slot finding is needed, this logic
                                           // would need significant enhancement.
         }
         // Otherwise, loop continues to read the next sector of the current cluster
 
     } // End while(true) scanning loop
 
 find_slot_check_extend:
     // --- Phase 2: Attempt Directory Extension (if applicable) ---
     // This section is reached if the loop finished because:
     // 1. End of cluster chain was reached (`ret == -FS_ERR_NO_SPACE`).
     // 2. End marker (0x00) was found (`ret == -FS_ERR_NO_SPACE`).
 
     if (ret == -FS_ERR_NO_SPACE && !scanning_fixed_root) {
         // Directory is extendable (not FAT12/16 root) and no space was found.
         // DIAG_PRINTK("[FAT find_free_directory_slot] No slot found in existing chain (last cluster %u), attempting extension.\n", last_valid_cluster);
 
         terminal_printf("[FAT find_free_directory_slot] Attempting allocation, linking from cluster %lu\n", (unsigned long)last_valid_cluster); // Added Log
         uint32_t new_cluster = fat_allocate_cluster(fs, last_valid_cluster); // CORRECT CALL: Pass cluster to link from, get new cluster number back
         if (new_cluster == 0) { // Check return value for failure (0 means error/no space)
             terminal_printf("[FAT find_free_directory_slot] fat_allocate_cluster failed (returned 0).\n");
             ret = -FS_ERR_NO_SPACE; // Allocation failure means no space
             goto find_slot_done;    // Exit without extending
         }
         // DIAG_PRINTK("[FAT find_free_directory_slot] Allocated new cluster %u.\n", new_cluster);
 
         // Zero out the newly allocated cluster
         uint8_t *zero_buffer = sector_data; // Reuse the allocated sector buffer
         memset(zero_buffer, 0, fs->bytes_per_sector);
         uint32_t new_cluster_lba = fat_cluster_to_lba(fs, new_cluster);
         if (new_cluster_lba == 0) {
             // Failed to convert new cluster to LBA - should not happen if allocation succeeded
             fat_free_cluster_chain(fs, new_cluster); // Attempt to free allocated cluster
             ret = -FS_ERR_IO;
             goto find_slot_done;
         }
 
         for (uint32_t i = 0; i < fs->sectors_per_cluster; i++) {
             buffer_t *b = buffer_get(fs->disk_ptr->blk_dev.device_name, new_cluster_lba + i);
             if (!b) {
                  // DIAG_PRINTK("[FAT find_free_directory_slot] Failed to get buffer for LBA %u during zeroing.\n", new_cluster_lba + i);
                  fat_free_cluster_chain(fs, new_cluster); // Attempt to free
                  ret = -FS_ERR_IO;
                  goto find_slot_done;
             }
             // Check if buffer already contains zeros (optimization - unlikely necessary)
             // bool already_zero = true;
             // for(size_t j=0; j<fs->bytes_per_sector; ++j) if(b->data[j] != 0) { already_zero = false; break;}
             // if(!already_zero) { ... }
             memcpy(b->data, zero_buffer, fs->bytes_per_sector); // Zero the buffer content
             buffer_mark_dirty(b); // Mark for write-back
             buffer_release(b);
         }
         // DIAG_PRINTK("[FAT find_free_directory_slot] Zeroed new cluster %u.\n", new_cluster);
 
         // Link the *last* cluster of the old chain to the new cluster
         int link_res = fat_set_cluster_entry(fs, last_valid_cluster, new_cluster);
         if (link_res != FS_SUCCESS) {
              // DIAG_PRINTK("[FAT find_free_directory_slot] Failed to link last cluster %u to new cluster %u: %d\n", last_valid_cluster, new_cluster, link_res);
              fat_free_cluster_chain(fs, new_cluster); // Attempt to free
              ret = link_res; // Propagate FAT write error
              goto find_slot_done;
         }
 
         // Mark the new cluster itself as the End Of Chain (EOC)
         int eoc_res = fat_set_cluster_entry(fs, new_cluster, fs->eoc_marker);
          if (eoc_res != FS_SUCCESS) {
               // DIAG_PRINTK("[FAT find_free_directory_slot] Failed to set EOC marker for new cluster %u: %d\n", new_cluster, eoc_res);
               // Attempt rollback? Difficult. Mark filesystem potentially inconsistent?
               // We might have linked the cluster but failed to mark it EOC.
               // Try to free the allocated cluster at least.
               fat_set_cluster_entry(fs, last_valid_cluster, fs->eoc_marker); // Try to restore EOC on previous last cluster
               fat_free_cluster_chain(fs, new_cluster); // Attempt to free
               ret = eoc_res;
               goto find_slot_done;
          }
 
         // Extension successful! The first slot is at the beginning of the new cluster.
         *out_slot_cluster = new_cluster;
         *out_slot_offset = 0;
         ret = FS_SUCCESS;
         // DIAG_PRINTK("[FAT find_free_directory_slot] Successfully extended directory, using cluster %u offset 0.\n", new_cluster);
 
     } else if (ret == -FS_ERR_NO_SPACE && scanning_fixed_root) {
          // Cannot extend FAT12/16 root directory.
          // DIAG_PRINTK("[FAT find_free_directory_slot] Cannot extend FAT12/16 root directory.\n");
          ret = -FS_ERR_NO_SPACE; // Explicitly ensure correct error code
     }
 
 find_slot_done:
     kfree(sector_data); // Free the buffer used for scanning/zeroing
     return ret; // Return final status code
 }
 
 
 /**
  * @brief Checks if a directory entry with the exact raw 11-byte short name exists.
  * @note Used by fat_generate_unique_short_name. Assumes caller holds fs lock.
  */
 bool fat_raw_short_name_exists(fat_fs_t *fs, uint32_t dir_cluster, const uint8_t short_name_raw[11]) {
     KERNEL_ASSERT(fs != NULL && short_name_raw != NULL, "NULL fs or name pointer");
     // Assumes caller holds fs->lock
 
     uint32_t current_cluster = dir_cluster;
     bool scanning_fixed_root = (fs->type != FAT_TYPE_FAT32 && dir_cluster == 0);
     uint32_t current_byte_offset = 0;
     bool found = false;
     int io_error = FS_SUCCESS; // Track I/O errors during scan
 
     uint8_t *sector_data = kmalloc(fs->bytes_per_sector);
     if (!sector_data) {
         // DIAG_PRINTK("[RawExists] Failed to allocate sector buffer.\n");
         return true; // Fail safe: Assume it exists if we can't check due to OOM.
     }
 
     // Loop through directory
     while (true) {
         if (current_cluster >= fs->eoc_marker && !scanning_fixed_root) break; // End of chain
 
         uint32_t sector_offset_in_chain = current_byte_offset / fs->bytes_per_sector;
         size_t entries_per_sector = fs->bytes_per_sector / sizeof(fat_dir_entry_t);
 
         // Read sector
         int read_res = read_directory_sector(fs, current_cluster, sector_offset_in_chain, sector_data);
         if (read_res != FS_SUCCESS) {
             io_error = read_res; // Store read error
             break; // End search on I/O error
         }
 
         // Scan sector entries
         for (size_t e_idx = 0; e_idx < entries_per_sector; e_idx++) {
             fat_dir_entry_t *de = (fat_dir_entry_t*)(sector_data + e_idx * sizeof(fat_dir_entry_t));
 
             if (de->name[0] == FAT_DIR_ENTRY_UNUSED) goto check_done; // End marker found
             if (de->name[0] == FAT_DIR_ENTRY_DELETED || de->name[0] == FAT_DIR_ENTRY_KANJI) continue; // Skip deleted/kanji
             // Skip volume labels unless they are LFN parts (shouldn't match raw 8.3 anyway)
             if ((de->attr & FAT_ATTR_VOLUME_ID) && !(de->attr & FAT_ATTR_LONG_NAME)) continue;
             // Skip LFN entries themselves
             if ((de->attr & FAT_ATTR_LONG_NAME_MASK) == FAT_ATTR_LONG_NAME) continue;
 
             // Compare raw 11 bytes of the 8.3 entry
             if (memcmp(de->name, short_name_raw, 11) == 0) {
                 found = true; // Found a match!
                 goto check_done;
             }
         }
 
         // Advance to next sector/cluster
         current_byte_offset += fs->bytes_per_sector;
         if (!scanning_fixed_root && (current_byte_offset % fs->cluster_size_bytes == 0)) {
             uint32_t next_c;
             int get_next_res = fat_get_next_cluster(fs, current_cluster, &next_c);
              if (get_next_res != FS_SUCCESS) {
                  io_error = get_next_res; // Store I/O error reading FAT
                  break; // End search on I/O error
              }
             if (next_c >= fs->eoc_marker) break; // End of chain
             current_cluster = next_c;
             current_byte_offset = 0;
         }
     } // End while
 
 check_done:
     kfree(sector_data);
     if (io_error != FS_SUCCESS) {
         // DIAG_PRINTK("[RawExists] I/O error %d during check.\n", io_error);
         return true; // Fail safe: Assume exists on I/O error during check.
     }
     return found; // Return true if found, false otherwise
 }
 // REMOVED DUPLICATE DEFINITION that was here
