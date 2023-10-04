#include "hash.h"
#include "bitmap.h"
#include "string.h"
#include "debug.h"
#include "devices/timer.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include "threads/interrupt.h"

#include "filesys.h"
#include "fsdisk.h"

/*! The mode of a read/write lock, used to be able to release cache entries
    generically rather than specifying whether you're releasing as a reader or
    writer. LOCK_UNLOCKED exists for error checking. */
typedef enum {
    LOCK_UNLOCKED = 0,
    LOCK_READ = 1,
    LOCK_WRITE = 2,
} lock_mode_t;

/*! An entry in the cache. */
typedef struct cache_entry {
    hash_elem_t elem;                   /*!< Hash element to insert into cache. */
    block_sector_t sector;              /*!< The sector this caches. */
    uint32_t pin_count;                 /*!< The number of users pinning this. */
    lock_t evict;                       /*!< Lock to be held while evicting. */
    uint8_t buffer[BLOCK_SECTOR_SIZE];  /*!< The buffer for caching. */
    rwlock_t lock;                      /*!< Lock used for synchronizing
                                             reads and writes to the buffer via
                                             _read/_write/_get. */
    lock_mode_t lock_mode : 2;          /*!< Whether the entry is locked
                                             to read (LOCK_READ), locked to 
                                             write (LOCK_WRITE), or unlocked 
                                             (LOCK_UNLOCKED).
                                             */
    lock_t can_read_lock;               /*!< Lock used to prevent TOCTOU bugs in
                                             loading the buffer from the disk
                                             on initial read. */
    bool can_read;                      /*!< Whether the entry has been loaded
                                             from disk or overwritten entriely.
                                             Must be true before reads can be
                                             made. */
    uint64_t last_accessed;             /*!< Time this was last accessed, or
                                             NEVER_ACCESSED. */
    bool dirty;                         /*!< Whether this cache entry has been
                                             dirtied. */
    bool free;                          /*!< Whether the entry is unused. Reads
                                             and writes are assumed to be
                                             synchronized with the global lock.*/
} cache_entry_t;

/*! The value of last_accessed if the entry hasn't been accessed. */
#define NEVER_ACCESSED ((uint64_t) -1)

/*! Partition that contains the file system. */
static block_t *device;

/*! Boolean indicator for whether the cache has been closed. Used by the read
    ahead and write behind helpers. */
static bool cache_closed;

/*! Hash map between sectors and the cache entries which represent them. */
static hash_t cache;

/*! For working with the hashmap itself. */
static lock_t cache_lock;

/*! The size of the cache, in block device sectors. */
#define CACHE_SECTORS 64

/*! The memory actually used by the buffer. Allocated as a global variable 
    since the size is known at compile time to avoid allocations. 
    
    NOTE: Pintos limits the total size of global variables to 1 megabyte, so the
    cache should be significantly smaller than that. */
static cache_entry_t entries[CACHE_SECTORS];

/*! The cache buffer for the free map, which we're allowed to not count against
    our 64 cache sectors. It is also periodically flushed by the write_behind
    functionality. */

/*! Buffer for the largest possible free map. It is unsynchronized and the 
free map is itself responsible for ensuring synchronization. */
static uint8_t free_map_buffer[FREE_MAP_BUF_SIZE];
                                       
/*! The actualy number of free sectors. Sectors 1 -- free_map_sectors 
    are assumed to be free map sectors (sector 0 is the root directory's inode). */
static block_sector_t free_map_sectors;

/*! Stores whether the free map is dirty. */
static bool free_map_dirty;

/*! Lock for the free map. */
static lock_t free_map_lock;

/*! A block sector-sized buffer of zeros. */
static const uint8_t ZERO_BUF[BLOCK_SECTOR_SIZE] = { 0 };

/*! Concurrency safe, rotating queue for read_ahead entries. 
    If it's full, read ahead requests
    will simply be lost (since blocking on other read ahead requests would
    defeat the point, and losing read ahead requests is not a correctness
    error, only a potential performance penalty)*/
#define READ_AHEAD_QUEUE_SIZE 16
static semaphore_t read_ahead_free;    /*!< Semaphore of free spots. */
static semaphore_t read_ahead_used;    /*!< Semaphore of used spots. */
static lock_t read_ahead_lock;         /*!< Lock for the following fields. */
static size_t read_ahead_head;         /*!< Next element to pop. */
static size_t read_ahead_tail;         /*!< Where to insert next pushed element. */
/*! The buffer for the queue. */
static block_sector_t read_ahead_queue[READ_AHEAD_QUEUE_SIZE];

static void write_behind_start(void);

static void read_ahead_start(void);
static void read_ahead_enqueue(block_sector_t);
static block_sector_t read_ahead_dequeue(void);

static void cache_clean(cache_entry_t *entry);
static unsigned cache_hash(const hash_elem_t *, void *);
static bool cache_less(const hash_elem_t *, const hash_elem_t *, void *);
static cache_entry_t *cache_get(block_sector_t, lock_mode_t);
static void cache_release(cache_entry_t *);
static void cache_pin(cache_entry_t *);
static bool cache_try_pin(cache_entry_t *);
static void cache_unpin(cache_entry_t *);
static bool cache_try_pin_evict(cache_entry_t *);
static void cache_unpin_evict(cache_entry_t *);
static cache_entry_t *cache_get_free(void);
static void entry_init(cache_entry_t *);
static cache_entry_t *cache_set(cache_entry_t *, block_sector_t);
static void cache_ensure_can_read(cache_entry_t *);
static void cache_set_can_read(cache_entry_t *);

static void fs_cache_init(void);
static void fs_cache_destroy(void);

/*! Initializes the file system's disk and memory-cache.
    Writes through the cache are guaranteed to be synchronized across the OS,
    but need not be reflected on the disk until fs_cache_flush() or
    fs_disk_close() is invoked. 

    Writes through fs_disk_write() may not be reflected in calls to
    fs_cache_read even if _cache_flush() has been invoked. fs_disk_read() will
    reflect changes made through fs_cache_write() 

    As a result, fs_disk_write() and fs_cache_write() should not be used on
    the same sectors.
    
    Calls made to any other fsdisk functions are undefined until init returns. */
void fs_disk_init(void) {
    device = block_get_role(BLOCK_FILESYS);
    if (device == NULL) {
        PANIC("No file system device found, can't initialize file system.");
    }
    if (fs_disk_size() > MAX_DISK_SIZE / BLOCK_SECTOR_SIZE) {
        PANIC("Your disk is too big! We can't handle it!");
    }
    fs_cache_init();
}

/*! Returns the size, in sectors, of the file system device. */
block_sector_t fs_disk_size(void) {
    return block_size(device);
}

/*! Checks whether the given sector is a sector of the free map. */
static bool is_free_map_sec(block_sector_t sector) {
    return 1 <= sector && sector < FREE_MAP_START + free_map_sectors;
}

/*! Converts a free map sector to its location in the free map buffer. */
static uint8_t *free_map_sec_to_buf(block_sector_t sector) {
    ASSERT(is_free_map_sec(sector));
    return free_map_buffer + (sector - FREE_MAP_START) * BLOCK_SECTOR_SIZE;
}

/*! Initializes the file system cache. Helper for fs_disk_init(). Because the
    cache does not require */
static void fs_cache_init(void) {
    if (!hash_init(&cache, cache_hash, cache_less, NULL)) {
        PANIC("Could not initialize file system cache.");
    }
    cache_closed = false;
    for (size_t i = 0; i < CACHE_SECTORS; i++) {
        entry_init(&entries[i]);
    }
    ASSERT(bitmap_buf_size(fs_disk_size()) <= FREE_MAP_BUF_SIZE);
    free_map_sectors = 
        DIV_ROUND_UP(bitmap_buf_size(fs_disk_size()), BLOCK_SECTOR_SIZE);
    for (block_sector_t i = FREE_MAP_START; 
         i < free_map_sectors + FREE_MAP_START; i++) {
        fs_disk_read(i, free_map_sec_to_buf(i));
    }
    lock_init(&cache_lock);
    lock_init(&free_map_lock);
    write_behind_start();
    read_ahead_start();
}

/*! Closes the file system cache. Any writes that occurred before invocation
    are guaranteed to be reflected on the disk once this function returns. 
    Writes concurrent with _close() are undefined. */
void fs_disk_close(void) {
    fs_cache_destroy();
}

/*! Flushes the file system cache.
    Writes concurrent with _flush() are may or may not be reflected
    on the disk depending on interleaving, but are otherwise correct. 
    
    If blocking is set to true, any writes that occurred before invocation
    are guaranteed to be reflected on the disk once this function returns, but
    this function may have to wait for other IO operations.
    
    If blocking is false, any entries that are in use when it is their turn to
    be cleaned will be skipped. */
void fs_cache_flush(bool blocking) {
    for (size_t i = 0; i < CACHE_SECTORS; i++) {
        cache_entry_t *entry = &entries[i];
        if (blocking) {
            cache_pin(entry);
            cache_clean(entry);
            cache_unpin(entry);
        } else if (cache_try_pin(entry)) {
            cache_clean(entry);
            cache_unpin(entry);
        }
    }
    if (free_map_dirty) {
        lock_acquire(&free_map_lock);
        for (block_sector_t i = FREE_MAP_START;
            i < free_map_sectors + FREE_MAP_START; i++) {
            fs_disk_write(i, free_map_sec_to_buf(i));
        }
        free_map_dirty = false;
        lock_release(&free_map_lock);
    }
}

/*! Destroys the file system cache, flushing it. */
static void fs_cache_destroy(void) {
    free_map_dirty = true;
    fs_cache_flush(true);
    cache_closed = true;
}

/*! Makes an uncached write directly to the file system's block device. */
void fs_disk_write(block_sector_t sector, const void *buf) {
    ASSERT(sector < fs_disk_size());
    block_write(device, sector, buf);
}
/*! Makes an uncached read directly from the file system's block device. */
void fs_disk_read(block_sector_t sector, void *buf) {
    ASSERT(sector < fs_disk_size());
    block_read(device, sector, buf);
}

/*! Makes a cached write to the file system's block device. See _init()
    for details on coherence. Writes may be lost if the kernel exits without
    _close() or _flush() being invoked. */
void fs_cache_write(block_sector_t sector, const void *buf) {
    ASSERT(sector < fs_disk_size());
    cache_entry_t *entry = cache_get(sector, LOCK_WRITE);
    ASSERT(entry->sector == sector);
    entry->last_accessed = timer_ticks();
    entry->dirty = true;
    if (buf != NULL) {
        memcpy(entry->buffer, buf, BLOCK_SECTOR_SIZE);
    } else {
        memcpy(entry->buffer, ZERO_BUF, BLOCK_SECTOR_SIZE);
    }
    // since we overwrite the entire buffer, we declare that it can be read from
    // without loading from disk now.
    cache_set_can_read(entry);
    cache_release(entry);
}
/*! Makes a cached read from the file system's block device. See _init()
    for details on coherence. Writes may be lost if the kernel exits without
    _close() or _flush() being invoked. 
    
    As a utility, invoking this function with the sector -1 fills the buffer
    with zeros. */
void fs_cache_read(block_sector_t sector, void *buf) {
    if (sector == (block_sector_t) -1) {
        memset(buf, 0, BLOCK_SECTOR_SIZE);
        return;
    }
    ASSERT(sector < fs_disk_size());
    cache_entry_t *entry = cache_get(sector, LOCK_READ);
    ASSERT(entry->sector == sector);
    cache_ensure_can_read(entry);
    entry->last_accessed = timer_ticks();
    memcpy(buf, entry->buffer, BLOCK_SECTOR_SIZE);
    cache_release(entry);
}

/*! Returns the cache buffer for a given sector. Writes to/reads from the buffer
    are then equivalent to interactions through cache_write/cache_read, except
    that they do not overwrite or read the entire buffer at once.
    
    Behavior is unspecified if the WRITE flag is not set and the buffer is
    written to. The initial value of the buffer is unspecified if the
    noload flag is set. 
    
    The buffer is held locked until _release is called.
    
    As a utility, if WRITE is not set and the sector is -1, returns a pointer to
    an array of 0's. */
void *fs_cache_get(block_sector_t sector, uint32_t flags) {
    if (sector == (block_sector_t) -1 && 
        !(flags & (CACHE_WRITE | CACHE_NOLOAD))) {
        return (void *) ZERO_BUF;
    }
    ASSERT(sector < fs_disk_size());
    bool noload = (flags & CACHE_NOLOAD) != 0;
    lock_mode_t mode = (flags & CACHE_WRITE) != 0 || noload ? 
                       LOCK_WRITE : LOCK_READ;
    cache_entry_t *entry = cache_get(sector, mode);
    ASSERT(entry->sector == sector);
    if (mode == LOCK_WRITE) entry->dirty = true;
    if (noload) {
        cache_set_can_read(entry);
    } else {
        cache_ensure_can_read(entry);
    }
    return entry->buffer;
}

/*! Releases a buffer cache obtained from fs_cache_get. */
void fs_cache_release(void *buffer) {
    if (buffer == (void *) ZERO_BUF) return;
    if (buffer == free_map_buffer) {
        free_map_dirty = true;
        lock_release(&free_map_lock);
        return;
    }
    ASSERT(!cache_closed);
    cache_entry_t *entry = buffer - offsetof(cache_entry_t, buffer);
    entry->last_accessed = timer_ticks();
    cache_release(entry);
}

/*! Returns the buffer containing the free map, if free_map_create() has been
    invoked on this disk image. */
void *fs_cache_get_free_map_buf(void) {
    lock_acquire(&free_map_lock);
    return free_map_buffer;
}

/*! Converts pointer to hash elem embeded in cache entry to pointer to that
    cache entry. */
static inline cache_entry_t *cache_entry(const hash_elem_t *e) {
    return hash_entry(e, cache_entry_t, elem);
}

/*! Flushes a cache entry to the disk. Since this can perform a disk operation,
    it cannot be called while holding the global cache lock. */
static void cache_clean(cache_entry_t *entry) {
    // ASSERT(!lock_held_by_current_thread(&cache_lock));
    rw_read_acquire(&entry->lock);
    if (entry->dirty) {
        fs_disk_write(entry->sector, entry->buffer);
        entry->dirty = false;
    }
    rw_read_release(&entry->lock);
}
/*! Computes the hash of a cache entry. */
static unsigned cache_hash(const hash_elem_t *e, void *aux UNUSED) {
    return hash_int(cache_entry(e)->sector);
}
/*! A total order on cache entries. */
static bool cache_less(const hash_elem_t *a, const hash_elem_t *b,
                       void *aux UNUSED) {
    return cache_entry(a)->sector < cache_entry(b)->sector;
}
/*! Looks up a cache entry by sector. If one is not found, creates it.
    If one is found, locks its lock as a reader if `write` is false or a writer
    if `write` is true, then returns it. */
static cache_entry_t *cache_get(block_sector_t sector, lock_mode_t mode) {
    ASSERT(!cache_closed);
    ASSERT(!is_free_map_sec(sector));
    cache_entry_t lookup = {.sector = sector};
    lock_acquire(&cache_lock);
    cache_entry_t *entry;
    do {
        entry = cache_entry(hash_find(&cache, &lookup.elem));
        if (entry != NULL && !cache_try_pin(entry)) {
            // if the entry is currently being evicted, wait for that to
            // finish, then continue.
            lock_release(&cache_lock);
            cache_pin(entry);
            cache_unpin(entry);
            lock_acquire(&cache_lock);
            entry = NULL;
            continue;
        }
        if (entry == NULL) {
            entry = cache_set(cache_get_free(), sector);
        }
    } while (entry == NULL);
    ASSERT(entry->sector == sector);
    lock_release(&cache_lock);

    if (mode == LOCK_WRITE) {
        rw_write_acquire(&entry->lock);
        ASSERT(entry->lock_mode == LOCK_UNLOCKED);
    } else {
        ASSERT(mode == LOCK_READ);
        rw_read_acquire(&entry->lock);
        ASSERT(entry->lock_mode != LOCK_WRITE)
    }
    entry->lock_mode = mode;
    return entry;
}

/*! Releases a cache entry obtained from cache_get. `write` should have the same
    value given to cache_get. */
static void cache_release(cache_entry_t *entry) {
    lock_mode_t mode = entry->lock_mode;
    entry->lock_mode = LOCK_UNLOCKED;
    if (mode == LOCK_WRITE) {
        rw_write_release(&entry->lock);
    } else {
        rw_read_release(&entry->lock);
    }
    cache_unpin(entry);
}

/*! Pins the entry as user. Blocks until it is not pinned to evict. */
static void cache_pin(cache_entry_t *entry) {
    lock_acquire(&entry->evict);
    entry->pin_count++;
    lock_release(&entry->evict);
}

/*! Tries to pin the entry as user. Fails only if it's already pinned to evict. */
static bool cache_try_pin(cache_entry_t *entry) {
    bool success = lock_try_acquire(&entry->evict);
    if (success) {
        entry->pin_count++;
        lock_release(&entry->evict);
    }
    return success;
}

/*! Unpins the entry as user. */
static void cache_unpin(cache_entry_t *entry) {
    ASSERT(entry->pin_count > 0);
    enum intr_level old_level = intr_disable();
    entry->pin_count--;
    intr_set_level(old_level);
}

/*! Tries to pin the entry as evictor. Fails if anybody has pinned as user. */
static bool cache_try_pin_evict(cache_entry_t *entry) {
    bool success;
    enum intr_level old_level = intr_disable();
    if (entry->pin_count == 0) {
        success = lock_try_acquire(&entry->evict);
    } else {
        success = false;
    }
    intr_set_level(old_level);
    return success;
}

/*! Unpins the entry as an evictor. */
static void cache_unpin_evict(cache_entry_t *entry) {
    lock_release(&entry->evict);
}

/*! Returns a cache entry to evict, pinned to evict. The entry returned may or
    may not be clean. */
static cache_entry_t *entry_to_evict(void) {
    static size_t clock_hand = 0;
    // We don't lock the clock hand because we don't really care if it fails to
    // increment occasionally, which is the only race-y interleaving.
    for (size_t i = clock_hand++ % CACHE_SECTORS;; i = (i+1) % CACHE_SECTORS) {
        cache_entry_t *entry = &entries[i];
        // entry is being used by someone, skip it.
        if (!cache_try_pin_evict(entry)) continue;

        // entry is already free, so just use it.
        if (entry->free) return entry;

        // the clock part; if it's accessed, flag as unaccessed and move on.
        if (entry->last_accessed != NEVER_ACCESSED) {
            entry->last_accessed = NEVER_ACCESSED;
            cache_unpin_evict(entry);
            continue;
        }

        // cache_clean has its own check for dirtiness, but this way we avoid
        // releasing and reacquiring the lock for no reason.
        if (entry->dirty) {
            lock_release(&cache_lock);
            cache_clean(entry);
            lock_acquire(&cache_lock);
        }

        // An entry pinned for eviction should never be dirtied.
        ASSERT(!entry->dirty)

        return entry;
    }
}

/*! Gets a free element, evicting an existing one if necessary. Must be called
    will holding the cache lock. */
static cache_entry_t *cache_get_free(void) {
    ASSERT(lock_held_by_current_thread(&cache_lock));

    cache_entry_t *entry = entry_to_evict();
    ASSERT(lock_held_by_current_thread(&entry->evict));
    ASSERT(entry->free || entry->last_accessed == NEVER_ACCESSED);
    if (!entry->free) {
        ASSERT(hash_delete(&cache, &entry->elem) == &entry->elem);
        entry->free = true;
    }
    ASSERT(entry->dirty == false);

    return entry;
}

/*! Initializes the entry for general use. This should only be invoked once for
    each entry. Entries are initialized as unpinned. */
static void entry_init(cache_entry_t *entry) {
    entry->pin_count = 0;
    entry->free = true;
    rw_init(&entry->lock);
    lock_init(&entry->evict);
    lock_init(&entry->can_read_lock);
}

/*! Returns an entry for the given sector registered with the cache. If one
    already exists, returns NULL instead.
    
    Caller must hold cache_lock. 
    
    The given entry must be free, pinned for eviction, and clean. */
static cache_entry_t *cache_set(cache_entry_t *entry, block_sector_t sector) {
    ASSERT(entry != NULL);
    ASSERT(lock_held_by_current_thread(&cache_lock));
    ASSERT(lock_held_by_current_thread(&entry->evict));
    ASSERT(entry->free);
    ASSERT(!entry->dirty);

    entry->sector = sector;
    entry->last_accessed = NEVER_ACCESSED;
    entry->can_read = false;
    entry->pin_count = 1;

    cache_entry_t *ret;
    if (hash_insert(&cache, &entry->elem) != NULL) {
        entry->pin_count = 0;
        ret = NULL;
    } else {
        entry->free = false;
        ASSERT(entry->sector == sector);
        ret = entry;
    }
    lock_release(&entry->evict);
    return ret;
}

/*! Ensures that the cache entry has been loaded from disk or overwritten. */
static void cache_ensure_can_read(cache_entry_t *entry) {
    lock_acquire(&entry->can_read_lock);
    if (!entry->can_read) {
        fs_disk_read(entry->sector, entry->buffer);
        entry->can_read = true;
    }
    lock_release(&entry->can_read_lock);
}

/*! Sets the cache entry to reflect that it can read from. */
static void cache_set_can_read(cache_entry_t *entry) {
    lock_acquire(&entry->can_read_lock);
    entry->can_read = true;
    lock_release(&entry->can_read_lock);
}

/*! Helper for write-behind functionality of the cache; flushes the cache to
    disk FLUSH_FREQ times per second. */
static void write_behind_helper(void *aux UNUSED) {
    const int64_t FLUSH_FREQ = 10;
    const int64_t FLUSH_PERIOD = TIMER_FREQ / FLUSH_FREQ;
    while (true) {
        timer_sleep(FLUSH_PERIOD);
        if (cache_closed) break;
        fs_cache_flush(false);
    }
}

/*! Starts the write behind system. */
static void write_behind_start(void) {
    thread_create("write behind", PRI_DEFAULT, write_behind_helper, NULL);
}

/*! Helper for read-ahead functionality. Dequeues read ahead requests and reads
    them from the disk in a loop. Blocks if there are no requests. */
static void read_ahead_helper(void *aux UNUSED) {
    while (true) {
        block_sector_t sector = read_ahead_dequeue();
        if (cache_closed) break;
        cache_entry_t *entry = cache_get(sector, LOCK_READ);
        ASSERT(entry->sector == sector);
        cache_ensure_can_read(entry);
        cache_release(entry);
    }
}

/*! Starts the read ahead system. */
static void read_ahead_start(void) {
    read_ahead_head = read_ahead_tail = 0;
    sema_init(&read_ahead_free, READ_AHEAD_QUEUE_SIZE);
    sema_init(&read_ahead_used, 0);
    lock_init(&read_ahead_lock);
    thread_create("read ahead", PRI_DEFAULT, read_ahead_helper, NULL);
}

/*! Enqueues a read ahead request. Because this is a performance optimization
    and not a correctness issue, this function will fail SILENTLY if the queue
    is full. If the sector is -1, does nothing. */
static void read_ahead_enqueue(block_sector_t sector) {
    if (sector == (block_sector_t) -1) return;
    lock_acquire(&read_ahead_lock);
    if (sema_try_down(&read_ahead_free)) {
        read_ahead_queue[read_ahead_tail] = sector;
        read_ahead_tail = (read_ahead_tail + 1) % READ_AHEAD_QUEUE_SIZE;
        sema_up(&read_ahead_used);
    }
    lock_release(&read_ahead_lock);
}

/*! Dequeues a read ahead request. Blocks until the queue is non-empty. */
static block_sector_t read_ahead_dequeue(void) {
    block_sector_t sector;
    sema_down(&read_ahead_used);
    lock_acquire(&read_ahead_lock);
    sector = read_ahead_queue[read_ahead_head];
    read_ahead_head = (read_ahead_head + 1) % READ_AHEAD_QUEUE_SIZE;
    sema_up(&read_ahead_free);
    lock_release(&read_ahead_lock);
    return sector;
}

/*! External interface to make a read-ahead request, for inode to make smarter
    requests than just "the next one." */
void fs_request_read_ahead(block_sector_t sector) {
    read_ahead_enqueue(sector);
}