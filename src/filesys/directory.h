#ifndef FILESYS_DIRECTORY_H
#define FILESYS_DIRECTORY_H

#include <stdbool.h>
#include <stddef.h>
#include "devices/block.h"
#include "filesys/inode.h"

/*! Maximum length of a file name component.
    This is the traditional UNIX maximum length.
    After directories are implemented, this maximum length may be
    retained, but much longer full path names must be allowed. */
#define NAME_MAX 14

#define PARENT_STR ".."
#define SELF_STR "."

struct inode;

typedef struct dir dir_t;

/* Opening and closing directories. */
bool dir_create_root(void);
bool dir_create(block_sector_t sector, dir_t *parent);
dir_t *dir_open(inode_t *);
dir_t *dir_open_root(void);
dir_t *dir_reopen(dir_t *);
void dir_close(dir_t *);
inode_t *dir_get_inode(dir_t *);

/* Reading and writing. */
bool dir_lookup(const dir_t *, const char *name, inode_t **, bool *is_dir);
bool dir_add(dir_t *, const char *name, block_sector_t, bool is_dir);
bool dir_remove(dir_t *, const char *name);
bool dir_readdir(dir_t *, char name[NAME_MAX + 1]);

#endif /* filesys/directory.h */

