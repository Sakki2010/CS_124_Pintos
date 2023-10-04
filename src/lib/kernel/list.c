#include "list.h"
#include "../debug.h"

/* Our doubly linked lists have two header elements: the "head"
     just before the first element and the "tail" just after the
     last element.  The `prev' link of the front header is null, as
     is the `next' link of the back header.  Their other two links
     point toward each other via the interior elements of the list.

     An empty list looks like this:

                                            +------+     +------+
                                    <---| head |<--->| tail |--->
                                            +------+     +------+

     A list with two elements in it looks like this:

                +------+     +-------+     +-------+     +------+
        <---| head |<--->|   1   |<--->|   2   |<--->| tail |<--->
                +------+     +-------+     +-------+     +------+

     The symmetry of this arrangement eliminates lots of special
     cases in list processing.  For example, take a look at
     list_remove(): it takes only two pointer assignments and no
     conditionals.  That's a lot simpler than the code would be
     without header elements.

     (Because only one of the pointers in each header element is used,
     we could in fact combine them into a single header element
     without sacrificing this simplicity.  But using two separate
     elements allows us to do a little bit of checking on some
     operations, which can be valuable.) */

static bool is_sorted (list_elem_t *a, list_elem_t *b,
                                             list_less_func *less, void *aux) UNUSED;

/* Returns true if ELEM is a head, false otherwise. */
static inline bool
is_head (list_elem_t *elem)
{
    return elem != NULL && elem->prev == NULL && elem->next != NULL;
}

/* Returns true if ELEM is an interior element,
     false otherwise. */
static inline bool
is_interior (list_elem_t *elem)
{
    return elem != NULL && elem->prev != NULL && elem->next != NULL;
}

/* Returns true if ELEM is a tail, false otherwise. */
static inline bool
is_tail (list_elem_t *elem)
{
    return elem != NULL && elem->prev != NULL && elem->next == NULL;
}

/* Initializes LIST as an empty list. */
void
list_init (list_t *list)
{
    ASSERT (list != NULL);
    list->head.prev = NULL;
    list->head.next = &list->tail;
    list->tail.prev = &list->head;
    list->tail.next = NULL;
}

/* Returns the beginning of LIST.  */
list_elem_t *
list_begin (list_t *list)
{
    ASSERT (list != NULL);
    return list->head.next;
}

/* Returns the element after ELEM in its list.  If ELEM is the
     last element in its list, returns the list tail.  Results are
     undefined if ELEM is itself a list tail. */
list_elem_t *
list_next (list_elem_t *elem)
{
    ASSERT (is_head (elem) || is_interior (elem));
    return elem->next;
}

/* Returns LIST's tail.

     list_end() is often used in iterating through a list from
     front to back.  See the big comment at the top of list.h for
     an example. */
list_elem_t *
list_end (list_t *list)
{
    ASSERT (list != NULL);
    return &list->tail;
}

/* Returns the LIST's reverse beginning, for iterating through
     LIST in reverse order, from back to front. */
list_elem_t *
list_rbegin (list_t *list) 
{
    ASSERT (list != NULL);
    return list->tail.prev;
}

/* Returns the element before ELEM in its list.  If ELEM is the
     first element in its list, returns the list head.  Results are
     undefined if ELEM is itself a list head. */
list_elem_t *
list_prev (list_elem_t *elem)
{
    ASSERT (is_interior (elem) || is_tail (elem));
    return elem->prev;
}

/* Returns LIST's head.

     list_rend() is often used in iterating through a list in
     reverse order, from back to front.  Here's typical usage,
     following the example from the top of list.h:

            for (e = list_rbegin (&foo_list); e != list_rend (&foo_list);
                     e = list_prev (e))
                {
                    struct foo *f = list_entry (e, struct foo, elem);
                    ...do something with f...
                }
*/
list_elem_t *
list_rend (list_t *list) 
{
    ASSERT (list != NULL);
    return &list->head;
}

/* Return's LIST's head.

     list_head() can be used for an alternate style of iterating
     through a list, e.g.:

            e = list_head (&list);
            while ((e = list_next (e)) != list_end (&list)) 
                {
                    ...
                }
*/
list_elem_t *
list_head (list_t *list) 
{
    ASSERT (list != NULL);
    return &list->head;
}

/* Return's LIST's tail. */
list_elem_t *
list_tail (list_t *list) 
{
    ASSERT (list != NULL);
    return &list->tail;
}

/* Inserts ELEM just before BEFORE, which may be either an
     interior element or a tail.  The latter case is equivalent to
     list_push_back(). */
void
list_insert (list_elem_t *before, list_elem_t *elem)
{
    ASSERT (is_interior (before) || is_tail (before));
    ASSERT (elem != NULL);

    elem->prev = before->prev;
    elem->next = before;
    before->prev->next = elem;
    before->prev = elem;
}

/* Removes elements FIRST though LAST (exclusive) from their
     current list, then inserts them just before BEFORE, which may
     be either an interior element or a tail. */
void
list_splice (list_elem_t *before,
                         list_elem_t *first, list_elem_t *last)
{
    ASSERT (is_interior (before) || is_tail (before));
    if (first == last)
        return;
    last = list_prev (last);

    ASSERT (is_interior (first));
    ASSERT (is_interior (last));

    /* Cleanly remove FIRST...LAST from its current list. */
    first->prev->next = last->next;
    last->next->prev = first->prev;

    /* Splice FIRST...LAST into new list. */
    first->prev = before->prev;
    last->next = before;
    before->prev->next = first;
    before->prev = last;
}

/* Inserts ELEM at the beginning of LIST, so that it becomes the
     front in LIST. */
void
list_push_front (list_t *list, list_elem_t *elem)
{
    list_insert (list_begin (list), elem);
}

/* Inserts ELEM at the end of LIST, so that it becomes the
     back in LIST. */
void
list_push_back (list_t *list, list_elem_t *elem)
{
    list_insert (list_end (list), elem);
}

/* Removes ELEM from its list and returns the element that
     followed it.  Undefined behavior if ELEM is not in a list.

     A list element must be treated very carefully after removing
     it from its list.  Calling list_next() or list_prev() on ELEM
     will return the item that was previously before or after ELEM,
     but, e.g., list_prev(list_next(ELEM)) is no longer ELEM!

     The list_remove() return value provides a convenient way to
     iterate and remove elements from a list:

     for (e = list_begin (&list); e != list_end (&list); e = list_remove (e))
         {
             ...do something with e...
         }

     If you need to free() elements of the list then you need to be
     more conservative.  Here's an alternate strategy that works
     even in that case:

     while (!list_empty (&list))
         {
             list_elem_t *e = list_pop_front (&list);
             ...do something with e...
         }
*/
list_elem_t *
list_remove (list_elem_t *elem)
{
    ASSERT (is_interior (elem));
    elem->prev->next = elem->next;
    elem->next->prev = elem->prev;
    return elem->next;
}

/* Removes the front element from LIST and returns it.
     Undefined behavior if LIST is empty before removal. */
list_elem_t *
list_pop_front (list_t *list)
{
    list_elem_t *front = list_front (list);
    list_remove (front);
    return front;
}

/* Removes the back element from LIST and returns it.
     Undefined behavior if LIST is empty before removal. */
list_elem_t *
list_pop_back (list_t *list)
{
    list_elem_t *back = list_back (list);
    list_remove (back);
    return back;
}

/* Returns the front element in LIST.
     Undefined behavior if LIST is empty. */
list_elem_t *
list_front (list_t *list)
{
    ASSERT (!list_empty (list));
    return list->head.next;
}

/* Returns the back element in LIST.
     Undefined behavior if LIST is empty. */
list_elem_t *
list_back (list_t *list)
{
    ASSERT (!list_empty (list));
    return list->tail.prev;
}

/* Returns the number of elements in LIST.
     Runs in O(n) in the number of elements. */
size_t
list_size (list_t *list)
{
    list_elem_t *e;
    size_t cnt = 0;

    for (e = list_begin (list); e != list_end (list); e = list_next (e))
        cnt++;
    return cnt;
}

/* Returns true if LIST is empty, false otherwise. */
bool
list_empty (list_t *list)
{
    return list_begin (list) == list_end (list);
}

/* Swaps the `list_elem_t *'s that A and B point to. */
static void
swap (list_elem_t **a, list_elem_t **b) 
{
    list_elem_t *t = *a;
    *a = *b;
    *b = t;
}

/* Reverses the order of LIST. */
void
list_reverse (list_t *list)
{
    if (!list_empty (list)) 
        {
            list_elem_t *e;

            for (e = list_begin (list); e != list_end (list); e = e->prev)
                swap (&e->prev, &e->next);
            swap (&list->head.next, &list->tail.prev);
            swap (&list->head.next->prev, &list->tail.prev->next);
        }
}

/* Returns true only if the list elements A through B (exclusive)
     are in order according to LESS given auxiliary data AUX. */
static bool
is_sorted (list_elem_t *a, list_elem_t *b,
                     list_less_func *less, void *aux)
{
    if (a != b)
        while ((a = list_next (a)) != b) 
            if (less (a, list_prev (a), aux))
                return false;
    return true;
}

/* Finds a run, starting at A and ending not after B, of list
     elements that are in nondecreasing order according to LESS
     given auxiliary data AUX.  Returns the (exclusive) end of the
     run.
     A through B (exclusive) must form a non-empty range. */
static list_elem_t *
find_end_of_run (list_elem_t *a, list_elem_t *b,
                                 list_less_func *less, void *aux)
{
    ASSERT (a != NULL);
    ASSERT (b != NULL);
    ASSERT (less != NULL);
    ASSERT (a != b);
    
    do 
        {
            a = list_next (a);
        }
    while (a != b && !less (a, list_prev (a), aux));
    return a;
}

/* Merges A0 through A1B0 (exclusive) with A1B0 through B1
     (exclusive) to form a combined range also ending at B1
     (exclusive).  Both input ranges must be nonempty and sorted in
     nondecreasing order according to LESS given auxiliary data
     AUX.  The output range will be sorted the same way. */
static void
inplace_merge (list_elem_t *a0, list_elem_t *a1b0,
                             list_elem_t *b1,
                             list_less_func *less, void *aux)
{
    ASSERT (a0 != NULL);
    ASSERT (a1b0 != NULL);
    ASSERT (b1 != NULL);
    ASSERT (less != NULL);
    ASSERT (is_sorted (a0, a1b0, less, aux));
    ASSERT (is_sorted (a1b0, b1, less, aux));

    while (a0 != a1b0 && a1b0 != b1)
        if (!less (a1b0, a0, aux)) 
            a0 = list_next (a0);
        else 
            {
                a1b0 = list_next (a1b0);
                list_splice (a0, list_prev (a1b0), a1b0);
            }
}

/* Sorts LIST according to LESS given auxiliary data AUX, using a
     natural iterative merge sort that runs in O(n lg n) time and
     O(1) space in the number of elements in LIST. */
void
list_sort (list_t *list, list_less_func *less, void *aux)
{
    size_t output_run_cnt;        /* Number of runs output in current pass. */

    ASSERT (list != NULL);
    ASSERT (less != NULL);

    /* Pass over the list repeatedly, merging adjacent runs of
         nondecreasing elements, until only one run is left. */
    do
        {
            list_elem_t *a0;     /* Start of first run. */
            list_elem_t *a1b0;   /* End of first run, start of second. */
            list_elem_t *b1;     /* End of second run. */

            output_run_cnt = 0;
            for (a0 = list_begin (list); a0 != list_end (list); a0 = b1)
                {
                    /* Each iteration produces one output run. */
                    output_run_cnt++;

                    /* Locate two adjacent runs of nondecreasing elements
                         A0...A1B0 and A1B0...B1. */
                    a1b0 = find_end_of_run (a0, list_end (list), less, aux);
                    if (a1b0 == list_end (list))
                        break;
                    b1 = find_end_of_run (a1b0, list_end (list), less, aux);

                    /* Merge the runs. */
                    inplace_merge (a0, a1b0, b1, less, aux);
                }
        }
    while (output_run_cnt > 1);

    ASSERT (is_sorted (list_begin (list), list_end (list), less, aux));
}

/* Inserts ELEM in the proper position in LIST, which must be
     sorted according to LESS given auxiliary data AUX.
     Runs in O(n) average case in the number of elements in LIST. */
void
list_insert_ordered (list_t *list, list_elem_t *elem,
                                         list_less_func *less, void *aux)
{
    list_elem_t *e;

    ASSERT (list != NULL);
    ASSERT (elem != NULL);
    ASSERT (less != NULL);

    for (e = list_begin (list); e != list_end (list); e = list_next (e))
        if (less (elem, e, aux))
            break;
    return list_insert (e, elem);
}

/* Iterates through LIST and removes all but the first in each
     set of adjacent elements that are equal according to LESS
     given auxiliary data AUX.  If DUPLICATES is non-null, then the
     elements from LIST are appended to DUPLICATES. */
void
list_unique (list_t *list, list_t *duplicates,
                         list_less_func *less, void *aux)
{
    list_elem_t *elem, *next;

    ASSERT (list != NULL);
    ASSERT (less != NULL);
    if (list_empty (list))
        return;

    elem = list_begin (list);
    while ((next = list_next (elem)) != list_end (list))
        if (!less (elem, next, aux) && !less (next, elem, aux)) 
            {
                list_remove (next);
                if (duplicates != NULL)
                    list_push_back (duplicates, next);
            }
        else
            elem = next;
}

/* Returns the element in LIST with the largest value according
     to LESS given auxiliary data AUX.  If there is more than one
     maximum, returns the one that appears earlier in the list.  If
     the list is empty, returns its tail. */
list_elem_t *
list_max (list_t *list, list_less_func *less, void *aux)
{
    list_elem_t *max = list_begin (list);
    if (max != list_end (list)) 
        {
            list_elem_t *e;
            
            for (e = list_next (max); e != list_end (list); e = list_next (e))
                if (less (max, e, aux))
                    max = e; 
        }
    return max;
}

/* Returns the element in LIST with the smallest value according
     to LESS given auxiliary data AUX.  If there is more than one
     minimum, returns the one that appears earlier in the list.  If
     the list is empty, returns its tail. */
list_elem_t *
list_min (list_t *list, list_less_func *less, void *aux)
{
    list_elem_t *min = list_begin (list);
    if (min != list_end (list)) 
        {
            list_elem_t *e;
            
            for (e = list_next (min); e != list_end (list); e = list_next (e))
                if (less (e, min, aux))
                    min = e; 
        }
    return min;
}

/* Removes the element in LIST defined by LIST_MAX().
     Undefined behavior if LIST is empty before removal. */
list_elem_t *
list_pop_max (list_t *list, list_less_func *less, void *aux)
{
    ASSERT (!list_empty(list));
    list_elem_t *max = list_max (list, less, aux);
    list_remove (max);
    return max;
}

/* Removes the element in LIST defined by LIST_MIN().
     Undefined behavior if LIST is empty before removal. */
list_elem_t *
list_pop_min (list_t *list, list_less_func *less, void *aux)
{
    ASSERT (!list_empty(list));
    list_elem_t *min = list_min (list, less, aux);
    list_remove (min);
    return min;
}