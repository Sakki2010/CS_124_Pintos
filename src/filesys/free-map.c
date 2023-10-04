#include "filesys/free-map.h"
#include <bitmap.h>
#include <debug.h>
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "filesys/fsdisk.h"
#include "filesys/inode.h"

/*! Initializes the free map. */
void free_map_init(void) {
    // the current implementation requires no initialization.
}

/*! Allocates CNT consecutive sectors from the free map and stores the first
    into *SECTORP.
    Returns true if successful, false if not enough consecutive sectors were
    available or if the free_map file could not be written.
    On failure, does not modify SECTORP. */
bool free_map_allocate(size_t cnt, block_sector_t *sectorp) {
    bitmap_t *free_map = fs_cache_get_free_map_buf();
    block_sector_t sector = bitmap_scan_and_flip(free_map, 0, cnt, false);
    fs_cache_release(free_map);
    if (sector != BITMAP_ERROR)
        *sectorp = sector;
    return sector != BITMAP_ERROR;
}

/*! Allocates a single sector from the free map and stores it into *SECTORP. 
    Returns true if successful, false if a sector could not be found.
    On failure, does not modify SECTORP. */
bool free_map_get(block_sector_t *sectorp) {
    bitmap_t *free_map = fs_cache_get_free_map_buf();
    block_sector_t sector = bitmap_lowest(free_map, false);
    bool found = sector != BITMAP_ERROR;
    if (found) bitmap_mark(free_map, sector);
    fs_cache_release(free_map);
    if (found) *sectorp = sector;
    return found;
}

/*! Makes CNT sectors starting at SECTOR available for use. */
void free_map_release(block_sector_t sector, size_t cnt) {
    bitmap_t *free_map = fs_cache_get_free_map_buf();
    ASSERT(bitmap_all(free_map, sector, cnt));
    bitmap_set_multiple(free_map, sector, cnt, false);
    fs_cache_release(free_map);
}

/*! Creates a free map and writes it to the disk. */
void free_map_create(void) {
    /*! Create a bitmap */
    void *free_map_buf = fs_cache_get_free_map_buf();
    bitmap_t *free_map = bitmap_create_in_buf(fs_disk_size(), free_map_buf,
                                              FREE_MAP_BUF_SIZE);
    bitmap_mark(free_map, ROOT_DIR_SECTOR);
    block_sector_t free_map_sectors = 
        DIV_ROUND_UP(bitmap_buf_size(fs_disk_size()), BLOCK_SECTOR_SIZE);
    bitmap_set_multiple(free_map, FREE_MAP_START, free_map_sectors, true);
    fs_cache_release(free_map);
}

