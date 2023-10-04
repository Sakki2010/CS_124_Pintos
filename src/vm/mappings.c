/*! \file pagedir.c
 *
 * Functions for initializing and manipulating page directory tables and
 * entries.
 */
#include "vm/mappings.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include "threads/malloc.h"
#include "threads/init.h"
#include "threads/palloc.h"
#include "threads/synch.h"
#include "userprog/pagedir.h"
#include "vm/frametbl.h"
#include "vm/swaptbl.h"
#include "filesys/filesys.h"

#include <stdio.h>

/*! Represents what a virtual page should contain. */
struct vm_mapping {
    hash_elem_t elem;   /*!< Element to insert in hash. */
    void *page;         /*!< Address being mapped from. */
    bin_sema_t lock;    /*!< Page lock. */
    int present : 1;    /*!< this address is currently mapped. */
    int writable : 1;   /*!< this mapping is writable. */
    int hasfile : 1;    /*!< this is a file-backed mapping. */
    int fwrite : 1;     /*!< the backing file can be written to. */
    int map_start : 1;  /*!< this is the start of a file mapping. */
    int orphaned : 1;   /*!< this mapping is unmapped and should be freed when
                             it is next evicted. */
    int swapped : 1;    /*!< Whether the page has been swapped. If it is not
                             present, data.swap_slot indicates the slot. */
    int isstack : 1;    /*!< Whether the page is a stack page. */
    sup_pagetable_t *pt;/*!< The page table this mapping belongs to, if orphaned
                             is false. Undefined if orphaned is true. */
    frame_t *frame;     /*!< The frame currently mapped to. 
                             Undefined if present and orphaned are false. */
    union {
        struct {
            file_t *file;           /*!< File to read/write from. */
            unsigned offset : 20;   /*!< Location in the file in pages. */
            unsigned size : 12;     /*!< Max bytes to read from the file - 1. */
        } file_info;                /*!< Valid if hasfile is true. */
        uintptr_t swap_slot;        /*!< Valid if hasfile and present are false. */
    } data;
};

static vm_mapping_t *mapping_lookup(sup_pagetable_t *pt, const void *addr);

static vm_mapping_t *map_entry(const hash_elem_t *a);
static unsigned mapping_hash(const hash_elem_t *a, void *aux UNUSED);
static bool mapping_less(const hash_elem_t *a, const hash_elem_t *b,
                         void *aux UNUSED); 
static void mapping_destroy(hash_elem_t *a, void *aux UNUSED);
static void mapping_free(vm_mapping_t *mapping);
static void mapping_delete(sup_pagetable_t *pt, const void *addr);

/*! Creates a supplemental page table at the given buffer. Returns false if
    memory allocation fails and true on success. */
bool sup_pt_create(sup_pagetable_t *pt) {
    pt->pd = pagedir_create();
    pt->user = true;
    if (pt->pd == NULL) {
        return false;
    }
    if (!hash_init(&pt->mappings, mapping_hash, mapping_less, NULL)) {
        pagedir_destroy(pt->pd);
        return false;
    }
    return true;
}

/*! Destroys a supplemental page table, deallocating associated memory, freeing
    used swap slots, writing mapped files to disk, and closing owned files. */
void sup_pt_destroy(sup_pagetable_t *pt) {
    /* Correct ordering here is crucial.  We must set
    pt->user to false before switching page directories,
    so that a timer interrupt can't switch back to the
    process page directory.  We must activate the base page
    directory before destroying the process's page
    directory, or our active page directory will be one
    that's been freed (and cleared). */
    uint32_t *pd = pt->pd;
    pt->user = false;
    pagedir_activate(NULL);
    hash_destroy(&pt->mappings, mapping_destroy);
    pagedir_destroy(pd);
}

/*! Actives the page directory associated with the supplemental page table. */
void sup_pt_activate(sup_pagetable_t *pt) {
    if (pt->user) {
        pagedir_activate(pt->pd);
    } else {
        pagedir_activate(NULL);
    }
}

/*! Indicates whether the page table corresponds to a kernel thread. */
bool sup_pt_is_kernel(sup_pagetable_t *pt) {
    return !pt->user;
}

/*! Marks where a user page expects its memory to come from, without actually
    necessarily loading that memory into a frame. This does not affect 
    actual mappings.

    Does not lock the page because an eviction can't occur until load occurs,
    and load cannot occur until this returns.

    If backing file is not NULL, offset specifies where in the file it is and
    file_writable specifies whether you're allowed to write changes back to the
    file when evicting the page. */
bool vm_set_page(sup_pagetable_t *pt, void *upage, uint32_t flags,
                     file_t *backing, off_t ofs, size_t size) {
    ASSERT(pg_ofs(upage) == 0);
    ASSERT(is_user_vaddr(upage));
    ASSERT(pt->pd != init_page_dir);

    vm_mapping_t *mapping = calloc(1, sizeof(vm_mapping_t));
    if (mapping == NULL) return false;
    bin_sema_init(&mapping->lock, 1);
    mapping->page = upage;
    mapping->writable = (flags & MAP_WRITE) != 0;
    mapping->map_start = (flags & MAP_START) != 0;
    mapping->isstack = (flags & MAP_STACK) != 0;
    mapping->pt = pt;

    if (backing != NULL && size > 0) {
        ASSERT(ofs % PGSIZE == 0);
        ASSERT(size <= PGSIZE);
        mapping->hasfile = 1;
        mapping->fwrite = (flags & MAP_FWRITE) != 0;
        if (mapping->fwrite) {
            if ((backing = file_reopen(backing)) == NULL) {
                free(mapping);
                return false;
            }
        }
        mapping->data.file_info.file = backing;
        mapping->data.file_info.offset = ofs / PGSIZE;
        mapping->data.file_info.size = size - 1;
    }

    ASSERT(hash_insert(&pt->mappings, &mapping->elem) == NULL);

    return true;
}

/*! Sets a given page with the flags for a stack page. */
bool vm_set_stack_page(sup_pagetable_t *pt, void *upage) {
    return vm_set_page(pt, upage, MAP_WRITE | MAP_STACK, NULL, 0, 0);
}

/*! Installs a stack page (anonymous, writable) at the given virtual address and
    then returns the frame. Returns NULL on failure, including if there is 
    already a page at this address. The returned frame is pinned. */
frame_t *vm_set_load_stack_page(sup_pagetable_t *pt, void *upage) {
    if (!vm_page_is_mappable(pt, upage) || !vm_set_stack_page(pt, upage)) {
        return NULL;
    }
    return vm_load_page(pt, upage);
}

/*! Checks whether the given page is mapped for the user. */
bool vm_page_is_mapped(sup_pagetable_t *pt, const void *upage) {
    ASSERT(pg_ofs(upage) == 0);

    return mapping_lookup(pt, upage) != NULL;
}

/*! Returns true if the given page exists and can be written by the user. */
bool vm_page_is_writeable(sup_pagetable_t *pt, const void *upage) {
    ASSERT(pg_ofs(upage) == 0);

    vm_mapping_t *mapping = mapping_lookup(pt, upage);
    return mapping != NULL && mapping->writable;
}

/*! Returns true if the given page could be mapped by the user. If this returns
    true, a call to vm_set_page should be valid, barring race conditions. */
bool vm_page_is_mappable(sup_pagetable_t *pt, const void *upage) {
    ASSERT(pg_ofs(upage) == 0);

    return is_user_vaddr(upage) && mapping_lookup(pt, upage) == NULL;
}

/*! Checks whether a page is a stack page. */
bool vm_page_is_stack(sup_pagetable_t *pt, const void *upage) {
    ASSERT(pg_ofs(upage) == 0);

    vm_mapping_t *mapping = mapping_lookup(pt, upage);
    return mapping != NULL && mapping->isstack;
}

/*! Returns true if the given page is the start of a file mapping. */
bool vm_page_is_mapping_start(sup_pagetable_t *pt, const void *upage) {
    ASSERT(pg_ofs(upage) == 0);

    vm_mapping_t *mapping = mapping_lookup(pt, upage);
    return mapping != NULL && mapping->map_start;
}

/*! Returns the last page in a file mapping. Input page must be the start of
    a file mapping. */
void *vm_page_get_mapping_end(sup_pagetable_t *pt, const void *upage) {
    vm_mapping_t *mapping = mapping_lookup(pt, upage);
    ASSERT(mapping != NULL && mapping->map_start);
    size_t len = file_length(mapping->data.file_info.file);
    return pg_round_down(upage + len - 1);
}

/*! Loads and populates a frame for an anonymous page. Return NULL on failure,
    otherwise the frame. */
static void *load_anonymous_page(void) {
    return palloc_get_page(PAL_ZERO | PAL_USER);
}

/*! Loads and populates a frame for a page which is backed by a file.
    Return NULL on failure, otherwise the frame. */
static void *load_file_page(vm_mapping_t *mapping) {
    ASSERT(mapping != NULL);
    ASSERT(mapping->hasfile);

    void *kpage = palloc_get_page(PAL_USER);
    if (kpage == NULL) {
        return NULL;
    }

    file_t *file = mapping->data.file_info.file;
    off_t offset = mapping->data.file_info.offset * PGSIZE;
    size_t size = mapping->data.file_info.size + 1;
    file_seek(file, offset);
    off_t eof = file_read(file, kpage, size);

    memset(kpage + eof, 0, PGSIZE - eof); // fill the rest with zeros
    return kpage;
}

/*! Loads and populates a frame from swap. Return NULL on failure, otherwise
    the frame. */
static void *load_swap_page(vm_mapping_t *mapping) {
    ASSERT(mapping != NULL);
    ASSERT(mapping->swapped);

    void *kpage = palloc_get_page(PAL_USER);
    if (kpage == NULL) {
        return NULL;
    }
    swaptbl_load(kpage, mapping->data.swap_slot);
    return kpage;
}

/*! Helper for vm_load_page which optionally allows not acquiring the page lock.
    If called with should_lock false, the page lock should already be held by
    the caller. */
static frame_t *_load_page(sup_pagetable_t *pt, void *upage, bool should_lock) {
    ASSERT(!pg_ofs(upage));
    ASSERT(is_user_vaddr(upage));

    vm_mapping_t *mapping = mapping_lookup(pt, upage);
    ASSERT(mapping != NULL);

    if (should_lock) bin_sema_down(&mapping->lock);
    void *kpage;
    if (mapping->hasfile) {
        kpage = load_file_page(mapping);
    } else if (mapping->swapped) {
        kpage = load_swap_page(mapping);
    } else { // no file and not in swap, so get a zero page
        kpage = load_anonymous_page();
    }

    if (kpage == NULL) {
        return NULL;
    }

    if (!pagedir_set_page(pt->pd, upage, kpage, mapping->writable)) {
        palloc_free_page(kpage);
        return NULL;
    }
    mapping->present = true;
    mapping->frame = kpage;
    frametbl_install_page(mapping, kpage);
    if (should_lock) bin_sema_up(&mapping->lock);
    return kpage;
}

/*! Loads a frame for UPAGE with the correct information in it, using the
    information in PT. Requires that UPAGE does not already have a frame.
    Returns the frame, pinned (which the user must unpin), or NULL on
    failure. 
    */
frame_t *vm_load_page(sup_pagetable_t *pt, void *upage) {
    return _load_page(pt, upage, true);
}

/*! Writes a mapping to a file. */
static void evict_to_file(vm_mapping_t *mapping) {
    ASSERT(mapping != NULL);

    file_t *file = mapping->data.file_info.file;
    off_t offset = mapping->data.file_info.offset * PGSIZE;
    size_t size = mapping->data.file_info.size + 1;
    frame_t *frame = mapping->frame;
    file_seek(file, offset);
    file_write(file, frame->bytes, size);
}

/*! Instructs the supplemental PT to evict a user-space page UPAGE from its
    frame. Requires that the page is pinned and frees it. */
void vm_evict_page(vm_mapping_t *mapping) {
    if (mapping == NULL) return;
    ASSERT(mapping->present);
    
    frame_t *frame = mapping->frame;
    bin_sema_down(&mapping->lock);
    if (mapping->orphaned) {
        mapping_free(mapping);
        return;
    }

    mapping->present = false;
    bool is_dirty = pagedir_is_dirty(mapping->pt->pd, mapping->page);
    pagedir_clear_page(mapping->pt->pd, mapping->page);

    if (!is_dirty && !mapping->swapped) goto exit; // clean page, no need to be saved

    // the page is dirty, we need to save it
    if (mapping->hasfile) {
        if (!mapping->fwrite) { // can't write to file, swap
            mapping->fwrite = mapping->hasfile = false;
            goto swap;
        }
        // write back to file
        evict_to_file(mapping);
    } else {
        swap:
        mapping->swapped = true;
        mapping->data.swap_slot = swaptbl_store(frame);
    }

    exit:
    palloc_free_page(frame);
    bin_sema_up(&mapping->lock);
}

/*! Clears out a page from the supplemental page table, making it no longer
    accessible and flushing it to disk if it's backed by a file. Wipes the
    supplemental page table entry. */
void vm_clear_page(sup_pagetable_t *pt, void *upage) {
    ASSERT(pg_ofs(upage) == 0);
    ASSERT(is_user_vaddr(upage));

    pagedir_clear_page(pt->pd, upage);
    mapping_delete(pt, upage);
}

/*! Resets the accessed bit of a given page table entry and returns the original
    value. */
bool vm_reset_accessed(vm_mapping_t *mapping) {
    if (mapping == NULL) return false;
    if (mapping->orphaned) return false;
    bin_sema_down(&mapping->lock);
    bool a = pagedir_is_accessed(mapping->pt->pd, mapping->page);
    pagedir_set_accessed(mapping->pt->pd, mapping->page, false);
    bin_sema_up(&mapping->lock);
    return a;
}

/*! Tries to resets the accessed bit of a given page table entry and returns the
    original value. Returns -1 if the page is locked. */
int vm_try_reset_accessed(vm_mapping_t *mapping) {
    if (mapping == NULL) return false;
    if (mapping->orphaned) return false;
    if (!bin_sema_try_down(&mapping->lock)) return -1;
    bool a = pagedir_is_accessed(mapping->pt->pd, mapping->page);
    pagedir_set_accessed(mapping->pt->pd, mapping->page, false);
    bin_sema_up(&mapping->lock);
    return a;
}

/*! Pins n pages following upages, loading them into memory first if necessary.
    Assumes that the pages are in the supplemental table and unpinned. */
void vm_pin_pages(sup_pagetable_t *pt, const void *upages, size_t n) {
    for (size_t i = 0; i < n; i++) {
        void *upage = (void *) upages + i * PGSIZE;
        vm_mapping_t *mapping = mapping_lookup(pt, upage);
        ASSERT(mapping != NULL);
        bin_sema_down(&mapping->lock);
        void *kpage = pagedir_get_page(pt->pd, upage);
        if (kpage == NULL) {
            _load_page(pt, upage, false);
            bin_sema_up(&mapping->lock);
            // vm_load_page automatically pins the page, so we can move on.
            continue;
        }
        ASSERT(frametbl_try_pin_frame(kpage));
        bin_sema_up(&mapping->lock);
    }
}

/*! Unpins n pages, following upages.
    Assumes that the pages are in the supplemental table and pinned. */
void vm_unpin_pages(sup_pagetable_t *pt, const void *upages, size_t n) {
    for (size_t i = 0; i < n; i++) {
        void *upage = (void *) upages + i * PGSIZE;
        ASSERT(vm_page_is_mapped(pt, upage));
        frametbl_unpin_frame(pagedir_get_page(pt->pd, upage));
    }
}

/*! Gets the map entry from the embedded hash elem. */
static vm_mapping_t *map_entry(const hash_elem_t *a) {
    return hash_entry(a, vm_mapping_t, elem);
}
/*! Gets the virtual address from the element in the mapping. */
static void *map_addr(const hash_elem_t *a) {
    return map_entry(a)->page;
}
/*! Computes the hash of a mapping element. */
static unsigned mapping_hash(const hash_elem_t *a, void *aux UNUSED) {
    return hash_int((int) map_addr(a));
}
/*! Provides a total order on mapping element. */
static bool mapping_less(const hash_elem_t *a, const hash_elem_t *b,
                         void *aux UNUSED) {
    return map_addr(a) < map_addr(b);
}

/*! Frees all resources associated with a mapping. Must be called while holding
    mapping->lock. */
static void mapping_free(vm_mapping_t *mapping) {
    ASSERT(mapping != NULL);
    if (mapping->fwrite) {
        file_close(mapping->data.file_info.file);
    } 
    if (mapping->swapped && !mapping->present) {
        swaptbl_load(NULL, mapping->data.swap_slot);
    }
    if (mapping->present) {
        palloc_free_page(mapping->frame);
    }
    bin_sema_up(&mapping->lock);
    free(mapping);
}

/*! Releases a mapping by its hash element. If it has no physical memory,
    mapping_free is invoked immediately. If it has a physical frame, it is
    orphaned and mapping free is invoked when the frame is evicted. */
static void mapping_destroy(hash_elem_t *a, void *aux UNUSED) {
    vm_mapping_t *mapping = map_entry(a);
    if (mapping == NULL) return;
    bin_sema_down(&mapping->lock);
    if (mapping->present) {
        mapping->orphaned = true;
        // flush the contents of the file, but leave freeing of allocated
        // resources to the next eviction.
        if (mapping->fwrite 
            && pagedir_is_dirty(mapping->pt->pd, mapping->page)) {
            evict_to_file(mapping);
        }
        bin_sema_up(&mapping->lock);
    } else {
        // the page is not present or not backed by a writable file, so its
        // contents can be discarded
        mapping_free(mapping);
    }
}

/*! Gets a vm_mapping_t struct for the given page table and address. */
static vm_mapping_t *mapping_lookup(sup_pagetable_t *pt, const void *addr) {
    vm_mapping_t lookup = {.page = (void *) addr};
    hash_elem_t *elem = hash_find(&pt->mappings, &lookup.elem);
    if (elem == NULL) {
        return NULL;
    }
    return map_entry(elem);
}

/*! Removes a mapping from page tables hash map and invokes mapping_destroy. */
static void mapping_delete(sup_pagetable_t *pt, const void *addr) {
    vm_mapping_t lookup = {.page = (void *) addr};
    hash_elem_t *elem = hash_delete(&pt->mappings, &lookup.elem);
    ASSERT(elem != NULL);
    mapping_destroy(elem, NULL);
}