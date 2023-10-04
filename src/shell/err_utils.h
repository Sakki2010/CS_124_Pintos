#ifndef SHELL_ERR_UTILS_H
#define SHELL_ERR_UTILS_H

#include <stdio.h>
#include <stdlib.h>

/**
 * @brief Reports a fatal error and exits. Has type void *.
 */
#define FATAL_ERROR(args...) \
    (fprintf(stderr, "fatal error: "args), exit(1))

/**
 * @brief Asserts that the given pointer is non-NULL and, if it isn't, prints
 * an appropriate fatal error. 
 * 
 * We treat being out of memory as an unrecoverable errror.
 * 
 * @param p The pointer to check
 * @return The pointer, if it's not NULL
 */

void *checked_ptr(void *p);

/**
 * @brief A wrapper for malloc which ensures that the return value is always a
 * successful allocation. On failure, exits the program.
 */
void *checked_malloc(size_t size);

/**
 * @brief A wrapper for realloc which ensures that the return value is always a
 * successful allocation. On failure, exits the program.
 */
void *checked_realloc(void *old_ptr, size_t size);

/**
 * @brief A wrapper for calloc which ensures that the return value is always a
 * successful allocation. On failure, exits the program.
 */
void *checked_calloc(size_t count, size_t size);

#endif