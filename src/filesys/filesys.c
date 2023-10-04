#include "filesys/filesys.h"
#include <debug.h>
#include <stdio.h>
#include <string.h>
#include "filesys/file.h"
#include "filesys/free-map.h"
#include "filesys/inode.h"
#include "filesys/directory.h"
#include "filesys/fsdisk.h"
#include "threads/palloc.h"

#define NO_SECTOR ((block_sector_t) -1)

static bool filesys_locate_parent(const char *path, const dir_t *wd,
        char **file_str, dir_t **dir);
static dir_t *filesys_locate_dir(const char *path, dir_t *wd, size_t len);
static void do_format(void);

/*! Initializes the file system module.
    If FORMAT is true, reformats the file system. */
void filesys_init(bool format) {
    fs_disk_init();
    inode_init();
    free_map_init();

    if (format) do_format();
}

/*! Shuts down the file system module, writing any unwritten data to disk. */
void filesys_done(void) {
    fs_disk_close();
}

static bool filesys_create(const char *path, off_t initial_size, bool is_dir,
        dir_t *wd) {
    char *file_str;
    dir_t *dir;
    
    if (!filesys_locate_parent(path, wd, &file_str, &dir)) return false;

    bool success = false;
    block_sector_t inode_sector = NO_SECTOR;
    if (!free_map_get(&inode_sector)) goto done;

    if (is_dir) {
        if (!dir_create(inode_sector, dir)) goto done;
    } else {
        if (!inode_create(inode_sector, initial_size)) goto done;
    }
    
    if (!dir_add(dir, file_str, inode_sector, is_dir)) goto done;

    success = true;
    
    done:
    if (!success && inode_sector != NO_SECTOR) {
        free_map_release(inode_sector, 1);
    }
    dir_close(dir);
    return success;
}

/*! Creates an ordinary file at PATH (absolute or relative to WD) with the given
    INITIAL_SIZE. Returns true if successful, false otherwise. Fails if a file
    at PATH already exists, a valid directory to PATH doesn't exist, or if
    internal memory allocation fails. */
bool filesys_create_file(const char *path, off_t initial_size, dir_t *wd) {
    return filesys_create(path, initial_size, false, wd);
}

/*! Creates a directory at PATH (absolute or relative to WD).
    Returns true if successful, false otherwise. Fails if a file
    at PATH already exists, a valid directory to PATH doesn't exist, or if
    internal memory allocation fails. */
bool filesys_create_dir(const char *path, dir_t *wd) {
    return filesys_create(path, 0, true, wd);
}

/*! Attempts to locate the directory corresponding to PATH in the file
    system. PATH may be absolute or relative to WD. Only considers LEN
    characters of PATH. Does not modify PATH. Returns the directory
    on success, or NULL on failure. */
static dir_t *filesys_locate_dir(const char *path, dir_t *wd, size_t len) {
    bool abs = (*path == '/' || wd == NULL);
    if (abs) {
        wd = dir_open_root();
    }

    char *_path = palloc_get_page(0);
    strlcpy(_path, path, len + 1);

    dir_t *dir = dir_reopen(wd);
    char *subdir_str, *save_ptr;
    for (subdir_str = strtok_r(_path, "/", &save_ptr); subdir_str != NULL;
            subdir_str = strtok_r(NULL, "/", &save_ptr)) {
        
        // Get subdirectory from directory
        inode_t *inode;
        bool is_dir;
        if (!dir_lookup(dir, subdir_str, &inode, &is_dir) || inode == NULL ||
                is_dir == false) {
            dir_close(dir);
            palloc_free_page(_path);
            return NULL;
        }

        dir_t *subdir = dir_open(inode);
        dir_close(dir);
        dir = subdir;
    }
    
    if (abs) {
        dir_close(wd);
    }

    palloc_free_page(_path);
    return dir;
}

/*! Returns the directory at the given path, or null on failure.
    If PATH is relative, it is relative to the working directory WD.
    Fails if anything in PATH isn't a valid directory, or if an
    internal memory allocation fails. */
dir_t *filesys_open_dir(const char *path, dir_t *wd) {
    if (path == NULL || *path == '\0') return NULL;
    return filesys_locate_dir(path, wd, strlen(path));
}

/*! Attempts to locate where the file corresponding to PATH (absolute or
    relative to WD) should be. Does NOT require that the file exist already.
    Valid for both directories and ordinary files.

    Returns true on success, false otherwise. If it succeeds, FILE_STR is
    the file name and DIR is the directory it's in. Fails if anything in
    PATH isn't a valid directory (except the file itself), or if an internal
    memory allocation fails. */
static bool filesys_locate_parent(const char *path, const dir_t *wd,
        char **file_str, dir_t **dir) {
    if (path == NULL || *path == '\0') return false;

    *file_str = strrchr(path, '/');
    if (*file_str == NULL) { // Path is already only file name
        *file_str = (char *) path;
        *dir = wd ? dir_reopen((dir_t *) wd) : dir_open_root();
    } else { // Find correct directory
        size_t len = (size_t) (*file_str - path);
        if (len == strlen(path) - 1) return false; // Path can't end in `/`

        *dir = filesys_locate_dir(path, (dir_t *) wd, len);
        if (*dir == NULL) return false; // Directory doesn't exist
        (*file_str)++; // Strip off the last `/`, leaving the file name behind
    }
    return true;
}

/*! Opens the file at the path, sets *IS_DIR on success. */
static inode_t *filesys_open_inode(const char *path, dir_t *wd, bool *is_dir) {
    char *file_str;
    dir_t *dir;
    
    if (!filesys_locate_parent(path, wd, &file_str, &dir)) return NULL;

    // Get file from directory
    inode_t *inode;
    if (!dir_lookup(dir, file_str, &inode, is_dir) || inode == NULL) {
        dir_close(dir);
        return NULL;
    }

    dir_close(dir);
    return inode;
}

/*! Returns the ordinary file at the given path, or null on failure.
    If PATH is relative, it is relative to the working directory WD.
    Fails if anything in PATH isn't a valid directory, or if an
    internal memory allocation fails. */
file_t *filesys_open_file(const char *path, dir_t *wd) {
    bool is_dir;
    inode_t *inode = filesys_open_inode(path, wd, &is_dir);
    if (is_dir) {
        inode_close(inode);
        return NULL;
    } else {
        return file_open(inode);
    }
}

/*! Returns the file at the given path, or null on failure.
    If PATH is relative, it is relative to the working directory WD.
    Fails if anything in PATH isn't a valid directory, or if an
    internal memory allocation fails.
    On success, sets *IS_DIR to whether or not it's a directory. */
void *filesys_open(const char *path, dir_t *wd, bool *is_dir) {
    if (path == NULL || *path == '\0') return NULL;

    // If it ends in a slash, require that it's a directory
    if (path[strlen(path) - 1] == '/') {
        *is_dir = true;
        return filesys_open_dir(path, wd);
    }

    inode_t *inode = filesys_open_inode(path, wd, is_dir);
    
    if (inode == NULL) return NULL;
    return *is_dir ? (void *) dir_open(inode) : (void *) file_open(inode);
}

/*! Deletes the file named NAME in DIR. Returns true on success, false on
    failure. Fails if no file named NAME exists, or if an internal memory
    allocation fails. */
bool filesys_remove(const char *path, dir_t *dir) {
    char *file_name;
    dir_t *file_dir;

    inode_t *inode;
    bool is_dir;
    bool success = false;

    if (!filesys_locate_parent(path, dir, &file_name, &file_dir)) {
        return false;
    }
    if (!dir_lookup(file_dir, file_name, &inode, &is_dir)) {
        goto exit;
    }
    if (is_dir) {
        inode_lock_write(inode);
        if (inode_get_open_cnt(inode) <= 1 && inode_counter_get(inode) == 0) {
            success = dir_remove(file_dir, file_name);
        }
        inode_unlock_write(inode);
    } else {
        success = dir_remove(file_dir, file_name);
    }
    inode_close(inode);
    exit:
    dir_close(file_dir);
    return success;
}

/*! Formats the file system. */
static void do_format(void) {
    printf("Formatting file system...");
    free_map_create();
    if (!dir_create_root()) {
        PANIC("root directory creation failed");
    }
    printf("done.\n");
}

