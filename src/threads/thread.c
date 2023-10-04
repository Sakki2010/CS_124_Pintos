#include "threads/thread.h"
#include <debug.h>
#include <stddef.h>
#include <random.h>
#include <stdio.h>
#include <string.h>
#include <bitmap.h>
#include "threads/flags.h"
#include "threads/interrupt.h"
#include "threads/intr-stubs.h"
#include "threads/palloc.h"
#include "threads/switch.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "threads/fixedpoint.h"
#include "devices/timer.h"
#ifdef USERPROG
#include "userprog/process.h"
#include "vm/frametbl.h"
#endif

/*! Random value for thread_t's `magic' member.
    Used to detect stack overflow.  See the big comment at the top
    of thread.h for details. */
#define THREAD_MAGIC 0xcd6abf4b

/*! The number of distinct priorities. */
#define PRI_CNT (PRI_MAX - PRI_MIN + 1)

/*! How often the advanced scheduler should update priorities. */
#define PRIORITY_FREQ 4

/*! Default niceness. */
#define NICE_DEFAULT 0

/*! Default recent_cpu value. */
#define RECENT_CPU_DEFAULT ((fp_val) {.v = 0})

/*! Struct holding the PRI_MAX-PRI_MIN + 1 ready queues*/
typedef struct ready_queue {
    list_t queues[PRI_CNT];        /*!< Array of queues threads by priority. */
    bitmap_t *populated_queues;    /*!< Bitmap of queue emptiness. */
    size_t num_ready_threads;      /*!< Threads running or ready to be run. (Excluding idle.)*/
    /*! Buffer to store the populated queues bitmap. */
    char bitmap_buf[BITMAP_BUF_SIZE(PRI_CNT)];
} ready_queue_t;

/*! List of processes currently asleep. */
static list_t sleeping_list;

// /*! List of processes in THREAD_READY state, that is, processes
//     that are ready to run but not actually running. */
// static list_t ready_list;

/*! Priority separated queues of processes in THREAD_READY state,
    that is, processes that are ready to run but not actually running. */
static ready_queue_t ready_queue;

/*! Hash of all processes.  Processes are added to this list
    when they are first scheduled and removed when they exit. */
static hash_t all_hash;

/*! Idle thread. */
static thread_t *idle_thread;

/*! Initial thread, the thread running init.c:main(). */
static thread_t *initial_thread;

/*! Stack frame for kernel_thread(). */
struct kernel_thread_frame {
    void *eip;                  /*!< Return address. */
    thread_func *function;      /*!< Function to call. */
    void *aux;                  /*!< Auxiliary data for function. */
};

/* Statistics. */
long long ticks;                /*!< # of timer ticks since start. */
static long long idle_ticks;    /*!< # of timer ticks spent idle. */
static long long kernel_ticks;  /*!< # of timer ticks in kernel threads. */
static long long user_ticks;    /*!< # of timer ticks in user programs. */

/* Scheduling. */
#define TIME_SLICE 4            /*!< # of timer ticks to give each thread. */
static unsigned thread_ticks;   /*!< # of timer ticks since last yield. */

/*! If false (default), use round-robin scheduler.
    If true, use multi-level feedback queue scheduler.
    Controlled by kernel command-line option "-o mlfqs". */
bool thread_mlfqs;

/*! Overall system load average. */
static fp_val load_avg;

/*! Lock used by allocate_tid(). */
static lock_t tid_lock;

static void kernel_thread(thread_func *, void *aux);

static void idle(void *aux UNUSED);
static thread_t *running_thread(void);
static thread_t *next_thread_to_run(void);
static void init_thread(thread_t *, const char *name, int priority, int nice,
        fp_val recent_cpu);
static bool is_thread(thread_t *) UNUSED;
static void *alloc_frame(thread_t *, size_t size);
static void schedule(void);
void thread_schedule_tail(thread_t *prev);
static void enqueue_ready_thread(thread_t *t);
static thread_t *dequeue_ready_thread(void);
static void bump_ready_thread(thread_t *t, int old_priority);
static void init_ready_queue(void);
static void _thread_set_priority(int new_priority);
static int highest_ready_priority(void);
static void thread_decreased_priority(void);
static void calculate_load_avg(void);
fp_val _thread_get_recent_cpu(void);
static void calculate_priority(thread_t *t, void *aux UNUSED);
static void calculate_recent_cpu(thread_t *t, void *aux UNUSED);
static int _calculate_priority(fp_val recent_cpu, int nice);
static size_t num_ready_threads(void);
static tid_t allocate_tid(void);
static void register_thread(thread_t *t);
static void remove_thread(thread_t *t);

/*! Shorthand function to get thread from its list element */
inline thread_t *list_entry_thread(const list_elem_t *e) {
    return list_entry(e, thread_t, elem);
}

/*! Shorthand function to get thread from its hash element */
static inline thread_t *th_entry(const hash_elem_t *e) {
    return hash_entry(e, thread_t, allelem);
}

/*! Computes the hash of a tid, for use by any external code which wants to hash
    on tids. */
unsigned tid_hash(tid_t tid) {
    return hash_int(tid);
}

/*! Compares two tids, for use by any external code which wants to hash
    on tids. Only guarantee is that tid_less forms a total order on tids. */
bool tid_less(tid_t a, tid_t b) {
    return a < b;
}

/*! All thread hash hash function and comparison function. */
static unsigned thread_hash_hash(const hash_elem_t *e, void *aux UNUSED) {
    return tid_hash(th_entry(e)->tid);
}
static bool thread_hash_less(const hash_elem_t *a, const hash_elem_t *b,
                        void *aux UNUSED) {
    return tid_less(th_entry(a)->tid, th_entry(b)->tid);
}

/*! Initializes the threading system by transforming the code
    that's currently running into a thread.  This can't work in
    general and it is possible in this case only because loader.S
    was careful to put the bottom of the stack at a page boundary.

    Also initializes the run queue and the tid lock.

    After calling this function, be sure to initialize the page allocator
    before trying to create any threads with thread_create().

    It is not safe to call thread_current() until this function finishes. */
void thread_init(void) {
    ASSERT(intr_get_level() == INTR_OFF);

    init_ready_queue();
    list_init(&sleeping_list);
    lock_init(&tid_lock);

    load_avg = FP(0);

    /* Set up a thread structure for the running thread. */
    initial_thread = running_thread();
    int priority = thread_mlfqs ?
        _calculate_priority(RECENT_CPU_DEFAULT, NICE_DEFAULT) : PRI_DEFAULT;
    init_thread(initial_thread, "main", priority, NICE_DEFAULT,
        RECENT_CPU_DEFAULT);
    initial_thread->status = THREAD_RUNNING;
    initial_thread->tid = allocate_tid();
}

/*! Starts preemptive thread scheduling by enabling interrupts.
    Also creates the idle thread. */
void thread_start(void) {
    /* Create the idle thread. */
    semaphore_t idle_started;
    sema_init(&idle_started, 0);
    ASSERT(hash_init(&all_hash, thread_hash_hash, thread_hash_less, NULL));
    register_thread(initial_thread);
    thread_create("idle", PRI_MIN, idle, &idle_started);

    /* Start preemptive thread scheduling. */
    intr_enable();

    /* Wait for the idle thread to initialize idle_thread. */
    sema_down(&idle_started);
}

/*! Called by the timer interrupt handler at each timer tick.
    Thus, this function runs in an external interrupt context. */
void thread_tick(void) {
    thread_t *t = thread_current();

    // enum intr_level old_level = intr_disable();

    /* Update statistics. */
    ticks++;
    if (t == idle_thread) {
        idle_ticks++;
    }
#ifdef USERPROG
    else if (!sup_pt_is_kernel(&t->pt)) {
        user_ticks++;
        // parameters suggested by Eric Paul, who had absolutely no idea what
        // they meant but they worked pretty well.
        const size_t BLOCK_CNT = 2;
        const size_t AGE_FREQ = 2;
        if (user_ticks % AGE_FREQ == 0) {
            frametbl_tick((user_ticks / AGE_FREQ) % BLOCK_CNT, BLOCK_CNT);
        }
    }
#endif
    else {
        kernel_ticks++;
    }

    if (thread_mlfqs) {
        t->recent_cpu = FP_ADD(t->recent_cpu, 1);
    }
    if (thread_mlfqs && ticks % TIMER_FREQ == 0) {
        calculate_load_avg(); // Recalculate load average, globally
        thread_foreach(calculate_recent_cpu, NULL); // Update recent cpus
    }
    if (thread_mlfqs && ticks % PRIORITY_FREQ == 0) {
        thread_foreach(calculate_priority, NULL); // Update priorities
        if (t != idle_thread && t->priority < highest_ready_priority()) { // Yield if necessary
            intr_yield_on_return();
        }
    }

    /* Enforce preemption. */
    if (++thread_ticks >= TIME_SLICE)
        intr_yield_on_return();

    // intr_set_level(old_level);
}

/*! Prints thread statistics. */
void thread_print_stats(void) {
    printf("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n",
           idle_ticks, kernel_ticks, user_ticks);
}

/*! Creates a new kernel thread named NAME with the given initial PRIORITY,
    which executes FUNCTION passing AUX as the argument, and adds it to the
    ready queue.  Returns the thread identifier for the new thread, or
    TID_ERROR if creation fails.

    If thread_start() has been called, then the new thread may be scheduled
    before thread_create() returns.  It could even exit before thread_create()
    returns.  Contrariwise, the original thread may run for any amount of time
    before the new thread is scheduled.  Use a semaphore or some other form of
    synchronization if you need to ensure ordering.

    The code provided sets the new thread's `priority' member to PRIORITY, but
    no actual priority scheduling is implemented.  Priority scheduling is the
    goal of Problem 1-3. */
tid_t thread_create(const char *name, int priority, thread_func *function,
                    void *aux) {
    thread_t *t;
    struct kernel_thread_frame *kf;
    struct switch_entry_frame *ef;
    struct switch_threads_frame *sf;
    tid_t tid;

    ASSERT(function != NULL);

    /* Allocate thread. */
    t = palloc_get_page(PAL_ZERO);
    if (t == NULL)
        return TID_ERROR;

    /* Initialize thread. */
    int nice = thread_mlfqs ? thread_get_nice() : NICE_DEFAULT;
    fp_val recent_cpu = thread_mlfqs ?
        _thread_get_recent_cpu() : RECENT_CPU_DEFAULT;
    priority = thread_mlfqs ?
        _calculate_priority(recent_cpu, nice) : priority;
    init_thread(t, name, priority, nice, recent_cpu);
    tid = t->tid = allocate_tid();

    /* Stack frame for kernel_thread(). */
    kf = alloc_frame(t, sizeof *kf);
    kf->eip = NULL;
    kf->function = function;
    kf->aux = aux;

    /* Stack frame for switch_entry(). */
    ef = alloc_frame(t, sizeof *ef);
    ef->eip = (void (*) (void)) kernel_thread;

    /* Stack frame for switch_threads(). */
    sf = alloc_frame(t, sizeof *sf);
    sf->eip = switch_entry;
    sf->ebp = 0;

    enum intr_level old_level = intr_disable();
    register_thread(t);
    intr_set_level(old_level);

    /* Add to run queue. */
    thread_unblock(t);
    thread_yield_if_lost_primacy();

    return tid;
}

/*! Puts the current thread to sleep.  It will not be scheduled
    again until awoken by thread_unblock().

    This function must be called with interrupts turned off.  It is usually a
    better idea to use one of the synchronization primitives in synch.h. */
void thread_block(void) {
    ASSERT(!intr_context());
    ASSERT(intr_get_level() == INTR_OFF);

    thread_current()->status = THREAD_BLOCKED;
    schedule();
}

/*! Transitions a blocked thread T to the ready-to-run state.  This is an
    error if T is not blocked.  (Use thread_yield() to make the running
    thread ready.)

    This function does not preempt the running thread.  This can be important:
    if the caller had disabled interrupts itself, it may expect that it can
    atomically unblock a thread and update other data. */
void thread_unblock(thread_t *t) {
    enum intr_level old_level;

    ASSERT(is_thread(t));

    old_level = intr_disable();
    ASSERT(t->status == THREAD_BLOCKED);
    enqueue_ready_thread(t);

    t->status = THREAD_READY;
    intr_set_level(old_level);
}

/*! Returns the name of the running thread. */
const char * thread_name(void) {
    return thread_current()->name;
}

/*! Returns the running thread.
    This is running_thread() plus a couple of sanity checks.
    See the big comment at the top of thread.h for details. */
thread_t * thread_current(void) {
    thread_t *t = running_thread();

    /* Make sure T is really a thread.
       If either of these assertions fire, then your thread may
       have overflowed its stack.  Each thread has less than 4 kB
       of stack, so a few big automatic arrays or moderate
       recursion can cause stack overflow. */
    ASSERT(is_thread(t));
    ASSERT(t->status == THREAD_RUNNING);

    return t;
}

/*! Returns the running thread's tid. */
tid_t thread_tid(void) {
    return thread_current()->tid;
}

/*! Registers a thread into the all threads hash map.
    Assumes interrupts are off. */
static void register_thread(thread_t *t) {
    ASSERT(intr_get_level() == INTR_OFF);

    hash_insert(&all_hash, &t->allelem);
}

/*! Removes a thread from the all threads hash map.
    Assumes interrupts are off. */
static void remove_thread(thread_t *t) {
    ASSERT(intr_get_level() == INTR_OFF);

    hash_delete(&all_hash, &t->allelem);
}

/*! Looks up thread by its tid. Returns NULL if given an invalid tid, including
    the tid of a thread that has already exited. */
thread_t *get_thread(tid_t tid) {
    // Current implementation of `tid_t` is an alias for thread_t *
    thread_t search = {.tid = tid};
    enum intr_level old_level = intr_disable();
    thread_t *t = th_entry(hash_find(&all_hash, &search.allelem));
    intr_set_level(old_level);
    return t;
}

/*! Deschedules the current thread and destroys it.  Never
    returns to the caller. */
void thread_exit(void) {
    ASSERT(!intr_context());

#ifdef USERPROG
    process_cleanup();
#endif

    /* Remove thread from all threads list, set our status to dying,
       and schedule another process.  That process will destroy us
       when it calls thread_schedule_tail(). */
    intr_disable();
    remove_thread(thread_current());
    thread_current()->status = THREAD_DYING;
    schedule();
    NOT_REACHED();
}

/*! Yields the CPU.  The current thread is not put to sleep and
    may be scheduled again immediately at the scheduler's whim. */
void thread_yield(void) {
    thread_t *cur = thread_current();
    enum intr_level old_level;

    ASSERT(!intr_context());

    old_level = intr_disable();
    if (cur != idle_thread) enqueue_ready_thread(cur);
    cur->status = THREAD_READY;
    schedule();
    intr_set_level(old_level);
}

/*! Indicates whether thread represented by `a` should wake up after thread
    represented by `b`. */
static bool thread_wake_less(const list_elem_t *a,
                             const list_elem_t *b,
                             void *aux UNUSED) {
    thread_t *ta = list_entry_thread(a);
    thread_t *tb = list_entry_thread(b);
    return ta->time < tb->time;
}

/*! Yields the CPU for at least `for_ticks` ticks, after which it may be
    scheduled again as the scheduler choses. Sleeps for nonpositive times are
    yields. The idle thread cannot sleep. */
void thread_sleep(int64_t for_ticks) {
    if (for_ticks <= 0) {
        thread_yield();
        return;
    }
    thread_t *cur = thread_current();
    enum intr_level old_level;

    ASSERT(!intr_context());
    ASSERT(cur != idle_thread);

    old_level = intr_disable();
    cur->time = ticks + for_ticks;
    list_insert_ordered(&sleeping_list, &cur->elem, thread_wake_less, NULL);
    thread_block();
    intr_set_level(old_level);
}

/*! Wakes up all sleeping threads whose wake up time is less than the current
    global tick. Assumes interrupts are disabled. */
static inline void thread_alarm_clock(void) {
    while (!list_empty(&sleeping_list) &&
        list_entry_thread(list_front(&sleeping_list))->time <= ticks
    ) {
        thread_unblock(list_entry_thread(list_pop_front(&sleeping_list)));
    }
}

/*! Invoke function 'func' on all threads, passing along 'aux'.
    This function must be called with interrupts off. */
void thread_foreach(thread_action_func *func, void *aux) {
    ASSERT(intr_get_level() == INTR_OFF);
    hash_iterator_t i;
    hash_first(&i, &all_hash);
    while (hash_next(&i)) {
        func(th_entry(hash_cur(&i)), aux);
    }
}

/*! Relinquishish control if the running thread no longer has the highest
    priority. */
void thread_yield_if_lost_primacy(void) {
    if (highest_ready_priority() > thread_get_priority()) {
        thread_yield();
    }
}

/*! Handle reshuffling of a thread that lost priority in queues */
static void thread_decreased_priority(void) {
    // Since our priority decreased, we may need to yield.
    thread_yield_if_lost_primacy();
}

/*! Handle reshuffling of a thread that gained priority in queues */
static void thread_increased_priority(thread_t *t, int old_priority) {
    if (!thread_mlfqs && t->blocked_on != NULL) {
        lock_gained_priority_donor(t->blocked_on, t->priority);
    } else if (t->status == THREAD_READY) {
        bump_ready_thread(t, old_priority);
    }
}

/*! Helper for setting the current thread's priority; invoked by
    thread_set_priority and internally by the advanced scheduler. Updates the
    thread's position in ready queue if necessary and yields if necessary.
    Atomic. Makes no assumptions about interrupt level. */
static void _thread_set_priority(int new_priority) {
    thread_t *cur = thread_current();
    enum intr_level old_level = intr_disable();
    if (thread_mlfqs) {
        int old_priority = cur->priority;
        cur->priority = new_priority;
        if (old_priority < new_priority) {
            thread_increased_priority(cur, old_priority);
        } else if (old_priority > new_priority) {
            thread_decreased_priority();
        }
    } else {
        int old_priority = cur->base_priority;
        cur->base_priority = new_priority;
        if (old_priority < new_priority) {
            thread_gained_priority_donor(cur, new_priority);
        } else if (old_priority > new_priority) {
            thread_lost_priority_donor(old_priority);
        }
    }

    intr_set_level(old_level);
}

/*! Sets the current thread's base priority to NEW_PRIORITY. Priority must be
    between PRI_MIN and PRI_MAX. */
void thread_set_priority(int new_priority) {
    ASSERT(PRI_MIN <= new_priority && new_priority <= PRI_MAX);
    if (!thread_mlfqs) {
        _thread_set_priority(new_priority);
    }
}

/*! Returns the current thread's priority. */
int thread_get_priority(void) {
    return thread_current()->priority;
}

/*! Given a thread, recalculates its recent cpu usage. */
static void calculate_recent_cpu(thread_t *t, void *aux UNUSED) {
    ASSERT(thread_mlfqs);
    if (t != idle_thread) {
        t->recent_cpu = FP_ADD(
            FP_MUL(
                FP_DIV(
                    FP_MUL(load_avg, 2),
                    FP_ADD(FP_MUL(load_avg, 2), 1)
                ),
                t->recent_cpu
            ),
            t->nice
        );
    }
}

/*! Updates the priority of a thread, in accordance with the advanced scheduler.
    Also updates its recent cpu usage. Assumes load_avg global is correct.
    Updates the thread's position in ready queue if necessary.
    Does NOT yield. */
static void calculate_priority(thread_t *t, void *aux UNUSED) {
    if (t != idle_thread) {
        int old_priority = t->priority;
        t->priority = _calculate_priority(t->recent_cpu, t->nice);
        if (old_priority < t->priority) {
            thread_increased_priority(t, old_priority);
        }
    }
}

/*! Performs the priority calculation for a thread
    under the advanced scheduler. */
static int _calculate_priority(fp_val recent_cpu, int nice) {
    ASSERT(thread_mlfqs);
    long priority = FP_TRUNC(FP_SUB(
            FP_SUB(PRI_MAX, FP_DIV(recent_cpu, 4)),
            FP_MUL(nice, 2)
    ));

    return priority > PRI_MAX ? PRI_MAX :
        (priority < PRI_MIN ? PRI_MIN :
        priority);
}

/*! Recalculate's the current thread's priority.
    For use by the advanced scheduler. */
static void thread_calculate_priority(void) {
    _thread_set_priority(_calculate_priority(_thread_get_recent_cpu(),
        thread_get_nice()));
}

/*! Sets the current thread's nice value to NICE. */
void thread_set_nice(int nice) {
    ASSERT(thread_mlfqs);
    thread_current()->nice = nice;
    thread_calculate_priority();
}

/*! Returns the current thread's nice value. */
int thread_get_nice(void) {
    return thread_current()->nice;
}

/*! Recalculates the system's load average. */
static void calculate_load_avg(void) {
    load_avg = FP_ADD(
        FP_MUL(
            FP_DIV(59, 60),
            load_avg
        ),
        FP_MUL(
            FP_DIV(1, 60),
            num_ready_threads()
        )
    );
    // load_avg = FP(num_ready_threads());
}

/*! Returns 100 times the system load average. */
int thread_get_load_avg(void) {
    return FP_ROUND(FP_MUL(load_avg, 100));
}

/*! Returns the current thread's recent_cpu value. */
fp_val _thread_get_recent_cpu(void) {
    return thread_current()->recent_cpu;
}

/*! Returns 100 times the current thread's recent_cpu value. */
int thread_get_recent_cpu(void) {
    return FP_ROUND(FP_MUL(_thread_get_recent_cpu(), 100));
}

/*! Indicates whether lock represented by `a` has lower priotity than lock
    represented by `b`. */
static bool lock_priority_less(const list_elem_t *a,
                              const list_elem_t *b,
                              void *aux UNUSED) {
    lock_t *la = list_entry(a, lock_t, elem);
    lock_t *lb = list_entry(b, lock_t, elem);
    return la->priority < lb->priority;
}

/*! Gets the maximum priority in a list of locks. */
static int lock_list_max_priority(list_t *l) {
    list_elem_t *e = list_max(l, lock_priority_less, NULL);
    return list_entry(e, lock_t, elem)->priority;
}

/*! Update current thread's priority based on losing a donor. Because donors are
    always blocked threads, a thread can never lose a donor except through its
    own actions, so this function only takes a donation argument.

    Assumes interrupts are off. */
void thread_lost_priority_donor(int donation) {
    thread_t *cur = thread_current();

    ASSERT(!intr_context());
    ASSERT(intr_get_level() == INTR_OFF);
    ASSERT(cur->status == THREAD_RUNNING);
    ASSERT(cur->blocked_on == NULL);
    // A threads priority should be the maximum of all its donors (and base)
    // thus, a lost donor should never be larger than the current priority
    // because then it's either not a real donor or the priority was not
    // properly updated at an earlier point.
    ASSERT(cur->priority >= donation);

    int old_priority = cur->priority;

    if (old_priority > donation) {
        // if the priority is larger than the donation, then this wasn't the
        // largest contributer to priority and so its loss requires no action
        return;
    }

    // No propogation is necessary, since a running thread cannot be donating
    // priority.
    int max_lock_priority = lock_list_max_priority(&cur->held_locks);
    cur->priority = cur->base_priority > max_lock_priority ?
                    cur->base_priority : max_lock_priority;

    if (old_priority > cur->priority) {
        thread_decreased_priority();
    }
}

/*! Update given thread's priority based on gaining a donor and propogates
    increased priority through any donees of `t`. The `yield` parameter
    indicates whether the running thread should check whether it lost primacy.

    Assumes interrupts are off. */
void thread_gained_priority_donor(thread_t *t, int donation) {
    ASSERT(t != NULL && is_thread(t));
    ASSERT(!intr_context());
    ASSERT(intr_get_level() == INTR_OFF);

    if (t->priority >= donation) {
        // If the current priority is already at least the donation then we
        // don't need to anything because no priorities have changed.
        return;
    }

    int old_priority = t->priority;
    t->priority = donation;
    thread_increased_priority(t, old_priority);
}

static void init_ready_queue(void) {
    for (int i = 0; i < PRI_CNT; i++){
        list_init(&ready_queue.queues[i]);
    }
    ready_queue.populated_queues =
        bitmap_create_in_buf(PRI_CNT, ready_queue.bitmap_buf,
                             sizeof(ready_queue.bitmap_buf));
    ready_queue.num_ready_threads = 0;
}

/*! Inserts a thread into the ready queue, according to its priority.

    Assumes interrupts are off. */
static void enqueue_ready_thread(thread_t *t) {
    ASSERT(intr_get_level() == INTR_OFF);

    size_t pri_i = t->priority - PRI_MIN;
    list_push_back(&ready_queue.queues[pri_i], &t->elem);
    bitmap_mark(ready_queue.populated_queues, pri_i);
    ready_queue.num_ready_threads++;
}

/*! Get the next thread from the ready queue. If the ready queue is empty,
    returns `NULL`.

    Assumes interrupts are off. */
static thread_t *dequeue_ready_thread(void) {
    ASSERT(intr_get_level() == INTR_OFF);

    size_t i = bitmap_highest(ready_queue.populated_queues, true);
    if (i == BITMAP_ERROR) {
        return NULL;
    }
    thread_t *first = list_entry_thread(list_pop_front(&ready_queue.queues[i]));
    if (list_empty(&ready_queue.queues[i])){
        bitmap_reset(ready_queue.populated_queues, i);
    }
    ready_queue.num_ready_threads--;
    return first;
}

/*! Returns the priority of the highest priority ready thread. */
static int highest_ready_priority(void) {
    enum intr_level old_level = intr_disable();
    size_t i = bitmap_highest(ready_queue.populated_queues, true);
    intr_set_level(old_level);
    if (i == BITMAP_ERROR) {
        return PRI_MIN;
    } else {
        return i + PRI_MIN;
    }
}

/*! Updates the thread's position in the ready queue based on new priority.
    The behavior of this function is unspecified if the function is not already
    in the ready queue.

    Assumes interrupts are off.

    Note that only a running thread can ever lose priority (
        strict scheduler: either by releasing a lock which was giving
            it priority or by setting its own priority lower
        BSD scheduler: by decreasing `nice` or increasing `recent_cpu`
    ), so this function should only ever be called when t's priority increased. */
static void bump_ready_thread(thread_t *t, int old_priority) {
    ASSERT(intr_get_level() == INTR_OFF);
    list_remove(&t->elem);
    ready_queue.num_ready_threads--;
    if (list_empty(&ready_queue.queues[old_priority - PRI_MIN])) {
        bitmap_reset(ready_queue.populated_queues, old_priority - PRI_MIN);
    }
    enqueue_ready_thread(t);
}

/*! Returns the number of threads that are ready to run (either enqueued or
    running, not including the idle thread) for the advanced scheduler. */
static size_t num_ready_threads(void) {
    enum intr_level old_level = intr_disable();
    size_t n = ready_queue.num_ready_threads;
    n += thread_current() != idle_thread;
    intr_set_level(old_level);
    return n;
}

/*! Idle thread.  Executes when no other thread is ready to run.

    The idle thread is initially put on the ready list by thread_start().
    It will be scheduled once initially, at which point it initializes
    idle_thread, "up"s the semaphore passed to it to enable thread_start()
    to continue, and immediately blocks.  After that, the idle thread never
    appears in the ready list.  It is returned by next_thread_to_run() as a
    special case when the ready list is empty. */
static void idle(void *idle_started_ UNUSED) {
    semaphore_t *idle_started = idle_started_;
    idle_thread = thread_current();
    sema_up(idle_started);

    for (;;) {
        /* Let someone else run. */
        intr_disable();
        thread_block();

        /* Re-enable interrupts and wait for the next one.

           The `sti' instruction disables interrupts until the completion of
           the next instruction, so these two instructions are executed
           atomically.  This atomicity is important; otherwise, an interrupt
           could be handled between re-enabling interrupts and waiting for the
           next one to occur, wasting as much as one clock tick worth of time.

           See [IA32-v2a] "HLT", [IA32-v2b] "STI", and [IA32-v3a]
           7.11.1 "HLT Instruction". */
        asm volatile ("sti; hlt" : : : "memory");
    }
}

/*! Function used as the basis for a kernel thread. */
static void kernel_thread(thread_func *function, void *aux) {
    ASSERT(function != NULL);

    intr_enable();       /* The scheduler runs with interrupts off. */
    function(aux);       /* Execute the thread function. */
    thread_exit();       /* If function() returns, kill the thread. */
}

/*! Returns the running thread. */
thread_t * running_thread(void) {
    uint32_t *esp;

    /* Copy the CPU's stack pointer into `esp', and then round that
       down to the start of a page.  Because `thread_t' is
       always at the beginning of a page and the stack pointer is
       somewhere in the middle, this locates the curent thread. */
    asm ("mov %%esp, %0" : "=g" (esp));
    return pg_round_down(esp);
}

/*! Returns true if T appears to point to a valid thread. */
static bool is_thread(thread_t *t) {
    return t != NULL && t->magic == THREAD_MAGIC;
}

/*! Does basic initialization of T as a blocked thread named NAME. */
static void init_thread(thread_t *t, const char *name, int priority, int nice,
        fp_val recent_cpu) {

    ASSERT(t != NULL);
    ASSERT(PRI_MIN <= priority && priority <= PRI_MAX);
    ASSERT(name != NULL);

    memset(t, 0, sizeof *t);
    t->status = THREAD_BLOCKED;
    strlcpy(t->name, name, sizeof t->name);
    t->stack = (uint8_t *) t + PGSIZE;
    t->priority = priority;
    t->nice = nice;
    t->recent_cpu = recent_cpu;
    t->base_priority = priority;
    t->magic = THREAD_MAGIC;
    list_init(&t->held_locks);
    t->blocked_on = NULL;
#ifdef USERPROG
    process_init(t);
#endif
}

/*! Allocates a SIZE-byte frame at the top of thread T's stack and
    returns a pointer to the frame's base. */
static void * alloc_frame(thread_t *t, size_t size) {
    /* Stack data is always allocated in word-size units. */
    ASSERT(is_thread(t));
    ASSERT(size % sizeof(uint32_t) == 0);

    t->stack -= size;
    return t->stack;
}

/*! Chooses and returns the next thread to be scheduled.  Should return a
    thread from the run queue, unless the run queue is empty.  (If the running
    thread can continue running, then it will be in the run queue.)  If the
    run queue is empty, return idle_thread. */
static thread_t * next_thread_to_run(void) {
    thread_alarm_clock();
    thread_t *ready = dequeue_ready_thread();
    return ready != NULL ? ready : idle_thread;
}

/*! Completes a thread switch by activating the new thread's page tables, and,
    if the previous thread is dying, destroying it.

    At this function's invocation, we just switched from thread PREV, the new
    thread is already running, and interrupts are still disabled.  This
    function is normally invoked by thread_schedule() as its final action
    before returning, but the first time a thread is scheduled it is called by
    switch_entry() (see switch.S).

   It's not safe to call printf() until the thread switch is complete.  In
   practice that means that printf()s should be added at the end of the
   function.

   After this function and its caller returns, the thread switch is complete. */
void thread_schedule_tail(thread_t *prev) {
    thread_t *cur = running_thread();

    ASSERT(intr_get_level() == INTR_OFF);

    /* Mark us as running. */
    cur->status = THREAD_RUNNING;

    /* Start new time slice. */
    thread_ticks = 0;

#ifdef USERPROG
    /* Activate the new address space. */
    process_activate();
#endif

    /* If the thread we switched from is dying, destroy its thread_t.
       This must happen late so that thread_exit() doesn't pull out the rug
       under itself.  (We don't free initial_thread because its memory was
       not obtained via palloc().) */
    if (prev != NULL && prev->status == THREAD_DYING &&
        prev != initial_thread) {
        ASSERT(prev != cur);
        palloc_free_page(prev);
    }
}

/*! Schedules a new process.  At entry, interrupts must be off and the running
    process's state must have been changed from running to some other state.
    This function finds another thread to run and switches to it.

    It's not safe to call printf() until thread_schedule_tail() has
    completed. */
static void schedule(void) {
    thread_t *cur = running_thread();
    thread_t *next = next_thread_to_run();
    thread_t *prev = NULL;

    ASSERT(intr_get_level() == INTR_OFF);
    ASSERT(cur->status != THREAD_RUNNING);
    ASSERT(is_thread(next));

    if (cur != next)
        prev = switch_threads(cur, next);
    thread_schedule_tail(prev);
}

/*! Returns a tid to use for a new thread. */
static tid_t allocate_tid(void) {
    static tid_t next_tid = 1;
    tid_t tid;

    lock_acquire(&tid_lock);
    tid = next_tid++;
    lock_release(&tid_lock);

    return tid;
}

/*! Offset of `stack' member within `thread_t'.
    Used by switch.S, which can't figure it out on its own. */
uint32_t thread_stack_ofs = offsetof(thread_t, stack);

