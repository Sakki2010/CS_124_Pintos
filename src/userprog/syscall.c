#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "userprog/exception.h"
#include "devices/shutdown.h"
#include "filesys/filesys.h"
#include "userprog/process.h"
#include "userprog/pagedir.h"
#include "filesys/file.h"
#include "devices/input.h"
#include "vm/mappings.h"

/*! The word size of the machine, in bytes. */
#define WORD_SIZE 4

/*! Limits on string lengths. */
#define MAX_PATH_LEN 4096

/*! Returned by some syscalls on error. */
#define SC_ERR ((uint32_t) -1)

/*! Maximum bytes which can be printed at once before the buffer is broken up.*/
static const uint32_t MAX_PRINT = 1024;

static void syscall_handler(intr_frame_t *);

/*! Initializes the syscall system. */
void syscall_init(void) {
    intr_register_int(0x30, 3, INTR_ON, syscall_handler, "syscall");
}

/*! Reads a byte at user virtual address UADDR.
    UADDR must be below PHYS_BASE.
    Returns the byte value if successful, PF_ERR if a segfault occurred. */
static uint32_t _get_user(const uint8_t *uaddr) {
    uint32_t result;
    asm ("movl $1f, %0; movzbl %1, %0; 1:"
         : "=&a" (result) : "m" (*uaddr));
    return result;
}

/*! Writes BYTE to user address UDST.
    UDST must be below PHYS_BASE.
    Returns true if successful, false if a segfault occurred. */
static bool _set_user(uint8_t *udst, uint8_t byte) {
    int error_code;
    asm ("movl $1f, %0; movb %b2, %1; 1:"
         : "=&a" (error_code), "=m" (*udst) : "q" (byte));
    return error_code != -1;
}

/*! Reads a byte from virtual address UADDR. Should be a location in user
    space. Returns the byte value on success, and PF_ERR on segfault
    or other invalid access. */
static uint32_t get_user8(const uint8_t *uaddr) {
    if (is_kernel_vaddr(uaddr)) {
        return PF_ERR;
    }
    return _get_user(uaddr);
}

/*! Reads a word (32 bits) from virtual address UADDR. Should be a location in
    user space. Returns the word value on success, and PF_ERR on segfault
    or other invalid access. */
static uint32_t get_user32(const uint8_t *uaddr) {
    uint32_t result = 0;
    for (uint8_t i = 0; i < WORD_SIZE; i++) {
        uint32_t by = get_user8(uaddr + i);
        if (by == PF_ERR) {
            return PF_ERR;
        }
        result = result | (by << 8 * i);
    }
    return result;
}

/*! Writes BYTE to virtual address UDST. Should be a location in user space.
    Returns true on success, and false on segfault or other invalid access. */
static bool set_user(uint8_t *udst, uint8_t byte) {
    if (is_kernel_vaddr(udst)) {
        return false;
    }
    return _set_user(udst, byte);
}

/*! Invoked in a syscall context to set the return value for that syscall.
    Modifies eax, so that value should be stored first if needed. */
static void set_return(intr_frame_t *f, uint32_t val) {
    f->eax = val;
}

/*! Gets the number of the system call which was invoked.
    Returns PF_ERR on segfault or other invalid access. */
static uint32_t get_syscall_num(intr_frame_t *f) {
    return get_user32((uint8_t *) f->esp);
}

/*! Gets the Nth argument to the syscall which was invoked.
    Returns PF_ERR on segfault or other invalid access. */
static uint32_t get_arg(intr_frame_t *f, size_t n) {
    uint8_t *addr = ((uint8_t *) f->esp) +
        WORD_SIZE * (n + 1); // First is syscall num
    return get_user32(addr);
}

static bool test_read(uint8_t *uaddr) {
    return get_user8(uaddr) != PF_ERR;
}

static bool test_write(uint8_t *uaddr) {
    return set_user(uaddr, get_user8(uaddr));
}

/*! Verifies that a buffer pointer is a valid one in user space, by attempting
    to read from it and each subsequent page until SIZE.
    Returns true if the entire buffer is valid, false otherwise. */
static bool verify_buffer(char *buffer, uint32_t size, bool write) {
    bool (*test_byte)(uint8_t *uaddr) = write ? test_write : test_read;

    // Verify a pointer on each subsequent page
    uint8_t *end = (uint8_t *) buffer + size;
    uint8_t *start = (uint8_t *) buffer;
    for (uint8_t *i = start; i < end; i = pg_round_down(i + PGSIZE)) {
        if (!test_byte(i)) return false;
    }
    return true;
}

/*! Verifies a string which is in user space is valid to access. Returns
    its length (ie: until the first null terminator), or SC_ERR on
    page fault or if its length is > MAX_SIZE. */
static uint32_t valid_str_len(char *str, const uint32_t max_size) {
    for (uint32_t i = 0; i <= max_size; i++) {
        char *ptr = str + i;

        // Try calling `get` on each character in the string
        uint32_t letter = get_user8((uint8_t *) ptr);
        if (letter == PF_ERR) {
            process_terminate();
        } else if ((char) letter == '\0') {
            return i;
        }
    }

    return SC_ERR;
}

/*! Pins the given buffer, ensuring it will not be swapped out under the kernel.
    Assumes it is valid. */
static void pin_buffer(void *buffer, uint32_t size) {
    sup_pagetable_t *pt = &thread_current()->pt;
    uintptr_t start = pg_no(buffer);
    uintptr_t end = pg_no(buffer + size - 1);
    size_t n = end - start + 1;
    vm_pin_pages(pt, (void *) (start * PGSIZE), n);
}
/*! Pins the given str, ensuring it will not be swapped out under the kernel.
    Assumes it is valid. */
static void pin_str(char *str) {
    sup_pagetable_t *pt = &thread_current()->pt;
    uintptr_t start = pg_no(str);
    uintptr_t end = pg_no(strchr(str, '\0'));
    size_t n = end - start + 1;
    vm_pin_pages(pt, (void *) (start * PGSIZE), n);
}
/*! Unpins the given buffer, allowing it to be swapped again. Should have been
    passed to pin_buffer before. */
static void unpin_buffer(void *buffer, uint32_t size) {
    sup_pagetable_t *pt = &thread_current()->pt;
    uintptr_t start = pg_no(buffer);
    uintptr_t end = pg_no(buffer + size - 1);
    size_t n = end - start + 1;
    vm_unpin_pages(pt, (void *) (start * PGSIZE), n);
}
/*! Unpins the given string, allowing it to be swapped again. Should have been
    passed to pin_str before. */
static void unpin_str(char *str) {
    sup_pagetable_t *pt = &thread_current()->pt;
    uintptr_t start = pg_no(str);
    uintptr_t end = pg_no(strchr(str, '\0'));
    size_t n = end - start + 1;
    vm_unpin_pages(pt, (void *) (start * PGSIZE), n);
}

/*! Invoked by the syscall `void halt(void)` */
static void sys_halt(void) {
    shutdown_power_off();
}

/*! Invoked by the syscall `void exit(int status)` */
static void sys_exit(uint32_t status) {
    process_exit(status);
}

/*! Invoked by the syscall `pid_t exec(const char *cmd_line)` */
static uint32_t sys_exec(char *cmd_line) {
    if (valid_str_len(cmd_line, PGSIZE) == SC_ERR) {
        return SC_ERR;
    }
    return process_execute(cmd_line);
}

/*! Invoked by the syscall `int wait(pid_t pid)` */
static uint32_t sys_wait(uint32_t pid) {
    return process_wait(pid);
}

/*! Invoked by the syscall
    `bool create (const char *path, unsigned initial_size)` */
static bool sys_create(char *path, uint32_t initial_size) {
    if (valid_str_len(path, MAX_PATH_LEN) == SC_ERR) {
        return false;
    }

    pin_str(path);
    bool success = filesys_create_file(path, initial_size,
            thread_current()->wd);
    unpin_str(path);

    return success;
}

/*! Invoked by the syscall `bool remove (const char *path)` */
static bool sys_remove(char *path) {
    if (valid_str_len(path, MAX_PATH_LEN) == SC_ERR) {
        return false;
    }

    pin_str(path);
    bool success = filesys_remove(path, thread_current()->wd);
    unpin_str(path);

    return success;
}

/*! Invoked by the syscall `int open (const char *path)` */
static uint32_t sys_open(char *path) {
    if (valid_str_len(path, MAX_PATH_LEN) == SC_ERR) {
        process_terminate();
    }

    bool is_dir = false;
    pin_str(path);
    void *file = filesys_open(path, thread_current()->wd, &is_dir);
    unpin_str(path);
    
    return (file == NULL) ? SC_ERR : process_create_fd(file, is_dir);
}

/*! Invoked by the syscall `int filesize (int fd)` */
static uint32_t sys_filesize(uint32_t fd) {
    bool is_dir;
    void *file = process_get_file(fd, &is_dir);

    if (file == NULL || is_dir) {
        return SC_ERR;
    }

    off_t size = file_length((file_t *) file);

    return (uint32_t) size;
}

/*! Invoked by the syscall `int read (int fd, void *buffer, unsigned size)` */
static uint32_t sys_read(uint32_t fd, char *buffer, off_t size) {
    if (fd == STDIN_FILENO) {
        for (off_t i = 0; i < size; i++) {
            if (!set_user((uint8_t *) (buffer + i), input_getc())) {
                return SC_ERR; // page fault
            }
        }
        return size; // otherwise, this always eventually succeeds
    }
    else {
        if (!verify_buffer(buffer, size, true)) {
            process_terminate();
        }
        bool is_dir;
        void *file = process_get_file(fd, &is_dir);

        if (file == NULL || is_dir) {
            return SC_ERR;
        }

        pin_buffer(buffer, size);
        off_t read = file_read((file_t *) file, (void *) buffer, size);
        unpin_buffer(buffer, size);

        return (uint32_t) read;
    }
}

/*! Invoked by the syscall
    `int write (int fd, const void *buffer, unsigned size)` */
static uint32_t sys_write(uint32_t fd, char *buffer, uint32_t size) {
    if (!verify_buffer(buffer, size, false)) {
        process_terminate();
    }

    if (fd == STDOUT_FILENO) {
        // Number of completely full buffers to print to the console
        uint32_t num_long = size / MAX_PRINT;
        for (uint32_t i = 0; i < num_long; i++) {
            putbuf(buffer, MAX_PRINT);
            buffer += MAX_PRINT;
        }
        putbuf(buffer, size - num_long * MAX_PRINT);
        return size; // putbuf always succeeds
    } else {
        bool is_dir;
        void *file = process_get_file(fd, &is_dir);

        if (file == NULL || is_dir) {
            return SC_ERR;
        }

        pin_buffer(buffer, size);
        off_t written = file_write((file_t *) file, buffer, (off_t) size);
        unpin_buffer(buffer, size);

        return (uint32_t) written;
    }
}

/*! Invoked by the syscall `void seek (int fd, unsigned position)` */
static void sys_seek(uint32_t fd, off_t position) {
    bool is_dir;
    void *file = process_get_file(fd, &is_dir);

    if (file == NULL || is_dir) {
        process_terminate();
    } else {
        // Unprotected, since it only affects the file_t *, not the file system
        file_seek((file_t *) file, position);
    }
}

/*! Invoked by the syscall `unsigned tell(int fd)` */
static uint32_t sys_tell(uint32_t fd) {
    bool is_dir;
    void *file = process_get_file(fd, &is_dir);

    if (file == NULL || is_dir) {
        return SC_ERR;
    }
    // Unprotected, since it only affects the file_t *, not the file system
    return (uint32_t) file_tell((file_t *) file);
}

/*! Invoked by the syscall `void close (int fd)` */
static void sys_close(uint32_t fd) {
    bool is_dir;
    void *file = process_remove_fd(fd, &is_dir);

    if (file == NULL) {
        process_terminate();
    } else if (is_dir) {
        dir_close((dir_t *) file);
    } else {
        file_close((file_t *) file);
    }
}

/*! Invoked by the syscall `mapid_t mmap(int fd, void *addr)` */
static uintptr_t sys_mmap(uint32_t fd, void *addr) {
    if (pg_ofs(addr) != 0 || pg_no(addr) == 0) return SC_ERR;

    bool is_dir;
    void *file = process_get_file(fd, &is_dir);
    if (file == NULL || is_dir) return SC_ERR;

    size_t len = file_length((file_t *) file);
    if (len == 0) return SC_ERR;

    thread_t *cur = thread_current();

    sup_pagetable_t *pt = &cur->pt;

    if (addr + len >= cur->stack_pointer) return SC_ERR;

    for (void *upage = addr; upage < addr + len; upage += PGSIZE) {
        if (!vm_page_is_mappable(pt, upage)) return SC_ERR;
    }

    for (void *upage = addr; upage < addr + len; upage += PGSIZE) {
        uint32_t flags = (upage == addr) * MAP_START | MAP_FWRITE | MAP_WRITE;
        size_t remaining = len - (upage - addr);
        size_t size = remaining < PGSIZE ? remaining : PGSIZE;
        if (!vm_set_page(pt, upage, flags, (file_t *) file, upage - addr, size)) {
            return SC_ERR;
        }
    }

    // Like real mmap, we simply use the address as the mapid_t
    return (uintptr_t) addr;
}

/*! Invoked by the syscall `void munmap(mapid_t mapping)` */
static void sys_munmap(uintptr_t mapping) {
    void *first = (void *) mapping;
    sup_pagetable_t *pt = &thread_current()->pt;

    if (!vm_page_is_mapping_start(pt, first)) process_terminate();
    void *last = vm_page_get_mapping_end(pt, first);

    for (void *upage = first; upage <= last; upage += PGSIZE) {
        vm_clear_page(pt, upage);
    }
}

/*! Invoked by the syscall `bool mkdir (const char *dir)` */
static bool sys_mkdir(char *path) {
    if (valid_str_len(path, MAX_PATH_LEN) == SC_ERR) {
        process_terminate();
    }

    pin_str(path);
    bool success = filesys_create_dir(path, thread_current()->wd);
    unpin_str(path);

    return success;
}

/*! Invoked by the syscall `bool chdir (const char *dir)` */
static bool sys_chdir(char *path) {
    if (valid_str_len(path, MAX_PATH_LEN) == SC_ERR) {
        process_terminate();
    }

    pin_str(path);
    dir_t * new_dir = filesys_open_dir(path, thread_current()->wd);
    unpin_str(path);

    if (new_dir == NULL) return false;

    dir_close(thread_current()->wd);
    thread_current()->wd = new_dir;
    return true;
}

/*! Invoked by the syscall `bool isdir (int fd)` */
static bool sys_isdir(uint32_t fd) {
    bool ret;
    void *dir_or_file = process_get_file(fd, &ret);
    if (dir_or_file == NULL) {
        process_terminate();
    }
    return ret;
}

/*! Invoked by the syscall `bool readdir (int fd, char* name)` */
static bool sys_readdir(uint32_t fd, char *name){
    bool is_dir;
    void *dir_or_file = process_get_file(fd, &is_dir);
    if (dir_or_file == NULL) {
        process_terminate();
    }
    if (is_dir) {
        return dir_readdir((dir_t *) dir_or_file, name);
    }
    return false;
}

/*! Invoked by the syscall `bool inumber (int fd)` */
static uint32_t sys_inumber(uint32_t fd){
    bool is_dir;
    void *dir_or_file = process_get_file(fd, &is_dir);
    if (dir_or_file == NULL) {
        process_terminate();
    }
    if (is_dir) {
        return inode_get_inumber(dir_get_inode((dir_t *) dir_or_file));
    }
    else{
        return inode_get_inumber(file_get_inode((file_t *) dir_or_file));
    }
    return SC_ERR;
}



/*! Registered handler for system calls. */
static void syscall_handler(intr_frame_t *f) {
    thread_current()->stack_pointer = f->esp;

    uint32_t num = get_syscall_num(f);
    if (num == PF_ERR) {
        process_terminate();
    }

    /*! Macros to assist with interfacing with a syscall.
        All assume that an `intr_frame_t*` named `f` is present in the scope,
        and will error otherwise. */
    #define RET(x) set_return(f, (x))
    #define ARG0 (get_arg(f, 0))
    #define ARG1 (get_arg(f, 1))
    #define ARG2 (get_arg(f, 2))

    switch (num) {
        case SYS_HALT: sys_halt(); break;
        case SYS_EXIT: sys_exit(ARG0); break;
        case SYS_EXEC: RET((uint32_t) sys_exec((char *) ARG0)); break;
        case SYS_WAIT: RET(sys_wait((tid_t) ARG0)); break;
        case SYS_CREATE: RET((uint32_t) sys_create((char *) ARG0, ARG1)); break;
        case SYS_REMOVE: RET((uint32_t) sys_remove((char *) ARG0)); break;
        case SYS_OPEN: RET(sys_open((char *) ARG0)); break;
        case SYS_FILESIZE: RET(sys_filesize(ARG0)); break;
        case SYS_READ: RET(sys_read(ARG0, (char *) ARG1, (off_t) ARG2)); break;
        case SYS_WRITE: RET(sys_write(ARG0, (char *) ARG1, ARG2)); break;
        case SYS_SEEK: sys_seek(ARG0, (off_t) ARG1); break;
        case SYS_TELL: RET(sys_tell(ARG0)); break;
        case SYS_CLOSE: sys_close(ARG0); break;
        case SYS_MMAP: RET(sys_mmap(ARG0, (void *) ARG1)); break;
        case SYS_MUNMAP: sys_munmap(ARG0); break;
        case SYS_MKDIR: RET(sys_mkdir((char *) ARG0)); break;
        case SYS_CHDIR: RET(sys_chdir((char *) ARG0)); break;
        case SYS_ISDIR: RET(sys_isdir(ARG0)); break;
        case SYS_READDIR: RET(sys_readdir(ARG0, (char *) ARG1)); break;
        case SYS_INUMBER: RET(sys_inumber(ARG0)); break;
        default: process_terminate(); // Invalid syscall
    }

    #undef RETURN
    #undef ARG0
    #undef ARG1
    #undef ARG2

    thread_current()->stack_pointer = NULL;
}