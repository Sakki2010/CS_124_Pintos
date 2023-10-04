#ifndef FILESYS_FSDISK_H
#define FILESYS_FSDISK_H

#include "devices/block.h"

/*! The given limit on the max size of disk we're required to handle, 8 MiB. */
#define MAX_DISK_SIZE 0x800000
/*! The size of the free bitmap required to track allocations for a max size
    disk. */
#define MAX_FREE_MAP_SIZE BITMAP_BUF_SIZE(MAX_DISK_SIZE / BLOCK_SECTOR_SIZE)
#define FREE_MAP_BUF_SIZE ROUND_UP(MAX_FREE_MAP_SIZE, BLOCK_SECTOR_SIZE)

void fs_disk_init(void);
void fs_disk_close(void);
block_sector_t fs_disk_size(void);
void fs_cache_flush(bool blocking);

void fs_disk_write(block_sector_t, const void *);
void fs_disk_read(block_sector_t, void *);

void fs_cache_write(block_sector_t, const void *);
void fs_cache_read(block_sector_t, void *);

/*! Flag to pass to _get to indicate that the buffer may be written to
    and should be locked accordingly. Since it is impossible to efficiently 
    check whether the buffer was actually written to, the entry will be
    flagged dirty. */
#define CACHE_WRITE 0x1
/*! Flag to pass to _get to indicate that the buffer will be overwritten
    entirely before it is read from and so does not need to be pre-loaded. 
    Note that this means if you call _get with this flag without then
    overwriting it, the sector may be overwritten with whatever garbage happened
    to be in the buffer once you release it.
    
    This flag implies CACHE_WRITE. */
#define CACHE_NOLOAD 0x2

void *fs_cache_get(block_sector_t, uint32_t flags);
void fs_cache_release(void *);

void *fs_cache_get_free_map_buf(void);

void fs_request_read_ahead(block_sector_t);

#endif