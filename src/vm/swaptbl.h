#ifndef VM_SWAPTBL_H
#define VM_SWAPTBL_H

#include <inttypes.h>

void swaptbl_init(void);

uintptr_t swaptbl_store(void *page);
void swaptbl_load(void *page, uintptr_t swapidx);

#endif