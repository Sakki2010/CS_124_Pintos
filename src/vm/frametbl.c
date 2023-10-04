#include "frametbl.h"
#include "threads/vaddr.h"
#include "userprog/pagedir.h"
#include "threads/palloc.h"
#include "threads/pte.h"
#include <stdio.h>

#define UNPINNED 1 // Frames started with semaphores unpinned

static bool try_pin_fte(fte_t *fte);
static void unpin_fte(fte_t *fte);

/*! Given an FTE, returns its index in the FT entries array. */
static size_t get_fte_inx(fte_t *fte) {
    uintptr_t offset = (uintptr_t) fte - (uintptr_t) frame_tbl->tbl;
    return (size_t)(offset / sizeof(fte_t));
}

/*! Given an FTE, returns the corresponding frame (kernel virtual address). */
static void *get_frame(fte_t *fte) {
    size_t frame_no = get_fte_inx(fte);
    ASSERT(frame_no < frame_tbl->num_frames);
    uintptr_t frame = ((uintptr_t) frame_no) * PGSIZE +
        (uintptr_t) frame_tbl->base;
    return (void *) frame;
}

/*! Gets the frame associated with a list element. */
static void *list_entry_frame(list_elem_t *elem) {
    return pg_round_down(elem);
}

/*! Gets the list element associated with a frame. */
static list_elem_t *get_elem(void *frame) {
    return (list_elem_t *)(frame + PGSIZE - sizeof(list_elem_t));
}

/*! Initializes an empty FTE associated with the FT. */
static void init_fte(fte_t *fte) {
    fte->mapping = NULL;
    bin_sema_init(&fte->lock, UNPINNED);
    list_push_back(&frame_tbl->unused, get_elem(get_frame(fte)));
}

/*! Initializes the empty frame table at FRAME_TBL
    with NUM_FRAMES empty entries. */
static void init_frametbl(size_t num_frames) {
    ASSERT(frame_tbl != NULL && frame_tbl->base != NULL);
    frame_tbl->num_frames = num_frames;
    list_init(&frame_tbl->unused);
    lock_init(&frame_tbl->lock);

    fte_t *tbl = frame_tbl->tbl;
    for (size_t i = 0; i < num_frames; i++) {
        fte_t *fte = tbl + i;
        init_fte(fte);
    }
}

/*! Returns how large of a buffer a frame table would need for
    supporting NUM_FRAMES frames. */
size_t frametbl_buf_size(size_t num_frames) {
    return sizeof(frametbl_t) + sizeof(fte_t) * num_frames;
}

/*! Creates the frame table in BLOCK of size BLOCK_SIZE which supports
    NUM_FRAMES frames. */
frametbl_t *frametbl_create_in_buf(size_t num_frames, void *block,
        size_t block_size) {
    ASSERT(block_size >= frametbl_buf_size(num_frames));

    frame_tbl = (frametbl_t *) block;
    frame_tbl->base = block + block_size;
    init_frametbl(num_frames);
    ASSERT(!pg_ofs(frame_tbl->base)); // must be page-aligned
    return frame_tbl;
}

/*! Given a frame (page-aligned kernel address), gets a pointer to the FTE
    associated with that frame. */
static fte_t *get_fte(void *frame) {
    ASSERT(!pg_ofs(frame) && is_kernel_vaddr(frame)); // must be page-aligned
    uintptr_t frame_no = ((uintptr_t) frame - (uintptr_t) frame_tbl->base)
        / PGSIZE;
    ASSERT(frame_no < frame_tbl->num_frames);
    return frame_tbl->tbl + frame_no;
}

static bool valid_frame(void *frame) {
    return frame >= frame_tbl->base 
            && get_fte_inx(get_fte(frame)) < frame_tbl->num_frames;
}

/*! Performs the aging required on a tick. Invoked by thread_tick(). */
void frametbl_tick(size_t block, size_t block_cnt) {
    size_t start = frame_tbl->num_frames * block / block_cnt;
    size_t end = frame_tbl->num_frames * (block + 1) / block_cnt;
    for (size_t i = start; i < end; i++) {
        fte_t *fte = &frame_tbl->tbl[i];
        int a = vm_try_reset_accessed(fte->mapping);
        if (a != -1 && try_pin_fte(fte)) {
            fte->age >>= 1;
            fte->age |= a << 7;
            unpin_fte(fte);
        }
    }
}

/*! Chooses a frame to evict. */
static fte_t *frame_to_evict(void) {
    static size_t hand_shared = 0;
    while (true) {
        size_t hand = hand_shared++;
        fte_t *best = NULL;
        age_t best_age = AGE_MAX;
        for (size_t i = 0; i < frame_tbl->num_frames; i++) {
            fte_t *fte = &frame_tbl->tbl[(i + hand) % frame_tbl->num_frames];
            if (!try_pin_fte(fte)) continue;

            if (best_age >= fte->age) {
                if (best != NULL) unpin_fte(best);
                best_age = fte->age;
                best = fte;
            } else {
                unpin_fte(fte);
            }
            
            if (best_age == 0) return best;
        }
        if (best != NULL) return best;
    }
}

/*! Gets a frame (kernel virtual address) from the frame table for
    immediate use. The returned frame is pinned and should be unpinned by the
    user if necessary. */
frame_t *frametbl_get_frame(void) {
    fte_t *fte;
    lock_acquire(&frame_tbl->lock);

    while (list_empty(&frame_tbl->unused)) {
        lock_release(&frame_tbl->lock);
        fte = frame_to_evict();
        vm_evict_page(fte->mapping);
        lock_acquire(&frame_tbl->lock);
    }

    void *frame = list_entry_frame(list_pop_front(&frame_tbl->unused));
    ASSERT(frametbl_try_pin_frame(frame));
    lock_release(&frame_tbl->lock);
    return frame;
}

/*! Installs PAGE (user virtual address) into FRAME (kernel virtual address,
    presumably from palloc_get_page) in the frame table. */
bool frametbl_install_page(vm_mapping_t *mapping, frame_t *frame) {
    ASSERT(valid_frame(frame));
    get_fte(frame)->mapping = mapping;
    return true;
}

/*! Tries to pin a frame to make it not evictable.
    Returns whether it succeeded. */
bool frametbl_try_pin_frame(frame_t *frame) {
    ASSERT(valid_frame(frame));
    return try_pin_fte(get_fte(frame));
}

/*! Unpins a frame. */
void frametbl_unpin_frame(frame_t *frame) {
    ASSERT(valid_frame(frame));
    unpin_fte(get_fte(frame));
    
}

/*! Try_pin, and unpin by frame table entry rather than frame itself. */
static bool try_pin_fte(fte_t *fte) {
    return bin_sema_try_down(&fte->lock);
}
static void unpin_fte(fte_t *fte) {
    bin_sema_up(&fte->lock);
}

/*! Marks a frame as empty and breaks its links to pages in the frame table.
    Must be pinned. */
void frametbl_empty_frame(frame_t *frame) {
    ASSERT(valid_frame(frame));
    fte_t *fte = get_fte(frame);
    ASSERT(!try_pin_fte(fte));
    lock_acquire(&frame_tbl->lock);
    unpin_fte(fte);
    init_fte(fte); // Re-initialize
    lock_release(&frame_tbl->lock);
}