#ifndef FILESYS_FILESYS_H
#define FILESYS_FILESYS_H

#include <stdbool.h>
#include "filesys/off_t.h"
#include "filesys/file.h"
#include "filesys/directory.h"
#include "devices/block.h"
#include "threads/synch.h"

/*! Sectors of system file inodes. @{ */
#define FREE_MAP_START 1        /*!< The start of the free map. The free map is
                                     not stored as a file and is simply computed
                                     from disk size. */
#define ROOT_DIR_SECTOR 0       /*!< Root directory file inode sector. */
/*! @} */

void filesys_init(bool format);
void filesys_done(void);
bool filesys_create_file(const char *path, off_t initial_size, dir_t *);
bool filesys_create_dir(const char *path, dir_t *);
file_t *filesys_open_file(const char *path, dir_t *);
dir_t *filesys_open_dir(const char *path, dir_t *);
void *filesys_open(const char *path, dir_t *, bool *is_dir);

bool filesys_remove(const char *name, dir_t *);

#endif /* filesys/filesys.h */

