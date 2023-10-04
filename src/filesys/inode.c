#include "filesys/inode.h"
#include <list.h>
#include <debug.h>
#include <round.h>
#include <string.h>
#include "filesys/filesys.h"
#include "filesys/free-map.h"
#include "filesys/fsdisk.h"
#include "threads/malloc.h"
#include "threads/interrupt.h"

#include "stdio.h"

/*! Atomically adds the word B to the word at pointer A, returns the new value.
    This is written such that, for example, if two people add 1 to a value
    which was initialized to 0, exactly one of them will see 1 and exactly one
    will see 2. 
    Thus, atomic_add(&x, 1) is an atomic version of ++x.
    */
static int32_t atomic_add(int32_t *a, int32_t b) {
    int32_t c;
    enum intr_level old_level = intr_disable();
    c = *a = (*a + b);
    intr_set_level(old_level);
    return c;
}

/*! Identifies an inode. */
#define INODE_MAGIC 0x494e4f44

/*! The file system is limited to 8 megabytes, so we can use a 16 bit int rather
    than a 32 bit one to store sector indices. */
typedef uint16_t fs_sector_t;

/*! The value used in an inode to show that a given offset currently has no
    sector. */
#define NO_SECTOR ((fs_sector_t) -1)

/*! A metadata header at the front of an inode. */
typedef struct inode_header {
    off_t length;                       /*!< File size in bytes. */
    unsigned magic;                     /*!< Magic number. */
    int32_t counter;                    /*!< Counter for external use. */
} inode_header_t;

/*! With sectors of 512 bytes and 2 bytes per sector index, we can store 256
    sectors per indirect sectors. An 8 MiB file requires 2^23/2^9 = 2^14 = 16384
    sectors, which means we need at most 16384/256 = 64 indirect nodes to store
    a file in our file system. */
#define NUM_INDIRECT 64

/*! In order to make small files faster and lighter weight, we store as many
    of the sectors directly as we can. */
#define NUM_DIRECT ((BLOCK_SECTOR_SIZE - sizeof(inode_header_t))\
                    / sizeof(fs_sector_t) - NUM_INDIRECT)

/*! On-disk inode, representing an entry in the file system (file or directory).
    Contains the information needed to read or write to the entry.

    Direct and indirect nodes sector entries are used to translate logical
    offsets to the sectors which contain their data.
    Must be exactly BLOCK_SECTOR_SIZE bytes long. */
typedef struct inode_disk {
    inode_header_t header;              /*! The metadata header. */
    fs_sector_t direct[NUM_DIRECT];     /*! The directly-indexed sectors. */
    fs_sector_t indirect[NUM_INDIRECT]; /*! Indirect nodes. */
} inode_disk_t;

/*! The number of children in an indirect inode. */
#define INDIRECT_NUM_DIRECT (BLOCK_SECTOR_SIZE / sizeof(fs_sector_t))

/*! A node used to translate logical offsets to the sectors that convert their
    information. A single indirect node represents INDIRECT_NUM_DIRECT
    contiguous sectors of logical offsets, which is a total of
    BLOCK_SECTOR_SIZE^2 / sizeof(fs_sector_t) bytes. */
typedef struct indirect_node {
    fs_sector_t direct[INDIRECT_NUM_DIRECT];    /*! Directly index sectors. */
} indirect_node_t;

/*! Returns the number of sectors to allocate for an inode SIZE
    bytes long. */
static inline size_t bytes_to_sectors(off_t size) {
    return DIV_ROUND_UP(size, BLOCK_SECTOR_SIZE);
}

/*! An in-memory struct to store some additional information about an inode.
    There is guaranteed to be at most one inode with a given value of sector
    open at any given time, and further references to the same sector use the
    same object and increase open_cnt. */
struct inode {
    list_elem_t elem;                   /*!< Element in inode list. */
    block_sector_t sector;              /*!< Sector number of disk location. */
    int open_cnt;                       /*!< Number of openers. */
    bool removed;                       /*!< True if deleted, false otherwise. */
    int deny_write_cnt;                 /*!< 0: writes ok, >0: deny writes. */
    rwlock_t lock;                      /*!< Advisory lock for the whole file. */
};

/*! Returns the block device sector that contains byte offset POS
    within INODE. If CREATE is true, attempts to allocate a sector if one does
    not exist.
    Returns -1 if INODE does not contain data for a byte at offset POS and
    either CREATE is false or allocation fails because the disk is full. */
static block_sector_t byte_to_sector(inode_t *inode, off_t pos, bool create) {
    ASSERT(inode != NULL);
    inode_disk_t *data = fs_cache_get(inode->sector, CACHE_WRITE * create);
    block_sector_t ret;
    size_t sec_off = pos / BLOCK_SECTOR_SIZE;
    bool allocated = false;
    if (sec_off < NUM_DIRECT) {
        if (create && data->direct[sec_off] == NO_SECTOR) {
            block_sector_t sec;
            if (free_map_get(&sec)) {
                ASSERT(sec < fs_disk_size());
                data->direct[sec_off] = sec;
                allocated = true;
            }
        }
        if (data->direct[sec_off] != NO_SECTOR) {
            ret = data->direct[sec_off];
        } else {
            ret = -1;
        }
        fs_cache_release(data);
    } else {
        size_t subnode_i = (sec_off - NUM_DIRECT) / INDIRECT_NUM_DIRECT;
        size_t subnode_j = (sec_off - NUM_DIRECT) % INDIRECT_NUM_DIRECT;
        indirect_node_t *subnode = NULL;
        if (data->indirect[subnode_i] == NO_SECTOR && create) {
            block_sector_t sec;
            if (free_map_get(&sec)) {
                ASSERT(sec < fs_disk_size());
                data->indirect[subnode_i] = sec;
                fs_cache_release(data);
                subnode = fs_cache_get(sec, CACHE_NOLOAD);
                for (size_t i = 0; i < INDIRECT_NUM_DIRECT; i++) {
                    subnode->direct[i] = NO_SECTOR;
                }
            }
        } else if (data->indirect[subnode_i] != NO_SECTOR) {
            ASSERT(data->indirect[subnode_i] < fs_disk_size());
            fs_cache_release(data);
            subnode = fs_cache_get(data->indirect[subnode_i],
                                   CACHE_WRITE * create);
        } else {
            fs_cache_release(data);
        }
        if (subnode != NULL) {
            if (create && subnode->direct[subnode_j] == NO_SECTOR) {
                block_sector_t sec;
                if (free_map_get(&sec)) {
                    ASSERT(sec < fs_disk_size());
                    subnode->direct[subnode_j] = sec;
                    allocated = true;
                }
            }
            if (subnode->direct[subnode_j] != NO_SECTOR) {
                ASSERT(subnode->direct[subnode_j] < fs_disk_size());
                ret = subnode->direct[subnode_j];
            } else {
                ret = -1;
            }
            fs_cache_release(subnode);
        } else {
            ret = -1;
        }
    }
    if (allocated) fs_cache_write(ret, NULL);
    return ret;
}

/*! List of open inodes, so that opening a single inode twice
    returns the same `struct inode'. */
static list_t open_inodes;
/*! Lock to synchronize use of open_inodes. */
static lock_t open_lock;

/*! Initializes the inode module. */
void inode_init(void) {
    list_init(&open_inodes);
    lock_init(&open_lock);
}

/*! Initializes an inode with LENGTH bytes of data and
    writes the new inode to sector SECTOR on the file system
    device.
    Never fails. */
bool inode_create(block_sector_t sector, off_t length) {
    ASSERT(length >= 0);

    /* If this assertion fails, the inode structure is not exactly
       one sector in size, and you should fix that. */
    ASSERT(sizeof(inode_disk_t) == BLOCK_SECTOR_SIZE);

    inode_disk_t *disk_inode = fs_cache_get(sector, CACHE_NOLOAD);
    disk_inode->header.length = length;
    disk_inode->header.magic = INODE_MAGIC;
    disk_inode->header.counter = 0;
    for (size_t i = 0; i < NUM_DIRECT; i++) {
        disk_inode->direct[i] = NO_SECTOR;
    }
    for (size_t i = 0; i < NUM_INDIRECT; i++) {
        disk_inode->indirect[i] = NO_SECTOR;
    }

    fs_cache_release(disk_inode);
    return true;
}

/*! Reads an inode from SECTOR
    and returns a `struct inode' that contains it.
    Returns a null pointer if memory allocation fails. */
inode_t* inode_open(block_sector_t sector) {
    list_elem_t *e;
    inode_t *inode;

    /* Check whether this inode is already open. */
    lock_acquire(&open_lock);
    for (e = list_begin(&open_inodes); e != list_end(&open_inodes);
         e = list_next(e)) {
        inode = list_entry(e, struct inode, elem);
        if (inode->sector == sector) {
            inode_reopen(inode);
            goto exit;
        }
    }

    /* Allocate memory. */
    inode = malloc(sizeof *inode);
    if (inode == NULL)
        goto exit;

    /* Initialize. */
    inode->sector = sector;
    inode->open_cnt = 1;
    inode->deny_write_cnt = 0;
    inode->removed = false;
    rw_init(&inode->lock);
    list_push_front(&open_inodes, &inode->elem);
    exit:
    lock_release(&open_lock);
    return inode;
}

/*! Reopens and returns INODE. */
inode_t * inode_reopen(inode_t *inode) {
    if (inode != NULL) atomic_add(&inode->open_cnt, 1);
    return inode;
}

/*! Returns INODE's inode number. */
block_sector_t inode_get_inumber(const inode_t *inode) {
    return inode->sector;
}

uint32_t inode_get_open_cnt(const inode_t * inode){
    return inode->open_cnt;
}

/*! Closes INODE and writes it to disk.
    If this was the last reference to INODE, frees its memory.
    If INODE was also a removed inode, frees its blocks. */
void inode_close(inode_t *inode) {
    /* Ignore null pointer. */
    if (inode == NULL)
        return;

    /* Release resources if this was the last opener. */
    if (atomic_add(&inode->open_cnt, -1) == 0) {
        /* Remove from inode list and release lock. */
        lock_acquire(&open_lock);
        list_remove(&inode->elem);
        lock_release(&open_lock);
 
        /* Deallocate blocks if removed. */
        // Even though we're freeing the sector in the free map, fs_cache_get 
        // ensures that it won't actually be mutated until we've released it
        if (inode->removed) {
            inode_disk_t *data = fs_cache_get(inode->sector, 0);
            for (size_t i = 0; i < NUM_DIRECT; i++) {
                if (data->direct[i] != NO_SECTOR) {
                    free_map_release(data->direct[i], 1);
                }
            }
            fs_sector_t indirect[NUM_INDIRECT];
            memcpy(indirect, data->indirect, sizeof(indirect));
            fs_cache_release(data);
            for (size_t i = 0; i < NUM_INDIRECT; i++) {
                if (indirect[i] != NO_SECTOR) {
                    indirect_node_t *subnode = fs_cache_get(indirect[i], 0);
                    for (size_t i = 0; i < NUM_DIRECT; i++) {
                        if (subnode->direct[i] != NO_SECTOR) {
                            free_map_release(subnode->direct[i], 1);
                        }
                    }
                    free_map_release(indirect[i], 1);
                    fs_cache_release(subnode);
                }
            }
            free_map_release(inode->sector, 1);
        }

        free(inode); 
    }
}

/*! Marks INODE to be deleted when it is closed by the last caller who
    has it open. */
void inode_remove(inode_t *inode) {
    ASSERT(inode != NULL);
    inode->removed = true;
}

/*! Reads SIZE bytes from INODE into BUFFER, starting at position OFFSET.
   Returns the number of bytes actually read, which may be less
   than SIZE if an error occurs or end of file is reached. */
off_t inode_read_at(inode_t *inode, void *buffer_, off_t size, off_t offset) {
    uint8_t *buffer = buffer_;
    off_t bytes_read = 0;
    while (size > 0) {
        /* Bytes left in inode, bytes left in sector, lesser of the two. */
        off_t inode_left = inode_length(inode) - offset;
        
        /* Disk sector to read, starting byte offset within sector. */
        block_sector_t sector_idx = byte_to_sector (inode, offset, false);
        int sector_ofs = offset % BLOCK_SECTOR_SIZE;

        if (inode_left > BLOCK_SECTOR_SIZE) {
            block_sector_t next_sector_idx = 
                byte_to_sector(inode, offset + BLOCK_SECTOR_SIZE, false);
            fs_request_read_ahead(next_sector_idx);
        }
        int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
        int min_left = inode_left < sector_left ? inode_left : sector_left;

        /* Number of bytes to actually copy out of this sector. */
        int chunk_size = size < min_left ? size : min_left;
        if (chunk_size <= 0)
            break;

        if (sector_ofs == 0 && chunk_size == BLOCK_SECTOR_SIZE) {
            /* Read full sector directly into caller's buffer. */
            fs_cache_read(sector_idx, buffer + bytes_read);
        }
        else {
            /* Read sector into bounce buffer, then partially copy
               into caller's buffer. */
            void *sec = fs_cache_get(sector_idx, 0);
            memcpy(buffer + bytes_read, sec + sector_ofs, chunk_size);
            fs_cache_release(sec);
        }
      
        /* Advance. */
        size -= chunk_size;
        offset += chunk_size;
        bytes_read += chunk_size;
    }
    return bytes_read;
}

/*! Writes SIZE bytes from BUFFER into INODE, starting at OFFSET.
    Returns the number of bytes actually written, which may be
    less than SIZE if end of file is reached or an error occurs.
    (Normally a write at end of file would extend the inode, but
    growth is not yet implemented.) */
off_t inode_write_at(inode_t *inode, const void *buffer_, off_t size, off_t offset) {
    const uint8_t *buffer = buffer_;
    off_t bytes_written = 0;

    if (inode->deny_write_cnt) return 0;

    while (size > 0) {
        /* Sector to write, starting byte offset within sector. */
        block_sector_t sector_idx = byte_to_sector(inode, offset, true);
        // there is no sector for the given offset and we couldn't allocate one
        if (sector_idx == (block_sector_t) -1) break;
        int sector_ofs = offset % BLOCK_SECTOR_SIZE;

        /* Bytes left in inode, bytes left in sector, lesser of the two. */
        int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
        int min_left = sector_left;

        /* Number of bytes to actually write into this sector. */
        int chunk_size = size < min_left ? size : min_left;
        if (chunk_size <= 0)
            break;

        if (sector_ofs == 0 && chunk_size == BLOCK_SECTOR_SIZE) {
            /* Write full sector directly to disk. */
            fs_cache_write(sector_idx, buffer + bytes_written);
        }
        else {
            void *sec = fs_cache_get(sector_idx, CACHE_WRITE);
            memcpy(sec + sector_ofs, buffer + bytes_written, chunk_size);
            fs_cache_release(sec);
        }

        /* Advance. */
        size -= chunk_size;
        offset += chunk_size;
        bytes_written += chunk_size;
    }
    inode_disk_t *data = fs_cache_get(inode->sector, CACHE_WRITE);
    if (data->header.length < offset) {
        data->header.length = offset;
    }
    fs_cache_release(data);
    return bytes_written;
}

/*! Disables writes to INODE.
    May be called at most once per inode opener. */
void inode_deny_write (inode_t *inode) {
    ASSERT(atomic_add(&inode->deny_write_cnt, 1) <= inode->open_cnt);
}

/*! Re-enables writes to INODE.
    Must be called once by each inode opener who has called
    inode_deny_write() on the inode, before closing the inode. */
void inode_allow_write (inode_t *inode) {
    ASSERT(inode->deny_write_cnt <= inode->open_cnt);
    ASSERT(atomic_add(&inode->deny_write_cnt, -1) >= 0);
}

/*! Returns the length, in bytes, of INODE's data. */
off_t inode_length(const inode_t *inode) {
    inode_disk_t *data = fs_cache_get(inode->sector, 0);
    off_t length = data->header.length;
    fs_cache_release(data);
    return length;
}

/*! Returns the current value of the counter for an inode. */
int32_t inode_counter_get(const inode_t *inode) {
    inode_disk_t *data = fs_cache_get(inode->sector, 0);
    int32_t counter = data->header.counter;
    fs_cache_release(data);
    return counter;
}

/*! Atomically adds to the counter of an inode and returns the new value */
int32_t inode_counter_add(const inode_t *inode, int32_t x) {
    inode_disk_t *data = fs_cache_get(inode->sector, CACHE_WRITE);
    int32_t counter = data->header.counter = data->header.counter + x;
    fs_cache_release(data);
    return counter;
}

/*! Lock the advisory lock as a reader. */
void inode_lock_read(inode_t *inode) {
    rw_read_acquire(&inode->lock);
}
/*! Lock the advisory lock as a writer. */
void inode_lock_write(inode_t *inode) {
    rw_write_acquire(&inode->lock);
}
/*! Unlock the advisory lock held as a reader. */
void inode_unlock_read(inode_t *inode) {
    rw_read_release(&inode->lock);
}
/*! Unlock the advisory lock held as a writer. */
void inode_unlock_write(inode_t *inode) {
    rw_write_release(&inode->lock);
}