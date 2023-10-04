/*! \file synch.h
 *
 * Data structures and function declarations for thread synchronization
 * primitives.
 */

#ifndef THREADS_SYNCH_H
#define THREADS_SYNCH_H

#include <list.h>
#include <stdbool.h>

/*! A counting semaphore. */
typedef struct semaphore {
    unsigned value;         /*!< Current value. */
    list_t waiters;         /*!< List of waiting threads. */
} semaphore_t;

/*! Preprocessor initializer for semaphore starting at n.
    Used as semaphore_t sema = SEMA_INITIALIZER(sema, n). */
#define SEMA_INITIALIZER(name, n) (semaphore_t)\
    {.value = (n),\
     .waiters = LIST_INITIALIZER((name).waiters)}

void sema_init(semaphore_t *, unsigned value);
void sema_down(semaphore_t *);
bool sema_try_down(semaphore_t *);
void sema_up(semaphore_t *);
void sema_self_test(void);

/*! A fast, memory efficient binary semaphore.
    Releases waiters in LIFO order.

    Uses the fact that thread structs are always page-aligned to store data in
    the lower 12 bits of a pointer. */
typedef struct bin_sema {
    uintptr_t data;
} bin_sema_t;

void bin_sema_init(bin_sema_t *, bool value);
void bin_sema_down(bin_sema_t *);
bool bin_sema_try_down(bin_sema_t *);
void bin_sema_up(bin_sema_t *);

/*! Lock. */
typedef struct lock {
    struct thread *holder;      /*!< Thread holding lock (for debugging). */
    semaphore_t semaphore;      /*!< Binary semaphore controlling access. */
    int priority;               /*!< Max priority of threads blocking on lock. */
    list_elem_t elem;           /*!< List element for threads to hold. */
} lock_t;

/*! Preprocessor initializer for lock.
    Used as lock_t lock = LOCK_INITIALIZER(lock). */
#define LOCK_INITIALIZER(name) (lock_t)\
    {.holder = NULL,\
     .priority = PRI_MIN,\
     .semaphore = SEMA_INITIALIZER((name).semaphore, 1)}

void lock_init(lock_t *);
void lock_acquire(lock_t *);
bool lock_try_acquire(lock_t *);
void lock_release(lock_t *);
bool lock_held_by_current_thread(const lock_t *);
void lock_gained_priority_donor(lock_t *lock, int donation);

/*! Read-write lock. */
typedef struct rwlock {
    /*! The list of threads waiting to get the lock as a reader. */
    list_t r_waiters;
    /*! The list of threads waiting to get the lock as a reader. */
    list_t w_waiters;
    /*! The number of holders of the lock as readers. Writers are treated as
        negative, so the value of num_holders ranges from -1 to INT32_MAX. */
    int32_t num_holders;
} rwlock_t;

void rw_init(rwlock_t *);
void rw_read_acquire(rwlock_t *);
void rw_read_release(rwlock_t *);
bool rw_read_held_by_current_thread(const rwlock_t *);
void rw_write_acquire(rwlock_t *);
void rw_write_release(rwlock_t *);
bool rw_write_held_by_current_thread(const rwlock_t *);

/*! Condition variable.
    A condition variable can be implemented as a semaphore without additional
    fields */
typedef struct condition {
    semaphore_t semaphore;      /*!< Semaphore which threads wait on. */
} condition_t;

/*! Preprocessor initializer for condition variable.
    Used as condition_t cond = COND_INITIALIZER(cond). */
#define COND_INITIALIZER(name) (condition_t)\
    {.semaphore = SEMA_INITIALIZER((name).semaphore, 0)}

void cond_init(condition_t *);
void cond_wait(condition_t *, lock_t *);
void cond_signal(condition_t *, lock_t *);
void cond_broadcast(condition_t *, lock_t *);

/*! Optimization barrier.

   The compiler will not reorder operations across an
   optimization barrier.  See "Optimization Barriers" in the
   reference guide for more information.*/
#define barrier() asm volatile ("" : : : "memory")

#endif /* threads/synch.h */

