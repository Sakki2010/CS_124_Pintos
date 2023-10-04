#include <debug.h>
#include <string.h>
#include <stdio.h>

#include "threads/malloc.h"
#include "filemap.h"

// Shut up the VS Code warnings from it not grabbing the right version of
// stdio.h.
#ifndef NUM_RESERVED_FDS
#define NUM_RESERVED_FDS (PANIC("#include <stdio.h> found wrong file!\n"), 0)
#endif

/*! Struct for storing files in the overflow list. */
typedef struct {
    void *f;                /*!< File to store. */
    bool is_dir;            /*!< True for directory, false for ordinary file. */
    list_elem_t elem;       /*!< Intrusive list element. */
    uint32_t fi;            /*!< The file index, which is the file descriptor
                                 minus the number of reserved file descriptors. */
} file_elem_t;

/*! Initializes an empty filemap in a buffer. The buffer is assumed to have size
    sizeof(file_map_t). */
void filemap_init(file_map_t *fm) {
    list_init(&fm->overflow);
    fm->first_open = 0;
    for (size_t i = 0; i < NUM_QUICK_FILES; i++) {
        fm->quick[i] = NULL;
        fm->flags[i].is_dir = false;
    }
}

/*! Helper to get the first open spot in the file map after start.
    Helper for filemap_insert. */
static uint32_t get_next_open(file_map_t *fm, uint32_t start) {
    for (uint32_t i = start + 1; i < NUM_QUICK_FILES; i++) {
        if (fm->quick[i] == NULL) return i;
    }
    uint32_t prev = NUM_QUICK_FILES - 1;
    for (list_elem_t *e = list_begin(&fm->overflow);
         e != list_end(&fm->overflow); e = list_next(e)) {
        uint32_t cur = list_entry(e, file_elem_t, elem)->fi;
        if (cur != prev + 1) {
            return prev + 1;
        }
        prev = cur;
    }
    return prev + 1;
}

/*! Compares the file indices of two list elements belonging to file_elem_t and
    returns whether the first is less. Helper for filemap_insert. */
static bool fi_less(const list_elem_t *a, const list_elem_t *b,
    void *aux UNUSED) {
    return list_entry(a, file_elem_t, elem)->fi <
            list_entry(b, file_elem_t, elem)->fi;
}

/*! Inserts a file into a file map and returns its assigned file
    descriptor. Returns FM_ERROR on failure, which should only occur if memory
    allocation fails. */
uint32_t filemap_insert(file_map_t *fm, void *f, bool is_dir) {
    uint32_t i = fm->first_open;
    if (i < NUM_QUICK_FILES) {
        fm->quick[i] = f;
        fm->flags[i].is_dir = is_dir;
    } else {
        file_elem_t *fe = malloc(sizeof(file_elem_t));
        if (fe == NULL) {
            return FM_ERROR;
        }
        fe->fi = i;
        fe->f = f;
        fe->is_dir = is_dir;
        list_insert_ordered(&fm->overflow, &fe->elem, fi_less, NULL);
    }
    fm->first_open = get_next_open(fm, i);
    return i + NUM_RESERVED_FDS;
}

/*! Gets the file associated by the map with a given file descriptor.
    If the file descriptor is not in the map, is a reserved file descriptor,
    or is a directory, it returns NULL and does nothing.
    On success, sets *IS_DIR to whether or not it's a directory. */
void *filemap_get(file_map_t *fm, uint32_t fd, bool *is_dir) {
    if (fd < NUM_RESERVED_FDS) {
        return NULL;
    }
    uint32_t i = fd - NUM_RESERVED_FDS;
    if (i < NUM_QUICK_FILES) {
        *is_dir = fm->flags[i].is_dir;
        return fm->quick[i];
    }
    for (list_elem_t *e = list_begin(&fm->overflow);
         e != list_end(&fm->overflow); e = list_next(e)) {
        file_elem_t *fe = list_entry(e, file_elem_t, elem);
        if (fe->fi == i) {
            *is_dir = fe->is_dir;
            return fe->f;
        }
    }
    return NULL;
}

/*! Removes the file associated by the map with a given file descriptor
    and returns the removed file.
    If the file descriptor is not in the map, is a reserved file descriptor,
    or is a directory, it returns NULL and does nothing.
    On success, also sets *IS_DIR to whether the file was a directory
    or ordinary file. */
void *filemap_remove(file_map_t *fm, uint32_t fd, bool *is_dir) {
    if (fd < NUM_RESERVED_FDS) {
        return NULL;
    }
    uint32_t i = fd - NUM_RESERVED_FDS;
    void *f = NULL;
    if (i < NUM_QUICK_FILES) {
        *is_dir = fm->flags[i].is_dir;
        f = fm->quick[i];
        fm->quick[i] = NULL;
    } else {
        for (list_elem_t *e = list_begin(&fm->overflow);
            e != list_end(&fm->overflow); e = list_next(e)) {
            file_elem_t *fe = list_entry(e, file_elem_t, elem);
            if (fe->fi == i) {
                list_remove(e);
                *is_dir = fe->is_dir;
                f = fe->f;
                free(fe);
                break;
            }
        }
    }
    if (fm->first_open > i) {
        fm->first_open = i;
    }
    return f;
}

/*! Returns true if the map associates the given file descriptor with 
    a directory; returns false if it associates it with an ordinary file,
    or if it's not in the map or is a reserved file descriptor. */
bool filemap_is_dir(file_map_t *fm, uint32_t fd) {
    if (fd < NUM_RESERVED_FDS) {
        return false;
    }
    uint32_t i = fd - NUM_RESERVED_FDS;
    if (i < NUM_QUICK_FILES) {
        if (fm->quick[i] == NULL) {
            return false;
        }
        return fm->flags[i].is_dir;
    }
    for (list_elem_t *e = list_begin(&fm->overflow);
         e != list_end(&fm->overflow); list_next(e)) {
        file_elem_t *fe = list_entry(e, file_elem_t, elem);
        if (fe->fi == i) {
            return fe->is_dir;
        }
    }
    return false;
}

/*! Calls the given file_action on each ordinary file in the map,
    and the given dir_action on each directory in the map. */
void filemap_foreach(file_map_t *fm, fm_file_action_func file_action,
    fm_dir_action_func dir_action, void *aux) {
    for (uint32_t i = 0; i < NUM_QUICK_FILES; i++) {
        if (fm->quick[i] != NULL) {
            if (fm->flags[i].is_dir && dir_action != NULL) {
                dir_action((dir_t *) fm->quick[i], aux);
            }
            else if (file_action != NULL) {
                file_action((file_t *) fm->quick[i], aux);
            }
        }
    }
    for (list_elem_t *e = list_begin(&fm->overflow);
         e != list_end(&fm->overflow); list_next(e)) {
        file_elem_t *fe = list_entry(e, file_elem_t, elem);
        if (fe->is_dir && dir_action != NULL) {
            dir_action((dir_t *) fe->f, aux);
        } else if (file_action != NULL) {
            file_action((file_t *) fe->f, aux);
        }
    }
}

/*! Destroys a filemap, freeing all allocated memomory and calling
    file_destructor(file, aux) for each ordinary file and dir_destructor(dir,
    aux) for each directory in the map.

    Does NOT call free on the fm pointer, because the file map is allocated into
    a buffer and so the owner of the buffer is responsible for handling it. */
void filemap_destroy(file_map_t *fm, fm_file_action_func file_destructor,
        fm_dir_action_func dir_destructor, void *aux) {
    for (uint32_t i = 0; i < NUM_QUICK_FILES; i++) {
        if (fm->quick[i] != NULL) {
            if (fm->flags[i].is_dir && dir_destructor != NULL) {
                dir_destructor((dir_t *) fm->quick[i], aux);
            }
            else if (file_destructor != NULL) {
                file_destructor((file_t *) fm->quick[i], aux);   
            }
        }
    }
    while (!list_empty(&fm->overflow)) {
        list_elem_t *e = list_pop_front(&fm->overflow);
        file_elem_t *fe = list_entry(e, file_elem_t, elem);
        if (fe->is_dir && dir_destructor != NULL) {
            dir_destructor((dir_t *) fe->f, aux);
        } else if (file_destructor != NULL) {
            file_destructor((file_t *) fe->f, aux);
        }
        free(fe);
    }
}