#include "err_utils.h"

void *checked_ptr(void *p) {
    if (p == NULL) {
        FATAL_ERROR("memory alloc failed, pointer returned was NULL.\n");
    }
    return p;
}

void *checked_malloc(size_t size) {
    return checked_ptr(malloc(size));
}

void *checked_realloc(void *old_ptr, size_t size) {
    return checked_ptr(realloc(old_ptr, size));
}

void *checked_calloc(size_t count, size_t size) {
    return checked_ptr(calloc(count, size));
}