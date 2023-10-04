/*! \file synch.c
 *
 * Implementation of various thread synchronization primitives.
 */

/* This file is derived from source code for the Nachos
   instructional operating system.  The Nachos copyright notice
   is reproduced in full below. */

/* Copyright (c) 1992-1996 The Regents of the University of California.
   All rights reserved.

   Permission to use, copy, modify, and distribute this software
   and its documentation for any purpose, without fee, and
   without written agreement is hereby granted, provided that the
   above copyright notice and the following two paragraphs appear
   in all copies of this software.

   IN NO EVENT SHALL THE UNIVERSITY OF CALIFORNIA BE LIABLE TO
   ANY PARTY FOR DIRECT, INDIRECT, SPECIAL, INCIDENTAL, OR
   CONSEQUENTIAL DAMAGES ARISING OUT OF THE USE OF THIS SOFTWARE
   AND ITS DOCUMENTATION, EVEN IF THE UNIVERSITY OF CALIFORNIA
   HAS BEEN ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

   THE UNIVERSITY OF CALIFORNIA SPECIFICALLY DISCLAIMS ANY
   WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
   WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
   PURPOSE.  THE SOFTWARE PROVIDED HEREUNDER IS ON AN "AS IS"
   BASIS, AND THE UNIVERSITY OF CALIFORNIA HAS NO OBLIGATION TO
   PROVIDE MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS, OR
   MODIFICATIONS.
*/

#include "threads/synch.h"
#include <stdio.h>
#include <string.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "devices/timer.h"

/*! Gets the lock that the given semaphore is a member of. Assumes that the
    given semaphore is a lock semaphore. Behavior undefined otherwise. */
static inline lock_t *lock_from_sema(semaphore_t *sema) {
    return ((void *) sema) - offsetof(lock_t, semaphore);
}

/*! Indicates whether thread represented by `a` has lower priority than thread
    represented by `b`. */
static bool thread_priority_less(const list_elem_t *a,
                                 const list_elem_t *b,
                                 void *aux UNUSED) {
    thread_t *ta = list_entry_thread(a);
    thread_t *tb = list_entry_thread(b);
    return ta->priority < tb->priority;
}

/*! Initializes semaphore SEMA to VALUE.  A semaphore is a
    nonnegative integer along with two atomic operators for
    manipulating it:

    - down or "P": wait for the value to become positive, then
      decrement it.

    - up or "V": increment the value (and wake up one waiting
      thread, if any). */
void sema_init(semaphore_t *sema, unsigned value) {
    ASSERT(sema != NULL);

    sema->value = value;
    list_init(&sema->waiters);
}

/*! Propogate priority donation through lock. */
void lock_gained_priority_donor(lock_t *lock, int donation) {
    if (lock->priority < donation) {
        lock->priority = donation;
        if (lock->holder != NULL) {
            thread_gained_priority_donor(lock->holder, lock->priority);
        }
    }
}

/*! Helper for sema_down and lock_acquire. If islock, it adds it the
    lock owning the semaphore to the blocked_on/locks_held fields of the current
    threads as necessary. See description of sema_down. */
static void _sema_down(semaphore_t *sema, bool islock) {
    enum intr_level old_level;
    thread_t *cur = NULL;

    ASSERT(sema != NULL);
    ASSERT(!intr_context());

    old_level = intr_disable();

    cur = thread_current();

    while (sema->value == 0) {
        list_push_back(&sema->waiters, &cur->elem);
        if (islock) {
            lock_t *lock = lock_from_sema(sema);
            cur->blocked_on = lock;
            lock_gained_priority_donor(lock, cur->priority);
        }
        thread_block();
    }
    sema->value--;
    intr_set_level(old_level);
}

/*! Down or "P" operation on a semaphore.  Waits for SEMA's value
    to become positive and then atomically decrements it.

    This function may sleep, so it must not be called within an
    interrupt handler.  This function may be called with
    interrupts disabled, but if it sleeps then the next scheduled
    thread will probably turn interrupts back on. */
void sema_down(semaphore_t *sema) {
    _sema_down(sema, false);
}

/*! Down or "P" operation on a semaphore, but only if the
    semaphore is not already 0.  Returns true if the semaphore is
    decremented, false otherwise.

    This function may be called from an interrupt handler. */
bool sema_try_down(semaphore_t *sema) {
    enum intr_level old_level;
    bool success;

    ASSERT(sema != NULL);

    old_level = intr_disable();
    if (sema->value > 0) {
        sema->value--;
        success = true;
    }
    else {
      success = false;
    }
    intr_set_level(old_level);

    return success;
}

/*! Up or "V" operation on a semaphore.  Increments SEMA's value
    and wakes up one thread of those waiting for SEMA, if any.

    This function may be called from an interrupt handler. */
void sema_up(semaphore_t *sema) {
    enum intr_level old_level;

    ASSERT(sema != NULL);

    old_level = intr_disable();
    if (!list_empty(&sema->waiters)) {
        thread_t *unblock = list_entry_thread(
            list_pop_max(&sema->waiters, thread_priority_less, NULL));
        unblock->blocked_on = NULL;
        thread_unblock(unblock);
    }
    sema->value++;
    intr_set_level(old_level);
    if (old_level == INTR_ON) {
        thread_yield_if_lost_primacy();
    }
}

static void sema_test_helper(void *sema_);

/*! Initializes binary semaphore SEMA to VALUE.  A semaphore is a
    binary flag along with two atomic operators for
    manipulating it:

    - down or "P": wait for the value to become 1, then decrement it.

    - up or "V": sets the value to 1 if it's 0. Panics if the value 1. */
void bin_sema_init(bin_sema_t *sema, bool value) {
    sema->data = (uintptr_t) NULL | value;
}

static thread_t *thread_next(thread_t *t) {
    return list_entry_thread(t->elem.next);
}

static void thread_set_next(thread_t *cur, thread_t *next) {
    cur->elem.next = &next->elem;
}

/*! Down or "P" operation on a semaphore.  Waits for SEMA's value
    to become 1 and then atomically decrements it.

    This function may sleep, so it must not be called within an
    interrupt handler.  This function may be called with
    interrupts disabled, but if it sleeps then the next scheduled
    thread will probably turn interrupts back on. */
void bin_sema_down(bin_sema_t *sema) {
    enum intr_level old_level;
    thread_t *cur = NULL;

    ASSERT(sema != NULL);
    ASSERT(!intr_context());

    old_level = intr_disable();

    cur = thread_current();

    while (!(sema->data & 1)) {
        thread_set_next(cur, (thread_t *) (sema->data & ~1));
        sema->data = (uintptr_t) cur;
        thread_block();
    }
    sema->data &= ~1;
    intr_set_level(old_level);
}

/*! Down or "P" operation on a semaphore, but only if the
    semaphore is not already 0.  Returns true if the semaphore is
    decremented, false otherwise.

    This function may be called from an interrupt handler. */
bool bin_sema_try_down(bin_sema_t *sema) {
    enum intr_level old_level;
    bool success;

    ASSERT(sema != NULL);

    old_level = intr_disable();
    success = (sema->data & 1);
    sema->data &= ~1;
    intr_set_level(old_level);

    return success;
}

/*! Up or "V" operation on a semaphore.  Increments SEMA's value
    and wakes up one thread of those waiting for SEMA, if any.

    This function may be called from an interrupt handler.

    Panics is the semaphore is not 0. */
void bin_sema_up(bin_sema_t *sema) {
    enum intr_level old_level;

    ASSERT(sema != NULL);
    ASSERT(!(sema->data & 1));

    old_level = intr_disable();
    thread_t *unblock = (thread_t *) sema->data;
    if (unblock != NULL) {
        sema->data = (uintptr_t) thread_next(unblock);
        thread_unblock(unblock);
    }
    sema->data |= 1;
    intr_set_level(old_level);
    if (old_level == INTR_ON) {
        thread_yield_if_lost_primacy();
    }
}

/*! Self-test for semaphores that makes control "ping-pong"
    between a pair of threads.  Insert calls to printf() to see
    what's going on. */
void sema_self_test(void) {
    semaphore_t sema[2];
    int i;

    printf("Testing semaphores...");
    sema_init(&sema[0], 0);
    sema_init(&sema[1], 0);
    thread_create("sema-test", PRI_DEFAULT, sema_test_helper, &sema);
    for (i = 0; i < 10; i++) {
        sema_up(&sema[0]);
        sema_down(&sema[1]);
    }
    printf ("done.\n");
}

/*! Thread function used by sema_self_test(). */
static void sema_test_helper(void *sema_) {
    semaphore_t *sema = sema_;
    int i;

    for (i = 0; i < 10; i++) {
        sema_down(&sema[0]);
        sema_up(&sema[1]);
    }
}

/*! Initializes LOCK.  A lock can be held by at most a single
    thread at any given time.  Our locks are not "recursive", that
    is, it is an error for the thread currently holding a lock to
    try to acquire that lock.

   A lock is a specialization of a semaphore with an initial
   value of 1.  The difference between a lock and such a
   semaphore is twofold.  First, a semaphore can have a value
   greater than 1, but a lock can only be owned by a single
   thread at a time.  Second, a semaphore does not have an owner,
   meaning that one thread can "down" the semaphore and then
   another one "up" it, but with a lock the same thread must both
   acquire and release it.  When these restrictions prove
   onerous, it's a good sign that a semaphore should be used,
   instead of a lock. */
void lock_init(lock_t *lock) {
    ASSERT(lock != NULL);

    lock->holder = NULL;
    lock->priority = PRI_MIN;
    sema_init(&lock->semaphore, 1);
}

/*! Acquires LOCK, sleeping until it becomes available if
    necessary.  The lock must not already be held by the current
    thread.

    This function may sleep, so it must not be called within an
    interrupt handler.  This function may be called with
    interrupts disabled, but interrupts will be turned back on if
    we need to sleep. */
void lock_acquire(lock_t *lock) {
    thread_t *cur;
    ASSERT(lock != NULL);
    ASSERT(!intr_context());
    ASSERT(!lock_held_by_current_thread(lock));

    cur = thread_current();
    enum intr_level old_level = intr_disable();
    _sema_down(&lock->semaphore, true);
    if (!thread_mlfqs) {
        if (cur->priority < lock->priority) {
            cur->priority = lock->priority;
            thread_gained_priority_donor(cur, lock->priority);
        }
        list_push_back(&cur->held_locks, &lock->elem);
    }
    lock->holder = cur;
    intr_set_level(old_level);
}

/*! Tries to acquires LOCK and returns true if successful or false
    on failure.  The lock must not already be held by the current
    thread.

    This function will not sleep, so it may be called within an
    interrupt handler. */
bool lock_try_acquire(lock_t *lock) {
    bool success;

    ASSERT(lock != NULL);
    ASSERT(!lock_held_by_current_thread(lock));

    enum intr_level old_level = intr_disable();
    success = sema_try_down(&lock->semaphore);
    if (success) {
        lock->holder = thread_current();
        if (!thread_mlfqs) {
            list_push_back(&lock->holder->held_locks, &lock->elem);
        }
    }
    intr_set_level(old_level);

    return success;
}

/*! Releases LOCK, which must be owned by the current thread.

    An interrupt handler cannot acquire a lock, so it does not
    make sense to try to release a lock within an interrupt
    handler. */
void lock_release(lock_t *lock) {
    ASSERT(lock != NULL);
    ASSERT(lock_held_by_current_thread(lock));

    enum intr_level old_level = intr_disable();
    lock->holder = NULL;
    semaphore_t *sema = &lock->semaphore;
    sema_up(&lock->semaphore);
    if (!thread_mlfqs) {
        int old_lock_priority = lock->priority;
        lock->priority = list_empty(&sema->waiters) ? PRI_MIN :
            list_entry_thread(
                list_max(&sema->waiters, thread_priority_less, NULL)
            )->priority;
        list_remove(&lock->elem);
        thread_lost_priority_donor(old_lock_priority);
    } else {
        thread_yield_if_lost_primacy();
    }
    intr_set_level(old_level);
}

/*! Returns true if the current thread holds LOCK, false
    otherwise.  (Note that testing whether some other thread holds
    a lock would be racy.) */
bool lock_held_by_current_thread(const lock_t *lock) {
    ASSERT(lock != NULL);

    return lock->holder == thread_current();
}

/*! Initializes condition variable COND.  A condition variable
    allows one piece of code to signal a condition and cooperating
    code to receive the signal and act upon it. */
void cond_init(condition_t *cond) {
    ASSERT(cond != NULL);

    sema_init(&cond->semaphore, 0);
}

/*! Atomically releases LOCK and waits for COND to be signaled by
    some other piece of code.  After COND is signaled, LOCK is
    reacquired before returning.  LOCK must be held before calling
    this function.

    The monitor implemented by this function is "Mesa" style, not
    "Hoare" style, that is, sending and receiving a signal are not
    an atomic operation.  Thus, typically the caller must recheck
    the condition after the wait completes and, if necessary, wait
    again.

    A given condition variable is associated with only a single
    lock, but one lock may be associated with any number of
    condition variables.  That is, there is a one-to-many mapping
    from locks to condition variables.

    This function may sleep, so it must not be called within an
    interrupt handler.  This function may be called with
    interrupts disabled, but interrupts will be turned back on if
    we need to sleep. */
void cond_wait(condition_t *cond, lock_t *lock) {
    ASSERT(cond != NULL);
    ASSERT(lock != NULL);
    ASSERT(!intr_context());
    ASSERT(lock_held_by_current_thread(lock));

    enum intr_level old_level = intr_disable();
    lock_release(lock);
    sema_down(&cond->semaphore);
    intr_set_level(old_level);
    lock_acquire(lock);
}

/*! If any threads are waiting on COND (protected by LOCK), then
    this function signals one of them to wake up from its wait.
    LOCK must be held before calling this function.

    An interrupt handler cannot acquire a lock, so it does not
    make sense to try to signal a condition variable within an
    interrupt handler. */
void cond_signal(condition_t *cond, lock_t *lock) {
    ASSERT(cond != NULL);
    ASSERT(lock != NULL);
    ASSERT(!intr_context ());
    ASSERT(lock_held_by_current_thread (lock));

    if (!list_empty(&cond->semaphore.waiters)) {
        // technically, this is a TOCTOU race conditiion, however, the failure
        // mode is a spurious wakeup, which is acceptable behavior for a
        // condition variable.
        sema_up(&cond->semaphore);
    }
}

/*! Wakes up all threads, if any, waiting on COND (protected by
    LOCK).  LOCK must be held before calling this function.

    An interrupt handler cannot acquire a lock, so it does not
    make sense to try to signal a condition variable within an
    interrupt handler. */
void cond_broadcast(condition_t *cond, lock_t *lock) {
    ASSERT(cond != NULL);
    ASSERT(lock != NULL);
    ASSERT(!intr_context ());
    ASSERT(lock_held_by_current_thread (lock));

    while (!list_empty(&cond->semaphore.waiters)) {
        // technically, this is a TOCTOU race conditiion, however, the failure
        // mode is a spurious wakeup, which is acceptable behavior for a
        // condition variable.
        sema_up(&cond->semaphore);
    }
}

/*! Returns the time of the first enqueue write waiter, or INT64_MAX if there
    are none.*/
static int64_t front_write_waiter_time(const rwlock_t *lock) {
    if (list_empty((list_t *) &lock->w_waiters)) return INT64_MAX;
    return list_entry_thread(list_front((list_t *) &lock->w_waiters))->time;
}

/*! Unblocks a set of waiters from the rw_lock following its release:
    - If there are only read-waiters, it unblocks all of them. 
    - If there are only write-waiters, it unblocks one of them.
    - If there are both then:
        - If the first write waiter was enqueued before the first read waiter,
            it dequeues one write waiter.
        - If the first write waiter was enqueued after the first read waiter,
            it dequeues read waiters until this is no longer true. 

    This way, the unblocking mimics the behavior of a FIFO queue, except that it
    groups consecutive read lock requests into a single request. */
static void rw_unblock_waiters(rwlock_t *lock) {
    if (!list_empty(&lock->r_waiters) && !list_empty(&lock->w_waiters)) {
        thread_t *r_front = list_entry_thread(list_front(&lock->r_waiters));
        thread_t *w_front = list_entry_thread(list_front(&lock->w_waiters));
        if (r_front->time >= w_front->time) {
            list_remove(&w_front->elem);
            thread_unblock(w_front);
        } else while (!list_empty(&lock->r_waiters)) {
            list_elem_t *e = list_front(&lock->r_waiters);
            thread_t *t = list_entry_thread(e);
            if (t->time >= w_front->time) break;
            list_remove(e);
            thread_unblock(list_entry_thread(e));
        }
    } else if (!list_empty(&lock->w_waiters)) {
        thread_unblock(list_entry_thread(list_pop_front(&lock->w_waiters)));
    } else while (!list_empty(&lock->r_waiters)) {
        thread_unblock(list_entry_thread(list_pop_front(&lock->r_waiters)));
    }
}

/*! Initializes a read/write lock. */
void rw_init(rwlock_t *lock) {
    lock->num_holders = 0;
    list_init(&lock->r_waiters);
    list_init(&lock->w_waiters);
}

/*! Obtains a read/write lock as a reader, blocking if it's held by a writer or
    there were already writers in line. */
void rw_read_acquire(rwlock_t *lock) {
    ASSERT(lock != NULL);
    thread_t *cur = thread_current();

    cur->time = timer_ticks();
    enum intr_level old_level = intr_disable();
    while (lock->num_holders < 0 || cur->time > front_write_waiter_time(lock)) {
        list_push_back(&lock->r_waiters, &cur->elem);
        thread_block();
    }
    ASSERT(lock->num_holders >= 0);
    lock->num_holders++;
    intr_set_level(old_level);
}

/*! Releases a read/write lock as a reader. Should only be called while holding
    the lock as a reader, but due to implementation limitations, there is no
    assertion enforcing this. */
void rw_read_release(rwlock_t *lock) {
    ASSERT(lock != NULL);

    enum intr_level old_level = intr_disable();
    ASSERT(--lock->num_holders >= 0);
    if (lock->num_holders == 0) {
        rw_unblock_waiters(lock);
    }

    intr_set_level(old_level);
}

/*! Obtains a read/write lock as a writer, blocking if anybody holds the lock
    or if there is somebody in line for the lock. */
void rw_write_acquire(rwlock_t *lock) {
    ASSERT(lock != NULL);
    thread_t *cur = thread_current();

    cur->time = timer_ticks();
    enum intr_level old_level = intr_disable();
    while (lock->num_holders != 0) {
        list_push_back(&lock->w_waiters, &cur->elem);
        thread_block();
    }
    ASSERT(lock->num_holders == 0);
    lock->num_holders = -1;
    intr_set_level(old_level);
}

/*! Releases a read/write lock as a writer. Should only be called while holding
    the lock as a writer, but due to implementation limitations, there is no
    assertion enforcing this. */
void rw_write_release(rwlock_t *lock) {
    ASSERT(lock != NULL);

    enum intr_level old_level = intr_disable();
    ASSERT(++lock->num_holders == 0);
    rw_unblock_waiters(lock);

    intr_set_level(old_level);
}