#ifndef FILESYS_FILE_H
#define FILESYS_FILE_H

#include "filesys/off_t.h"
#include "filesys/inode.h"

struct inode;

typedef struct file file_t;

/* Opening and closing files. */
file_t *file_open (inode_t *);
file_t *file_reopen (file_t *);
void file_close (file_t *);
inode_t *file_get_inode (file_t *);

/* Reading and writing. */
off_t file_read (file_t *, void *, off_t);
off_t file_read_at (file_t *, void *, off_t size, off_t start);
off_t file_write (file_t *, const void *, off_t);
off_t file_write_at (file_t *, const void *, off_t size, off_t start);

/* Preventing writes. */
void file_deny_write (file_t *);
void file_allow_write (file_t *);

/* File position. */
void file_seek (file_t *, off_t);
off_t file_tell (file_t *);
off_t file_length (file_t *);

#endif /* filesys/file.h */

