#ifndef VM_FRAMETBL_H
#define VM_FRAMETBL_H

#include <inttypes.h>
#include <stdbool.h>
#include <list.h>
#include "threads/vaddr.h"
#include "threads/synch.h"
#include "vm/mappings.h"

typedef uint8_t age_t;
#define AGE_MAX ((age_t) -1);

/*! Frame table entry, indicating what pages are loaded into each frame. */
typedef struct fte {
    vm_mapping_t *mapping;  /*!< The mapping which owns the frame. */
    bin_sema_t lock;        /*!< For pinning a frame, to prevent its eviction. */
    age_t age;              /*!< The "age" of the frame for aging. The lowest
                                 age gets evicted. */
} fte_t;

/*! Frame table. Stores mappings from each frame to user space pages which
    are loaded into them. */
typedef struct frametbl {
    size_t num_frames;  /*!< The number of user-space frames. */
    list_t unused;      /*!< A list of frames without pages loaded into them. */
    lock_t lock;        /*!< Lock for manipulating the frame table. */
    void *base;         /*!< The start of the user-space frames. */
    fte_t tbl[];        /*!< Table of frame table entries. */
} frametbl_t;

/*! Representation of a single frame. */
typedef struct frame {
    uint8_t bytes[PGSIZE];
} frame_t;

/*! Pointer to the global frame table shared by all processes.
    Must be initialized by FRAMETBL_CREATE_IN_BUF. */
frametbl_t *frame_tbl;

size_t frametbl_buf_size(size_t num_frames);
frametbl_t *frametbl_create_in_buf(size_t num_frames, void *block,
    size_t block_size);

frame_t *frametbl_get_frame(void);
bool frametbl_install_page(vm_mapping_t *mapping, frame_t *frame);
void frametbl_empty_frame(frame_t *frame);

bool frametbl_try_pin_frame(frame_t *frame);
void frametbl_unpin_frame(frame_t *frame);

void frametbl_tick(size_t block, size_t block_cnt);

#endif /* VM_FRAME_H */