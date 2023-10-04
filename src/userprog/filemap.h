#ifndef USERPROG_FILEMAP_H
#define USERPROG_FILEMAP_H

#include <kernel/list.h>
#include <inttypes.h>
#include "filesys/file.h"
#include "filesys/directory.h"

/*! Number of files that a process can have open and access them quickly.
    Files opened while this many are open are stored in an overflow list and are
    slower to access. */
#define NUM_QUICK_FILES 8

#define FM_ERROR ((uint32_t) -1)

/*! A flag for whether a given entry is an ordinary file or a directory. */
typedef struct {
    char is_dir : 1;    /*!< True for directory, false for ordinary file. */
} dir_flag_t;

/*! Represents a mapping between files/directories and userspace `fd`s. */
typedef struct file_map {
    void *quick[NUM_QUICK_FILES];    /*!< Array of files for small fds
                                                 since majority of programs will
                                                 only open a couple files. */
    dir_flag_t flags[NUM_QUICK_FILES];    /*!< Whether each is an ordinary
                                                 file or directory. */
    list_t overflow;                        /*!< List to store arbitrary number
                                                 of files beyond quick access. */
    uint32_t first_open;                    /*!< First available file index slot
                                                 (fi = fd - NUM_RESERVED_FDS). */
} file_map_t;

typedef void fm_file_action_func(file_t *, void *);
typedef void fm_dir_action_func(dir_t *, void *);

void filemap_init(file_map_t *fm);
uint32_t filemap_insert(file_map_t *fm, void *, bool is_dir);
void *filemap_get(file_map_t *fm, uint32_t fd, bool *is_dir);
void *filemap_remove(file_map_t *fm, uint32_t fd, bool *is_dir);
bool filemap_is_dir(file_map_t *fm, uint32_t fd);
void filemap_foreach(file_map_t *fm, fm_file_action_func file_action,
    fm_dir_action_func dir_action, void *aux);
void filemap_destroy(file_map_t *fm, fm_file_action_func file_destructor,
    fm_dir_action_func dir_destructor, void *aux);

#endif /* userprog/filemap.h */

