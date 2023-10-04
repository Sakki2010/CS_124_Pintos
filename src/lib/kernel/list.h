#ifndef __LIB_KERNEL_LIST_H
#define __LIB_KERNEL_LIST_H

/* Doubly linked list.

   This implementation of a doubly linked list does not require
   use of dynamically allocated memory.  Instead, each structure
   that is a potential list element must embed a list_elem_t
   member.  All of the list functions operate on these `struct
   list_elem's.  The list_entry macro allows conversion from a
   list_elem_t back to a structure object that contains it.

   For example, suppose there is a needed for a list of `struct
   foo'.  `struct foo' should contain a `list_elem_t'
   member, like so:

      struct foo
        {
          list_elem_t elem;
          int bar;
          ...other members...
        };

   Then a list of `struct foo' can be be declared and initialized
   like so:

      list_t foo_list;

      list_init (&foo_list);

   Iteration is a typical situation where it is necessary to
   convert from a list_elem_t back to its enclosing
   structure.  Here's an example using foo_list:

      list_elem_t *e;

      for (e = list_begin (&foo_list); e != list_end (&foo_list);
           e = list_next (e))
        {
          struct foo *f = list_entry (e, struct foo, elem);
          ...do something with f...
        }

   You can find real examples of list usage throughout the
   source; for example, malloc.c, palloc.c, and thread.c in the
   threads directory all use lists.

   The interface for this list is inspired by the list<> template
   in the C++ STL.  If you're familiar with list<>, you should
   find this easy to use.  However, it should be emphasized that
   these lists do *no* type checking and can't do much other
   correctness checking.  If you screw up, it will bite you.

   Glossary of list terms:

     - "front": The first element in a list.  Undefined in an
       empty list.  Returned by list_front().

     - "back": The last element in a list.  Undefined in an empty
       list.  Returned by list_back().

     - "tail": The element figuratively just after the last
       element of a list.  Well defined even in an empty list.
       Returned by list_end().  Used as the end sentinel for an
       iteration from front to back.

     - "beginning": In a non-empty list, the front.  In an empty
       list, the tail.  Returned by list_begin().  Used as the
       starting point for an iteration from front to back.

     - "head": The element figuratively just before the first
       element of a list.  Well defined even in an empty list.
       Returned by list_rend().  Used as the end sentinel for an
       iteration from back to front.

     - "reverse beginning": In a non-empty list, the back.  In an
       empty list, the head.  Returned by list_rbegin().  Used as
       the starting point for an iteration from back to front.

     - "interior element": An element that is not the head or
       tail, that is, a real list element.  An empty list does
       not have any interior elements.
*/

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* List element. */
typedef struct list_elem list_elem_t;
typedef struct list_elem {
    list_elem_t *prev;     /* Previous list element. */
    list_elem_t *next;     /* Next list element. */
} list_elem_t;

/* List. */
typedef struct list {
    list_elem_t head;      /* List head. */
    list_elem_t tail;      /* List tail. */
} list_t;

/* Converts pointer to list element LIST_ELEM into a pointer to
   the structure that LIST_ELEM is embedded inside.  Supply the
   name of the outer structure STRUCT and the member name MEMBER
   of the list element.  See the big comment at the top of the
   file for an example. */
#define list_entry(LIST_ELEM, STRUCT, MEMBER)           \
        ((STRUCT *) ((uint8_t *) &(LIST_ELEM)->next     \
                     - offsetof (STRUCT, MEMBER.next)))

/* List initialization.

   A list may be initialized by calling list_init():

       list_t my_list;
       list_init (&my_list);

   or with an initializer using LIST_INITIALIZER:

       list_t my_list = LIST_INITIALIZER (my_list); */
#define LIST_INITIALIZER(NAME) { { NULL, &(NAME).tail }, \
                                 { &(NAME).head, NULL } }

void list_init (list_t *);

/* List traversal. */
list_elem_t *list_begin (list_t *);
list_elem_t *list_next (list_elem_t *);
list_elem_t *list_end (list_t *);

list_elem_t *list_rbegin (list_t *);
list_elem_t *list_prev (list_elem_t *);
list_elem_t *list_rend (list_t *);

list_elem_t *list_head (list_t *);
list_elem_t *list_tail (list_t *);

/* List insertion. */
void list_insert (list_elem_t *, list_elem_t *);
void list_splice (list_elem_t *before,
                  list_elem_t *first, list_elem_t *last);
void list_push_front (list_t *, list_elem_t *);
void list_push_back (list_t *, list_elem_t *);

/* List removal. */
list_elem_t *list_remove (list_elem_t *);
list_elem_t *list_pop_front (list_t *);
list_elem_t *list_pop_back (list_t *);

/* List elements. */
list_elem_t *list_front (list_t *);
list_elem_t *list_back (list_t *);

/* List properties. */
size_t list_size (list_t *);
bool list_empty (list_t *);

/* Miscellaneous. */
void list_reverse (list_t *);

/* Compares the value of two list elements A and B, given
   auxiliary data AUX.  Returns true if A is less than B, or
   false if A is greater than or equal to B. */
typedef bool list_less_func (const list_elem_t *a,
                             const list_elem_t *b,
                             void *aux);

/* Operations on lists with ordered elements. */
void list_sort (list_t *,
                list_less_func *, void *aux);
void list_insert_ordered (list_t *, list_elem_t *,
                          list_less_func *, void *aux);
void list_unique (list_t *, list_t *duplicates,
                  list_less_func *, void *aux);

/* Max and min. */
list_elem_t *list_max (list_t *, list_less_func *, void *aux);
list_elem_t *list_min (list_t *, list_less_func *, void *aux);
list_elem_t *list_pop_max (list_t *, list_less_func *, void *aux);
list_elem_t *list_pop_min (list_t *, list_less_func *, void *aux);

#endif /* lib/kernel/list.h */
