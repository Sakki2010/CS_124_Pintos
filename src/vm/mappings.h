#ifndef VM_MAPPINGS_H
#define VM_MAPPINGS_H

#include <stdbool.h>
#include <stdint.h>
#include <hash.h>
#include "filesys/file.h"

#define MAP_WRITE 0x1
#define MAP_FWRITE 0x2
#define MAP_START 0x4
#define MAP_STACK 0x8

/*! Supplemental page directory. Represents a user process's view of memory:
    when a page fault occurs, the handler consults the supplemental page
    directory to determine how to handle it. */
typedef struct sup_pagetable {
    bool user;          /*!< Boolean flag indicating whether to not use the
                                 default kernel page table. */
    uint32_t *pd;       /*!< The hardware page directory, augmented using
                                 the available free bits. */
    hash_t mappings;    /*!< Stores a mapping between addresses and files
                                 for memory mapped files. */
} sup_pagetable_t;

typedef struct vm_mapping vm_mapping_t;

bool sup_pt_create(sup_pagetable_t *pt);
void sup_pt_destroy(sup_pagetable_t *pt);
void sup_pt_activate(sup_pagetable_t *pt);
bool sup_pt_is_kernel(sup_pagetable_t *pt);

bool vm_page_is_mapped(sup_pagetable_t *pt, const void *upage);
bool vm_page_is_writeable(sup_pagetable_t *pt, const void *upage);
bool vm_page_is_mappable(sup_pagetable_t *pt, const void *upage);
bool vm_page_is_stack(sup_pagetable_t *pt, const void *upage);
bool vm_page_is_mapping_start(sup_pagetable_t *pt, const void *upage);
void *vm_page_get_mapping_end(sup_pagetable_t *pt, const void *upage);

bool vm_set_page(sup_pagetable_t *pt, void *upage, uint32_t flags,
                     file_t *, off_t, size_t);
bool vm_set_stack_page(sup_pagetable_t *pt, void *upage);
struct frame *vm_load_page(sup_pagetable_t *pt, void *upage);
struct frame *vm_set_load_stack_page(sup_pagetable_t *pt, void *upage);
void vm_evict_page(vm_mapping_t *);
void vm_clear_page(sup_pagetable_t *pt, void *upage);

void vm_pin_pages(sup_pagetable_t *pt, const void *upages, size_t n);
void vm_unpin_pages(sup_pagetable_t *pt, const void *upages, size_t n);

bool vm_reset_accessed(vm_mapping_t *);
int vm_try_reset_accessed(vm_mapping_t *mapping);

#endif /* userprog/pagedir.h */

