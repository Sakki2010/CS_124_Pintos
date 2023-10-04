#include <bitmap.h>
#include "devices/block.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "threads/palloc.h"

#include "swaptbl.h"

#define SECTORS_PER_PAGE (PGSIZE / BLOCK_SECTOR_SIZE)
#define MAX_SWAP_SIZE UINTPTR_MAX

/*! Bitmap tracking which slots of the swap table are currently occupied. 
    After swaptbl_store returns X, bit X is true, and once swaptbl_load is given
    X, bit X will be false. */
static bitmap_t *occupied;
/*! Lock to ensure the operation of finding and claiming a swap slot is atomic. */
static lock_t lock;
/*! The swap partition block device. */
static block_t *block;

/*! Initializes the swap table. Because we are allowed to panic if we run out of
    swap, this function will panic on failure. */
void swaptbl_init(void) {
    lock_init(&lock);
    block = block_get_role(BLOCK_SWAP);
    ASSERT(block != NULL);
    size_t swap_slots = block_size(block) / SECTORS_PER_PAGE;
    if (swap_slots > MAX_SWAP_SIZE) {
        swap_slots = MAX_SWAP_SIZE;
    }
    occupied = bitmap_create(swap_slots);
    ASSERT(occupied != NULL);
}

/*! Writes the given page to a free swap slot and returns the index of swap slot
    to be passed to swaptbl_load. Page passed in must be a valid address and
    should be pinned. Panics if there are no slots available.

    The top PGBITS bits are guaranteed to be 0. */
uintptr_t swaptbl_store(void *page) {
    ASSERT(pg_ofs(page) == 0);
    ASSERT(page > PHYS_BASE);
    lock_acquire(&lock);
    size_t slot = bitmap_lowest(occupied, false);
    if (slot == BITMAP_ERROR) {
        PANIC("Ran out of swap space, panic!\n");
    }
    bitmap_set(occupied, slot, true);
    for (block_sector_t sec = slot * SECTORS_PER_PAGE;
         sec < (slot + 1) * SECTORS_PER_PAGE; sec++) {
        size_t off = (sec - slot * SECTORS_PER_PAGE) * BLOCK_SECTOR_SIZE;
        block_write(block, sec, page + off);
    }
    lock_release(&lock);
    return slot;
}

/*! Reads the contents of the slot at slot into the given page and marks the
    slot as free. Panics if the given swap slot is not currently occupied.
    If page is NULL, marks the slot as free. */
void swaptbl_load(void *page, uintptr_t slot) {
    lock_acquire(&lock);
    if (page == NULL) goto exit;
    ASSERT(pg_ofs(page) == 0);
    ASSERT(page > PHYS_BASE);
    for (block_sector_t sec = slot * SECTORS_PER_PAGE;
         sec < (slot + 1) * SECTORS_PER_PAGE; sec++) {
        size_t off = (sec - slot * SECTORS_PER_PAGE) * BLOCK_SECTOR_SIZE;
        block_read(block, sec, page + off);
    }
    exit:
    ASSERT(slot < bitmap_size(occupied) && bitmap_test(occupied, slot));
    bitmap_set(occupied, slot, false);
    lock_release(&lock);
}