#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "userprog/gdt.h"
#include "userprog/pagedir.h"
#include "userprog/tss.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/malloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "userprog/filemap.h"
#include "vm/frametbl.h"
#include "vm/mappings.h"

static thread_func start_process NO_RETURN;
static bool load(const char *cmdline, void (**eip)(void), void **esp);

/*! Exit code for a process which is terminated. */
#define TERMINATED ((uint32_t) -1)

/*! The word size of the machine, in bytes. */
#define WORD_SIZE 4

/*! Child registry for a parent. */
typedef struct child {
    list_elem_t elem;       /*!< List element to put in children list of parent. */
    tid_t tid;              /*!< Pid of the child. */
    semaphore_t exit;       /*!< Semaphore which starts at 0 and is upped when
                                 the process exits. */
    int exit_code;          /*!< Exit code of the child.
                                 Invalid until exit is upped. */
} child_t;

/*! Info a child process needs to start. */
typedef struct start_info {
    child_t *child;         /*!< The parent's child registry. */
    char *command;          /*!< The full command line to execute. */
    dir_t *wd;              /*!< The working directory for the child. */
    semaphore_t start;      /*!< Semaphore starting at 0 which process should up
                                 when it finishes starting (successfully or not)*/
    bool success;           /*!< Whether the process succeeded in starting.
                                 Invalid until start has been upped. */
} start_info_t;

static void register_child(child_t *child);
static child_t *pop_child(tid_t tid);

/*! Starts a new thread running a user program loaded from FILENAME.  The new
    thread must be scheduled and run before process_execute() can returns.
    Returns the new process's thread id, or TID_ERROR if the thread
    cannot be created. */
tid_t process_execute(const char *command) {
    char *cmd_copy;
    size_t cmd_len;
    char *name;
    child_t *child = NULL;
    tid_t tid = TID_ERROR;

    /* Make a copy of FILE_NAME.
       Otherwise there's a race between the caller and load(). */
    if ((cmd_copy = palloc_get_page(0)) == NULL) return TID_ERROR;

    if ((cmd_len = strlcpy(cmd_copy, command, PGSIZE)) >= PGSIZE) {
        goto fail_total;
    }

    if ((child = malloc(sizeof(child_t))) == NULL) goto fail_total;

    sema_init(&child->exit, 0);

    char* space = strchr(cmd_copy, ' ');
    size_t name_len = (space ? (size_t) (space - cmd_copy) : cmd_len) + 1;

    if ((name = malloc(name_len * sizeof(char))) == NULL) goto fail_total;

    strlcpy(name, cmd_copy, name_len);

    /* Create a new thread to execute FILE_NAME. */
    start_info_t info = { .child = child, .command = cmd_copy,
            .wd = dir_reopen(thread_current()->wd) };
    sema_init(&info.start, 0);
    // Okay to pass stack pointer to another thread because we use semaphore to
    // block until done, so we can't return and invalidate the pointer before
    // the other thread is done using it.
    tid = thread_create(name, PRI_DEFAULT, start_process, &info);
    free(name);
    if (tid == TID_ERROR) goto fail_total;

    sema_down(&info.start);
    if (!info.success) goto fail_partial;

    child->tid = tid;
    register_child(child);

    thread_unblock(get_thread(tid));

    return tid;
    fail_total:
    palloc_free_page(cmd_copy);
    fail_partial:
    free(child);
    return TID_ERROR;
}

/*! Registers a child as belonging to the current thread. */
static void register_child(child_t *child) {
    list_push_back(&thread_current()->children, &child->elem);
}

/*! Returns a handle from a tid if the given tid is a child of the current
    thread and unregisters it as a child, or NULL if it isn't. */
static child_t *pop_child(tid_t tid) {
    list_elem_t *e;
    list_t *children = &thread_current()->children;
    for (e = list_begin(children); e != list_end(children); e = list_next(e)) {
        child_t *child = list_entry(e, child_t, elem);
        if (child->tid == tid) {
            list_remove(&child->elem);
            return child;
        }
    }
    return NULL;
}

/*! Destructor function for children hash. */
static void orphan_childen(void) {
    list_elem_t *e;
    list_t *children = &thread_current()->children;
    for (e = list_begin(children); e != list_end(children); e = list_next(e)) {
        child_t *child = list_entry(e, child_t, elem);
        enum intr_level old_level = intr_disable();
        thread_t *child_thread = get_thread(child->tid);
        if (child_thread != NULL) {
            child_thread->handle = NULL;
        }
        intr_set_level(old_level);
        free(child);
    }
}

/*! Initializes userprogram related fields of a thread. Called for all threads,
    so it should only initialize things that threads need regardless of whether
    they're processes or kernel threads.*/
void process_init(thread_t *t) {
    // Any thread can have children, even if the children must be user processes
    list_init(&t->children);
    // If the thread is not a child, it needs to know to do nothing when it
    // cleans up.
    t->handle = NULL;
    t->stack_pointer = NULL;
}

/*! A thread function that loads a user process and starts it running. */
static void start_process(void *start_info_) {
    start_info_t *info = start_info_;
    char *command = info->command;
    thread_t *cur = thread_current();
    cur->handle = info->child;
    cur->wd = info->wd == NULL ? dir_open_root() : info->wd;
    intr_frame_t if_;
    bool success;

    /* Initialize interrupt frame and load executable. */
    memset(&if_, 0, sizeof(if_));
    if_.gs = if_.fs = if_.es = if_.ds = if_.ss = SEL_UDSEG;
    if_.cs = SEL_UCSEG;
    if_.eflags = FLAG_IF | FLAG_MBS;
    success = load(command, &if_.eip, &if_.esp);
    info->success = success;

    /* If load failed, quit. */
    palloc_free_page(command);
    sema_up(&info->start);
    enum intr_level old_level = intr_disable();
    thread_block();
    intr_set_level(old_level);
    if (!success) {
        process_terminate();
    }

    /* Start the user process by simulating a return from an
       interrupt, implemented by intr_exit (in
       threads/intr-stubs.S).  Because intr_exit takes all of its
       arguments on the stack in the form of a `intr_frame_t',
       we just point the stack pointer (%esp) to our stack frame
       and jump to it. */
    asm volatile ("movl %0, %%esp; jmp intr_exit" : : "g" (&if_) : "memory");
    NOT_REACHED();
}

/*! Waits for thread TID to die and returns its exit status.  If it was
    terminated by the kernel (i.e. killed due to an exception), returns -1.
    If TID is invalid or if it was not a child of the calling process, or if
    process_wait() has already been successfully called for the given TID,
    returns -1 immediately, without waiting.

    This function will be implemented in problem 2-2.  For now, it does
    nothing. */
int process_wait(tid_t child_tid) {
    child_t *child = pop_child(child_tid);
    if (child == NULL) {
        return -1;
    }
    sema_down(&child->exit);
    int exit_code = child->exit_code;
    free(child);
    return exit_code;
}

/*! Returns a pointer to the current process's file map. */
static file_map_t *process_file_map(void) {
    return &thread_current()->file_map;
}

/*! Helper function for process_close_all_fds(). Is an fm_action_func. */
static void close_file_fd(file_t *file, void *aux UNUSED) {
    file_close(file);
}

static void close_dir_fd(dir_t *dir, void *aux UNUSED) {
    dir_close(dir);
}

/*! Closes all file descriptors which this process has open.
    Assumes that the file system lock is held. */
static void close_all_fds(void) {
    filemap_destroy(process_file_map(), close_file_fd, close_dir_fd, NULL);
}

/*! Free the current process's resources. */
void process_cleanup() {
    thread_t *cur = thread_current();
    orphan_childen();
    sup_pagetable_t *pt = &cur->pt;
    if (!sup_pt_is_kernel(pt)) {
        printf("%s: exit(%d)\n", thread_name(), cur->exit_code);

        /* Close all file system accesses currently open. */
        close_all_fds();
        dir_close(cur->wd);
        file_close(cur->exec_file);

        /* Destroy the current process's page directory and switch back
        to the kernel-only page directory */
        sup_pt_destroy(pt);

        enum intr_level old_level = intr_disable();
        if (cur->handle != NULL) {
            child_t *handle = cur->handle;
            handle->exit_code = cur->exit_code;
            sema_up(&handle->exit);
        }
        intr_set_level(old_level);
    }
}


/*! Causes the current user process to exit with code STATUS. */
void process_exit(uint32_t status) {
    thread_current()->exit_code = status;
    thread_exit();
}

/*! Terminates the current user process.
    Requires access to the file system in order t oclose file descriptors,
    and so needs the read/write lock for the file system. */
void process_terminate(void) {
    process_exit(TERMINATED);
}

/*! Sets up the CPU for running user code in the current thread.
    This function is called on every context switch. */
void process_activate(void) {
    thread_t *t = thread_current();

    /* Activate thread's page tables. */
    sup_pt_activate(&t->pt);

    /* Set thread's kernel stack for use in processing interrupts. */
    tss_update();
}

/*! We load ELF binaries.  The following definitions are taken
    from the ELF specification, [ELF1], more-or-less verbatim.  */

/*! ELF types.  See [ELF1] 1-2. @{ */
typedef uint32_t Elf32_Word, Elf32_Addr, Elf32_Off;
typedef uint16_t Elf32_Half;
/*! @} */

/*! For use with ELF types in printf(). @{ */
#define PE32Wx PRIx32   /*!< Print Elf32_Word in hexadecimal. */
#define PE32Ax PRIx32   /*!< Print Elf32_Addr in hexadecimal. */
#define PE32Ox PRIx32   /*!< Print Elf32_Off in hexadecimal. */
#define PE32Hx PRIx16   /*!< Print Elf32_Half in hexadecimal. */
/*! @} */

/*! Executable header.  See [ELF1] 1-4 to 1-8.
    This appears at the very beginning of an ELF binary. */
struct Elf32_Ehdr {
    unsigned char e_ident[16];
    Elf32_Half    e_type;
    Elf32_Half    e_machine;
    Elf32_Word    e_version;
    Elf32_Addr    e_entry;
    Elf32_Off     e_phoff;
    Elf32_Off     e_shoff;
    Elf32_Word    e_flags;
    Elf32_Half    e_ehsize;
    Elf32_Half    e_phentsize;
    Elf32_Half    e_phnum;
    Elf32_Half    e_shentsize;
    Elf32_Half    e_shnum;
    Elf32_Half    e_shstrndx;
};

/*! Program header.  See [ELF1] 2-2 to 2-4.  There are e_phnum of these,
    starting at file offset e_phoff (see [ELF1] 1-6). */
struct Elf32_Phdr {
    Elf32_Word p_type;
    Elf32_Off  p_offset;
    Elf32_Addr p_vaddr;
    Elf32_Addr p_paddr;
    Elf32_Word p_filesz;
    Elf32_Word p_memsz;
    Elf32_Word p_flags;
    Elf32_Word p_align;
};

/*! Values for p_type.  See [ELF1] 2-3. @{ */
#define PT_NULL    0            /*!< Ignore. */
#define PT_LOAD    1            /*!< Loadable segment. */
#define PT_DYNAMIC 2            /*!< Dynamic linking info. */
#define PT_INTERP  3            /*!< Name of dynamic loader. */
#define PT_NOTE    4            /*!< Auxiliary info. */
#define PT_SHLIB   5            /*!< Reserved. */
#define PT_PHDR    6            /*!< Program header table. */
#define PT_STACK   0x6474e551   /*!< Stack segment. */
/*! @} */

/* Flags for p_flags.  See [ELF3] 2-3 and 2-4. @{ */
#define PF_X 1          /*!< Executable. */
#define PF_W 2          /*!< Writable. */
#define PF_R 4          /*!< Readable. */
/*! @} */

static bool setup_stack(void **esp, const char* command);
static bool validate_segment(const struct Elf32_Phdr *, file_t *);
static bool load_segment(file_t *file, off_t ofs, uint8_t *upage,
                         uint32_t read_bytes, uint32_t zero_bytes,
                         bool writable);

/*! Loads an ELF executable from FILE_NAME into the current thread.  Stores the
    executable's entry point into *EIP and its initial stack pointer into *ESP.
    Returns true if successful, false otherwise. */
bool load(const char *command, void (**eip) (void), void **esp) {
    thread_t *t = thread_current();
    struct Elf32_Ehdr ehdr;
    file_t *file = NULL;
    off_t file_ofs;
    bool success = false;
    int i;

    /* Set status. */
    t->exit_code = 0;

    /* Allocate and activate page directory. */
    if (!sup_pt_create(&t->pt)) goto done;
    process_activate();

    /* Start file descriptor map. */
    filemap_init(&t->file_map);

    /* Open executable file. */
    file = filesys_open_file(t->name, t->wd);

    if (file == NULL) {
        printf("load: %s: open failed\n", t->name);
        goto done;
    }

    /* Protect the executable file. */
    file_deny_write(file);

    /* Read and verify executable header. */
    if (file_read(file, &ehdr, sizeof ehdr) != sizeof ehdr ||
        memcmp(ehdr.e_ident, "\177ELF\1\1\1", 7) || ehdr.e_type != 2 ||
        ehdr.e_machine != 3 || ehdr.e_version != 1 ||
        ehdr.e_phentsize != sizeof(struct Elf32_Phdr) || ehdr.e_phnum > 1024) {
        printf("load: %s: error loading executable\n", t->name);
        goto done;
    }

    /* Read program headers. */
    file_ofs = ehdr.e_phoff;
    for (i = 0; i < ehdr.e_phnum; i++) {
        struct Elf32_Phdr phdr;

        if (file_ofs < 0 || file_ofs > file_length(file))
            goto done;
        file_seek(file, file_ofs);

        if (file_read(file, &phdr, sizeof phdr) != sizeof phdr)
            goto done;

        file_ofs += sizeof phdr;

        switch (phdr.p_type) {
        case PT_NULL:
        case PT_NOTE:
        case PT_PHDR:
        case PT_STACK:
        default:
            /* Ignore this segment. */
            break;

        case PT_DYNAMIC:
        case PT_INTERP:
        case PT_SHLIB:
            goto done;

        case PT_LOAD:
            if (validate_segment(&phdr, file)) {
                bool writable = (phdr.p_flags & PF_W) != 0;
                uint32_t file_page = phdr.p_offset & ~PGMASK;
                uint32_t mem_page = phdr.p_vaddr & ~PGMASK;
                uint32_t page_offset = phdr.p_vaddr & PGMASK;
                uint32_t read_bytes, zero_bytes;
                if (phdr.p_filesz > 0) {
                    /* Normal segment.
                       Read initial part from disk and zero the rest. */
                    read_bytes = page_offset + phdr.p_filesz;
                    zero_bytes = (ROUND_UP(page_offset + phdr.p_memsz, PGSIZE) -
                                 read_bytes);
                }
                else {
                    /* Entirely zero.
                       Don't read anything from disk. */
                    read_bytes = 0;
                    zero_bytes = ROUND_UP(page_offset + phdr.p_memsz, PGSIZE);
                }
                if (!load_segment(file, file_page, (void *) mem_page,
                                  read_bytes, zero_bytes, writable))
                    goto done;
            }
            else {
                goto done;
            }
            break;
        }
    }

    /* Set up stack. */
    if (!setup_stack(esp, command))
        goto done;

    /* Start address. */
    *eip = (void (*)(void)) ehdr.e_entry;

    t->exec_file = file;
    success = true;

done:
    /* We arrive here whether the load is successful or not. */
    if (!success) {
        file_close(file);
    }
    return success;
}

/* load() helpers. */

/*! Checks whether PHDR describes a valid, loadable segment in
    FILE and returns true if so, false otherwise. */
static bool validate_segment(const struct Elf32_Phdr *phdr, file_t *file) {
    /* p_offset and p_vaddr must have the same page offset. */
    if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK))
        return false;

    /* p_offset must point within FILE. */
    if (phdr->p_offset > (Elf32_Off) file_length(file))
        return false;

    /* p_memsz must be at least as big as p_filesz. */
    if (phdr->p_memsz < phdr->p_filesz)
        return false;

    /* The segment must not be empty. */
    if (phdr->p_memsz == 0)
        return false;

    /* The virtual memory region must both start and end within the
       user address space range. */
    if (!is_user_vaddr((void *) phdr->p_vaddr))
        return false;
    if (!is_user_vaddr((void *) (phdr->p_vaddr + phdr->p_memsz)))
        return false;

    /* The region cannot "wrap around" across the kernel virtual
       address space. */
    if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
        return false;

    /* Disallow mapping page 0.
       Not only is it a bad idea to map page 0, but if we allowed it then user
       code that passed a null pointer to system calls could quite likely panic
       the kernel by way of null pointer assertions in memcpy(), etc. */
    if (phdr->p_vaddr < PGSIZE)
        return false;

    /* It's okay. */
    return true;
}

/*! Loads a segment starting at offset OFS in FILE at address UPAGE.  In total,
    READ_BYTES + ZERO_BYTES bytes of virtual memory are initialized, as follows:

        - READ_BYTES bytes at UPAGE must be read from FILE
          starting at offset OFS.

        - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.

    The pages initialized by this function must be writable by the user process
    if WRITABLE is true, read-only otherwise.

    Return true if successful, false if a memory allocation error or disk read
    error occurs. */
static bool load_segment(file_t *file, off_t f_ofs, uint8_t *upage,
                         uint32_t read_bytes, uint32_t zero_bytes,
                         bool writable) {
    ASSERT((read_bytes + zero_bytes) % PGSIZE == 0);
    ASSERT(pg_ofs(upage) == 0);
    ASSERT(f_ofs % PGSIZE == 0);
    sup_pagetable_t *pt = &thread_current()->pt;

    while (read_bytes > 0 || zero_bytes > 0) {
        /* Calculate how to fill this page.
           We will read PAGE_READ_BYTES bytes from FILE
           and zero the final PAGE_ZERO_BYTES bytes. */
        size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
        size_t page_zero_bytes = PGSIZE - page_read_bytes;
        if (page_read_bytes > 0) {
            if (!vm_set_page(pt, upage, MAP_WRITE * writable,
                                file, f_ofs, page_read_bytes)) {
                return false;
            }
        } else {
            if (!vm_set_page(pt, upage, MAP_WRITE * writable, NULL, 0, 0)) {
                return false;
            }
        }

        /* Advance. */
        read_bytes -= page_read_bytes;
        zero_bytes -= page_zero_bytes;
        upage += PGSIZE;
        f_ofs += PGSIZE;
    }
    return true;
}

static bool cleanup_stack_page(void **esp, uint8_t *kpage, char** argv, char** argv_addresses){
    if ((uint8_t *)PHYS_BASE-(uint8_t *)esp > PGSIZE){
                    palloc_free_page(kpage);
                    free(argv);
                    free(argv_addresses);
                    return true;
    }
    return false;
}

/*! Create a minimal stack by mapping a zeroed page at the top of
    user virtual memory. */
static bool setup_stack(void **esp, const char* command) {
    uint8_t *kpage;
    char* process_name;
    char* reentrant_arg;
    char** argv = malloc(sizeof(char*));
    size_t argc = 0;
    sup_pagetable_t *pt = &thread_current()->pt;

    kpage = vm_set_load_stack_page(pt, USER_STACK_BASE - PGSIZE)->bytes;
    if (kpage == NULL) return false;
    *esp = PHYS_BASE;

    for (process_name = strtok_r((char *)command, " ", &reentrant_arg);
        process_name != NULL;
        process_name = strtok_r(NULL, " ", &reentrant_arg)){
        argv = realloc (argv, (argc+1)*sizeof(char*));
        argv[argc++] = process_name;
    }

    char** argv_addresses = malloc((argc+1)*sizeof(char*));

    // Put each argument string on the stack
    for (size_t args = argc; args; args--){
        size_t arg_length = strlen(argv[args - 1]) + 1;
        *esp = (void *) ((char *) *esp - arg_length);
        if (cleanup_stack_page(esp, kpage, argv, argv_addresses)){
            return false;
        }
        argv_addresses[args-1] = *esp;
        memcpy (*esp, argv[args-1], arg_length);
    }

    // Puts word alignment bytes on the stack as needed
    size_t align = (size_t) *esp % WORD_SIZE;
    *esp = (void *) ((uint8_t *)*esp - align);
    if (cleanup_stack_page(esp, kpage, argv, argv_addresses)){
            return false;
        }
    for (size_t i = 0; i < align; i++) {
        uint8_t *i_ptr = (uint8_t*) *esp + i;
        *i_ptr = 0;
    }

    // Start the next word
    argv_addresses[argc] = 0;

    // Put pointers to each argument string on the stack
    for (int32_t args = argc; args >= 0; args--) {
        *esp = (void *) ((char *) *esp - WORD_SIZE);
        if (cleanup_stack_page(esp, kpage, argv, argv_addresses)){
            return false;
        }
        memcpy((char **) *esp, &argv_addresses[args], sizeof(char *));
    }

    // Put pointer to argument pointer array on the stack
    argv_addresses[0] = *esp;
    *esp = (void *) ((char **) *esp - 1);
    if (cleanup_stack_page(esp, kpage, argv, argv_addresses)){
            return false;
        }
    memcpy(*esp, &argv_addresses[0], sizeof(char*));

    // Put arg count on the stack
    *esp = (void *) ((int32_t *) *esp - 1);
    if (cleanup_stack_page(esp, kpage, argv, argv_addresses)){
            return false;
        }
    memcpy(*esp, &argc, sizeof(int));

    // Put return address of zero on the stack
    *esp = (void *) ((uint32_t *) *esp - 1);
    if (cleanup_stack_page(esp, kpage, argv, argv_addresses)){
            return false;
        }
    *(uint32_t*)(*esp) = 0;

    free(argv_addresses);
    frametbl_unpin_frame((frame_t *) kpage);

    // hex_dump(0, *esp, 100, true);
    free(argv);
    return true;
}

/*! Instructs the current thread to create a file descriptor for FILE.
    Returns the new file descriptor, or FM_ERROR on error.
    Does NOT interact with the file system itself.*/
uint32_t process_create_fd(void *file, bool is_dir) {
    return filemap_insert(process_file_map(), file, is_dir);
}

/*! Gets the file corresponding to this file descriptor, according to the
    current thread. Returns the file on success, or NULL on error.
    Does NOT interact with the file system itself.
    On success, sets *IS_DIR to whether or not it's a directory. */
void *process_get_file(uint32_t fd, bool *is_dir) {
    return filemap_get(process_file_map(), fd, is_dir);
}

/*! Removes the given file descriptor, according to the current thread.
    Returns the file it used to point to on success, or NULL on error.
    On success, modifies *IS_DIR to indicate if it was a directory or not.
    Does NOT interact with the file system itself. */
void *process_remove_fd(uint32_t fd, bool *is_dir) {
    return filemap_remove(process_file_map(), fd, is_dir);
}