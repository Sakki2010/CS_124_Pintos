#include "filesys/directory.h"
#include <stdio.h>
#include <string.h>
#include <list.h>
#include "filesys/filesys.h"
#include "filesys/inode.h"
#include "threads/malloc.h"

/*! A directory. */
struct dir {
    inode_t *inode;                /*!< Backing store. */
    off_t pos;                     /*!< Current position. */
};

/*! A single directory entry. Designed to be exactly 16 bytes in size. */
typedef struct dir_entry {
    char name[NAME_MAX];        /*!< File name. May not have null terminator. */
    uint16_t inode_sector : 14; /*!< Sector number of header. 8 MiB file system,
                                    so at most 2^23/512 = 2^14 sectors. */
    uint16_t in_use : 1;        /*!< In use or free? */
    uint16_t is_dir : 1;        /*!< Directory or ordinary file? */
} dir_entry_t;

#define DEFAULT_ENTRY_CNT 16

/*! Sets the name for a given directory entry. We have to be careful
    about this since we don't reserve space for the null terminator,
    so there's a vulnerability to a buffer overflow unless this is
    handled precisely. */
static void dir_entry_set_name(dir_entry_t *e, const char *name) {
    size_t name_len = strlen(name);
    ASSERT(name_len <= NAME_MAX);

    memcpy(e->name, name, name_len);
    // Null terminate, if there's space
    if (name_len < NAME_MAX) e->name[name_len] = '\0';
}

/*! Creates a directory with space for ENTRY_CNT entries in the
    given SECTOR. Returns true if successful, false on failure. */
static bool _dir_create(block_sector_t sector, size_t entry_cnt,
        block_sector_t parent) {
    if (!inode_create(sector, entry_cnt * sizeof(dir_entry_t))) return false;

    dir_t *dir = dir_open(inode_open(sector));
    if (dir == NULL) return false;

    bool success = false;
    if (!dir_add(dir, SELF_STR, sector, true)) goto fail;
    if (!dir_add(dir, PARENT_STR, parent, true)) goto fail;
    inode_counter_add(dir->inode, -2);
    success = true;

    fail:
    dir_close(dir);
    return success;
}

/*! Creates the root directory with space for ENTRY_CNT entries.
    Returns true on success, false otherwise. */
bool dir_create_root(void) {
    return _dir_create(ROOT_DIR_SECTOR, DEFAULT_ENTRY_CNT,
            ROOT_DIR_SECTOR);
}

/*! Creates a directory with INITIAL_SIZE space in the
    given SECTOR. Returns true if successful, false on failure. */
bool dir_create(block_sector_t sector, dir_t *_parent) {
    block_sector_t parent = inode_get_inumber(_parent->inode);
    return _dir_create(sector, DEFAULT_ENTRY_CNT, parent);
}

/*! Opens and returns the directory for the given INODE, of which
    it takes ownership. Returns a null pointer on failure. */
dir_t *dir_open(inode_t *inode) {
    if (inode == NULL) return NULL; 

    dir_t *dir = calloc(1, sizeof(dir_t));
    if (dir != NULL) {
        dir->inode = inode;
        dir->pos = 0;
        return dir;
    }
    inode_close(inode);
    return NULL;
}

/*! Opens and returns the root directory with a new INODE, of which
    it takes ownership. Returns a null pointer on failure. */
dir_t *dir_open_root(void) {
    return dir_open(inode_open(ROOT_DIR_SECTOR));
}

/* Opens and returns a new directory for the same inode as DIR.
   Returns a null pointer on failure. */
dir_t *dir_reopen(dir_t *dir) {
    if (dir == NULL) return NULL;
    return dir_open(inode_reopen(dir->inode));
}

/*! Destroys DIR and frees associated resources. */
void dir_close(dir_t *dir) {
    if (dir != NULL) {
        inode_close(dir->inode);
        free(dir);
    }
}

/*! Returns the inode encapsulated by DIR. */
inode_t *dir_get_inode(dir_t *dir) {
    return dir->inode;
}

/*! Searches DIR for a file with the given NAME.
    If successful, returns true, sets *EP to the directory entry
    if EP is non-null, and sets *OFSP to the byte offset of the
    directory entry if OFSP is non-null.
    otherwise, returns false and ignores EP and OFSP.
    Must be holding the inode lock, or produces undefined behavior. */
static bool lookup(const dir_t *dir, const char *name,
                   dir_entry_t *ep, off_t *ofsp) {
    ASSERT(dir != NULL);
    ASSERT(name != NULL);

    dir_entry_t e;
    for (size_t ofs = 0; inode_read_at(dir->inode, &e,
            sizeof(dir_entry_t), ofs) == sizeof(dir_entry_t);
         ofs += sizeof(dir_entry_t)) {
        if (e.in_use && !strncmp(name, e.name, NAME_MAX)) {
            if (ep != NULL) *ep = e;
            if (ofsp != NULL) *ofsp = ofs;
            return true;
        }
    }
    return false;
}

/*! Searches DIR for a file with the given NAME and returns true if one exists,
    false otherwise. On success, sets *INODE to an inode for the file,
    otherwise to a null pointer. The caller must close *INODE.
    On success, also sets *IS_DIR to whether the file is a directory or not.
    IS_DIR is only valid if *INODE is not null. */
bool dir_lookup(const dir_t *dir, const char *name, inode_t **inode, bool
        *is_dir) {
    ASSERT(dir != NULL);
    ASSERT(name != NULL);

    dir_entry_t e;
    bool success = false;
    inode_lock_read(dir->inode);
    if (lookup(dir, name, &e, NULL)) {
        *inode = inode_open((block_sector_t) e.inode_sector);
        if (*inode != NULL) {
            *is_dir = e.is_dir;
            success = true;
            goto exit;
        }
    }
    *inode = NULL;
    exit:
    inode_unlock_read(dir->inode);
    return success;
}

/*! Adds a file named NAME to DIR, which must not already contain a file by
    that name. The file's inode is in sector INODE_SECTOR.
    Returns true if successful, false on failure.
    Fails if NAME is invalid (i.e. too long) or a disk or memory
    error occurs. */
bool dir_add(dir_t *dir, const char *name, block_sector_t inode_sector,
        bool is_dir) {
    ASSERT(dir != NULL);
    ASSERT(name != NULL);

    bool success = false;
    inode_lock_write(dir->inode);
    if (*name == '\0' || strlen(name) > NAME_MAX || // Check NAME for validity.
            lookup(dir, name, NULL, NULL)) { // Check that NAME is not in use.
        goto exit;
    }

    /* Set OFS to offset of free slot.
       If there are no free slots, then it will be set to the
       current end-of-file.
       
       inode_read_at() will only return a short read at end of file.
       Otherwise, we'd need to verify that we didn't get a short
       read due to something intermittent such as low memory. */
    
    dir_entry_t e;
    off_t ofs;
    for (ofs = 0; inode_read_at(dir->inode, &e,
            sizeof(dir_entry_t), ofs) == sizeof(dir_entry_t);
         ofs += sizeof(dir_entry_t)) {
        if (!e.in_use) break;
    }

    /* Write slot. */
    e.in_use = true;
    dir_entry_set_name(&e, name);
    e.inode_sector = inode_sector;
    e.is_dir = is_dir;
    success = inode_write_at(dir->inode, &e,
        sizeof(dir_entry_t), ofs) == sizeof(dir_entry_t);
    inode_counter_add(dir->inode, 1);
    exit:
    inode_unlock_write(dir->inode);
    return success;
}

/*! Removes any entry for NAME in DIR. Returns true if successful, false on
    failure, which occurs only if there is no file with the given NAME. */
bool dir_remove(dir_t *dir, const char *name) {
    ASSERT(dir != NULL);
    ASSERT(name != NULL);

    if (!strcmp(name, PARENT_STR) || !strcmp(name, SELF_STR)) return false;

    bool success = false;
    /* Find directory entry. */
    dir_entry_t e;
    off_t ofs;
    
    inode_lock_write(dir->inode);
    if (!lookup(dir, name, &e, &ofs)) goto exit;

    inode_t *inode = inode_open((block_sector_t) e.inode_sector);
    if (inode == NULL) goto exit;

    /* Erase directory entry. */
    e.in_use = false;
    ASSERT(inode_write_at(dir->inode, &e, sizeof(dir_entry_t), ofs) == 
           sizeof(dir_entry_t));

    inode_remove(inode);
    inode_close(inode);
    success = true;
    inode_counter_add(dir->inode, -1);
    exit:
    inode_unlock_write(dir->inode);
    return success;
}

/*! Reads the next directory entry in DIR and stores the name in NAME. Returns
    true if successful, false if the directory contains no more entries. */
bool dir_readdir(dir_t *dir, char name[NAME_MAX + 1]) {
    dir_entry_t e;
    bool success = false;
    inode_lock_read(dir->inode);
    while (inode_read_at(dir->inode, &e,
            sizeof(dir_entry_t), dir->pos) == sizeof(dir_entry_t)) {
        dir->pos += sizeof(dir_entry_t);
        if (e.in_use && strncmp(e.name, PARENT_STR, NAME_MAX)
                && strncmp(e.name, SELF_STR, NAME_MAX)) {
            strlcpy(name, e.name, NAME_MAX + 1);
            success = true;
            break;
        } 
    }
    inode_unlock_read(dir->inode);
    return success;
}

