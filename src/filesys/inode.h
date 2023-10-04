#ifndef FILESYS_INODE_H
#define FILESYS_INODE_H

#include <stdbool.h>
#include "filesys/off_t.h"
#include "devices/block.h"

struct bitmap;
typedef struct inode inode_t;

void inode_init(void);
bool inode_create(block_sector_t, off_t);
inode_t *inode_open(block_sector_t);
inode_t *inode_reopen(inode_t *);
block_sector_t inode_get_inumber(const inode_t *);
uint32_t inode_get_open_cnt(const inode_t *);
void inode_close(inode_t *);
void inode_remove(inode_t *);
off_t inode_read_at(inode_t *, void *, off_t size, off_t offset);
off_t inode_write_at(inode_t *, const void *, off_t size, off_t offset);
void inode_deny_write(inode_t *);
void inode_allow_write(inode_t *);
off_t inode_length(const inode_t *);
int32_t inode_counter_get(const inode_t *);
int32_t inode_counter_add(const inode_t *, int32_t);
void inode_lock_read(inode_t *);
void inode_lock_write(inode_t *);
void inode_unlock_read(inode_t *);
void inode_unlock_write(inode_t *);

#endif /* filesys/inode.h */
