#ifndef __LIB_KERNEL_BITMAP_H
#define __LIB_KERNEL_BITMAP_H

#include <stdbool.h>
#include <stddef.h>
#include <inttypes.h>
#include <round.h>
#include <limits.h>

/* Element type.

   This must be an unsigned integer type at least as wide as int.

   Each bit represents one bit in the bitmap.
   If bit 0 in an element represents bit K in the bitmap,
   then bit 1 in the element represents bit K+1 in the bitmap,
   and so on. */
typedef unsigned long elem_type;

/* Bitmap abstract data type. */
/* From the outside, a bitmap is an array of bits.  From the
   inside, it's an array of elem_type (defined above) that
   simulates an array of bits. */
typedef struct bitmap
  {
    size_t bit_cnt;     /* Number of bits. */
    elem_type *bits;    /* Elements that represent bits. */
  } bitmap_t;

/* Number of bits in an element. */
#define ELEM_BITS (sizeof (elem_type) * CHAR_BIT)

/* Returns the number of bytes required to accomodate a bitmap
   with BIT_CNT bits (for use with bitmap_create_in_buf()). */
#define BITMAP_BUF_SIZE(bit_cnt) (sizeof (struct bitmap) +\
    sizeof (elem_type) * DIV_ROUND_UP (bit_cnt, ELEM_BITS))

/* Creation and destruction. */
bitmap_t *bitmap_create (size_t bit_cnt);
bitmap_t *bitmap_create_in_buf (size_t bit_cnt, void *, size_t byte_cnt);
size_t bitmap_buf_size(size_t bit_cnt);
void bitmap_destroy (bitmap_t *);

/* Bitmap size. */
size_t bitmap_size (const bitmap_t *);

/* Setting and testing single bits. */
void bitmap_set (bitmap_t *, size_t idx, bool);
void bitmap_mark (bitmap_t *, size_t idx);
void bitmap_reset (bitmap_t *, size_t idx);
void bitmap_flip (bitmap_t *, size_t idx);
bool bitmap_test (const bitmap_t *, size_t idx);

/* Setting and testing multiple bits. */
void bitmap_set_all (bitmap_t *, bool);
void bitmap_set_multiple (bitmap_t *, size_t start, size_t cnt, bool);
size_t bitmap_count (const bitmap_t *, size_t start, size_t cnt, bool);
bool bitmap_contains (const bitmap_t *, size_t start, size_t cnt, bool);
bool bitmap_any (const bitmap_t *, size_t start, size_t cnt);
bool bitmap_none (const bitmap_t *, size_t start, size_t cnt);
bool bitmap_all (const bitmap_t *, size_t start, size_t cnt);

/* Finding set or unset bits. */
#define BITMAP_ERROR SIZE_MAX
size_t bitmap_scan (const bitmap_t *, size_t start, size_t cnt, bool);
size_t bitmap_scan_and_flip (bitmap_t *, size_t start, size_t cnt, bool);

size_t bitmap_lowest (const bitmap_t *, bool);
size_t bitmap_highest (const bitmap_t *, bool);

/* File input and output. */
#ifdef FILESYS
struct file;
size_t bitmap_file_size (const bitmap_t *);
bool bitmap_read (bitmap_t *, struct file *);
bool bitmap_write (const bitmap_t *, struct file *);
#endif

/* Debugging. */
void bitmap_dump (const bitmap_t *);

#endif /* lib/kernel/bitmap.h */
