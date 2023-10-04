#ifndef USERPROG_PROCESS_H
#define USERPROG_PROCESS_H

// Silence IDE errors about userprog thread fields.
#ifndef USERPROG
#define USERPROG
#endif

#include "threads/thread.h"

#define MAX_USER_STACK (512 * PGSIZE)
#define USER_STACK_BASE PHYS_BASE

tid_t process_execute(const char *command);
int process_wait(tid_t);
void process_activate(void);

void process_init(thread_t *t);
void process_cleanup(void);
void process_exit(uint32_t status) NO_RETURN;
void process_terminate(void) NO_RETURN;

uint32_t process_create_fd(void *file, bool is_dir);
void *process_get_file(uint32_t fd, bool *is_dir);
void *process_remove_fd(uint32_t fd, bool *is_dir);

#endif /* userprog/process.h */

