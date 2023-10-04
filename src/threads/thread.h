/*! \file thread.h
 *
 * Declarations for the kernel threading functionality in PintOS.
 */

#ifndef THREADS_THREAD_H
#define THREADS_THREAD_H

#include <debug.h>
#include <list.h>
#include <hash.h>
#include <stdint.h>

#include "synch.h"
#include "fixedpoint.h"
#ifdef USERPROG
#include "userprog/filemap.h"
#include "userprog/pagedir.h"
#include "vm/mappings.h"
#endif

/*! States in a thread's life cycle. */
enum thread_status {
    THREAD_RUNNING,     /*!< Running thread. */
    THREAD_READY,       /*!< Not running but ready to run. */
    THREAD_BLOCKED,     /*!< Waiting for an event to trigger. */
    THREAD_DYING        /*!< About to be destroyed. */
};

/*! Thread identifier type.
    You can redefine this to whatever type you like. */
typedef int tid_t;
#define TID_ERROR ((tid_t) -1)          /*!< Error value for tid_t. */

/* Thread priorities. */
#define PRI_MIN 0                       /*!< Lowest priority. */
#define PRI_DEFAULT 31                  /*!< Default priority. */
#define PRI_MAX 63                      /*!< Highest priority. */

/*! A kernel thread or user process.

   Each thread structure is stored in its own 4 kB page.  The
   thread structure itself sits at the very bottom of the page
   (at offset 0).  The rest of the page is reserved for the
   thread's kernel stack, which grows downward from the top of
   the page (at offset 4 kB).  Here's an illustration:

\verbatim
        4 kB +---------------------------------+
             |          kernel stack           |
             |                |                |
             |                |                |
             |                V                |
             |         grows downward          |
             |                                 |
             |                                 |
             |                                 |
             |                                 |
             |                                 |
             |                                 |
             |                                 |
             |                                 |
             +---------------------------------+
             |              magic              |
             |                :                |
             |                :                |
             |               name              |
             |              status             |
             |               tid               |
        0 kB +---------------------------------+
\endverbatim

   The upshot of this is twofold:

      1. First, `thread_t' must not be allowed to grow too
         big.  If it does, then there will not be enough room for
         the kernel stack.  Our base `thread_t' is only a
         few bytes in size.  It probably should stay well under 1
         kB.

      2. Second, kernel stacks must not be allowed to grow too
         large.  If a stack overflows, it will corrupt the thread
         state.  Thus, kernel functions should not allocate large
         structures or arrays as non-static local variables.  Use
         dynamic allocation with malloc() or palloc_get_page()
         instead.

   The first symptom of either of these problems will probably be
   an assertion failure in thread_current(), which checks that
   the `magic' member of the running thread's `thread_t' is
   set to THREAD_MAGIC.  Stack overflow will normally change this
   value, triggering the assertion.

   The `elem' member has a dual purpose.  It can be an element in
   the run queue (thread.c), or it can be an element in a
   semaphore wait list (synch.c).  It can be used these two ways
   only because they are mutually exclusive: only a thread in the
   ready state is on the run queue, whereas only a thread in the
   blocked state is on a semaphore wait list.
*/
typedef struct thread {
    /*! Owned by thread.c. */
    /**@{*/
    tid_t tid;                      /*!< ID of the thread. Unique up to reboot. */
    enum thread_status status;      /*!< Thread state. */
    char name[16];                  /*!< Name (for debugging purposes). */
    uint8_t *stack;                 /*!< Saved stack pointer. */
    int priority;                   /*!< Current value of priority. */
    int base_priority;              /*!< Priority without donations. */
    int nice;                       /*!< Thread's niceness value. */
    fp_val recent_cpu;              /*!< Metric of CPU time used recently. */
    int64_t time;                   /*!< Stores a time. This is used both by the
                                         sleep mechanism and read write locks. */
    hash_elem_t allelem;            /*!< Hash element for all threads hash. */
    /**@}*/

    /*! Shared between thread.c and synch.c. */
    /**@{*/
    list_elem_t elem;               /*!< List element. */
    list_t held_locks;              /*!< List of all locks held by thread. */
    lock_t *blocked_on;             /*!< Lock the thread is currently blocked on
                                         or NULL. */
    /**@}*/

#ifdef USERPROG
    /*! Owned by userprog/process.c and userprog/pagedir.c */
    /**@{*/
    sup_pagetable_t pt;             /*!< Page table. */
    file_map_t file_map;            /*!< Map of filenos to file_t's. */
    file_t *exec_file;              /*!< File being currently executed. */
    uint32_t exit_code;             /*!< Exit code of the process. */
    void *handle;                   /*!< Parent's handle for the child.
                                         Owned by parent and NULL if orphan. */
    list_t children;                /*!< List to look up children by pid. */
    void *stack_pointer;            /*!< The stack pointer of the user process,
                                         if executing a syscall. */
    dir_t *wd;                      /*<! Working directory of the process. */
    /**@{*/
#endif

    /*! Owned by thread.c. */
    /**@{*/
    unsigned magic;                     /* Detects stack overflow. */
    /**@}*/
} thread_t;

/*! Total number of ticks since OS on */
extern long long ticks;

/*! If false (default), use round-robin scheduler.
    If true, use multi-level feedback queue scheduler.
    Controlled by kernel command-line option "-o mlfqs". */
extern bool thread_mlfqs;

void thread_init(void);
void thread_start(void);

void thread_tick(void);
void thread_print_stats(void);

typedef void thread_func(void *aux);
tid_t thread_create(const char *name, int priority, thread_func *, void *);

void thread_block(void);
void thread_unblock(thread_t*);

thread_t *thread_current (void);
tid_t thread_tid(void);
thread_t *get_thread(tid_t);
const char *thread_name(void);

unsigned tid_hash(tid_t);
bool tid_less(tid_t, tid_t);

void thread_exit(void) NO_RETURN;
void thread_yield(void);
void thread_yield_if_lost_primacy(void);

void thread_sleep(int64_t for_ticks);

/*! Performs some operation on thread t, given auxiliary data AUX. */
typedef void thread_action_func(thread_t *t, void *aux);

void thread_foreach(thread_action_func *, void *);
extern thread_t *list_entry_thread(const list_elem_t *e);

int thread_get_priority(void);
void thread_set_priority(int);

void thread_lost_priority_donor(int donation);
void thread_gained_priority_donor(thread_t *t, int donation);

int thread_get_nice(void);
void thread_set_nice(int);
int thread_get_recent_cpu(void);
int thread_get_load_avg(void);

#endif /* threads/thread.h */

