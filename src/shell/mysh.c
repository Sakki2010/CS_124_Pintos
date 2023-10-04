#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/errno.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <fcntl.h>
#include <pwd.h>
#include <assert.h>
#include <readline/readline.h>
#include <readline/history.h>

#include "tokenizer.h"
#include "lexer.h"
#include "err_utils.h"

/**
 * @brief Collects the file descriptors for a given process created by the shell
*/
typedef struct {
    // For all file descriptors, DEFAULT_FD indicates a default behavior
    int fd_in; // File descriptor for input stream (default: STDIN_FILENO)
    int fd_out; // output stream (default: STDOUT_FILENO)
    int fd_err; // error stream (default: STDERR_FILENO)

    // Up to two file descriptors for this process to close immediately 
    // on its creation, for use with pipes (default: doesn't close)
    int fd_close[2];

    char *path; // The path to this process's command
    char **argv; // The arguments for this process, NULL-terminated as standard
} process_t;

/**
 * @brief Represents an unreaped command (either actively running or finished
 * but not yet cleaned up), keeping track of all its pids.
 */
typedef struct {
    pid_t last; // The pid of the last process in the pipe
    size_t num_running; // number of still running processes
    pid_t *running; // array of pids of still running processes
    char *command; // The user-provided text of the original command, or NULL
    char status; // The wait status of the command, undefined unless
                 // num_running is 0 (i.e., command is done)
} unreaped_command_t;

// Maximum number of history entries stored
#define MAX_HISTORY 1000
// File to story history in
const char* HISTORY_FILE = "mysh_history";

// Mode which output files are opened in
#define DEFAULT_FILE_MODE S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH

// For pipes
#define WRITE_END 0
#define READ_END 1

// Value indicating a file descriptor should be treated as default;
// see `process_t` for more detail
#define DEFAULT_FD -1

#define SUCCESS_CODE 0
#define ERR_CODE -1 // Returned when an internal error occurs
#define SERR_CODE -2 // Returned when an error message message was already
                     // printed and so futher failure should be silent
#define EXIT_CODE -3 // Returned to indicate that the shell should exit

#define MAX_PATH_LENGTH 4096 // Linux standard, with space for null terminator

// The currently running foreground command
unreaped_command_t *foreground = NULL;
// number of currently unreported background processes
size_t num_background = 0;
// array of currently unreported background processes
unreaped_command_t **background = NULL;

/**
 * @brief Error checks the evaluation of system calls inside a child,
 * which is supposed to exit(errno) on such failures.
 * 
 * @param out The value returned by the syscall to verify
 * @return The value returned by the syscall being verified
 */
int child_checked(int out) {
    if (out == -1) {
        exit(errno);
    }
    return out;
}

/**
 * @brief Attempts to clean up after a finished process in a given command.
 * 
 * Does nothing if the pid is not one of the running pids in that command.
 * 
 * @param pid The finished pid.
 * @param status The status set by wait
 * @param cmd The command in which to attempt cleanup
 * 
 * @return whether the pid was in the command
 */
bool cleanup_in_command(pid_t pid, int status, unreaped_command_t *cmd) {
    if (pid == cmd->last) {
        // if this is the last command in the pipe, we want to set the overall
        // exit status to its exit status
        fprintf(stderr, "Setting command status %d.\n", status);
        cmd->status = status;
    }
    for (size_t i = 0; i < cmd->num_running; i++) {
        // if we find the pid in the commands list of running pids, swap remove
        // it (we do not care about order) and finish.
        if (pid == cmd->running[i]) {
            cmd->running[i] = cmd->running[--cmd->num_running];
            return true;
        }
    }
    return false;
}

/**
 * @brief Reports a failure to clean up after a finished process.
 * 
 * This function is signal safe, as opposed to `fprintf(...)`.
 * 
 * @param pid The pid which couldn't be cleaned up
 * @param status The status wait gave for that pid. It is assumed that either
 * `WIFEXITED(status)` or `WIFSIGNALED(status)` are true.
 */
void report_cleanup_error(pid_t pid, int status) {
    fprintf(stderr, "The shell lost track of child [%d].\n", pid);
    if (WIFEXITED(status)) {
        fprintf(stderr, "It exited naturally with exit code %d.\n", 
                        WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        fprintf(stderr, "It exited due to receiving signal %d.\n",
                        WTERMSIG(status));
    }
}

/**
 * @brief Clean up after a finished process.
 * 
 * @param pid The finished pid.
 * @param status The status set by wait
 * @param cmd The command in which to attempt cleanup
 * 
 * @return whether the pid was in the command
 */
void cleanup(pid_t pid, int status) {
    for (size_t i = 0; i < num_background; i++) {
        if (cleanup_in_command(pid, status, background[i])) {
            return;
        }
    }
    
    report_cleanup_error(pid, status);
}

/**
 * @brief Reaps all outstanding corpses of children and cleans them up.
 */
void reap() {
    pid_t pid;
    int status;
    while ((pid = waitpid(-1, &status, WNOHANG | WUNTRACED)) > 0) {
        if (WIFEXITED(status) || WIFSIGNALED(status)) {
            CLEANUP:
            cleanup(pid, status);
        } else if (WIFSTOPPED(status) && WSTOPSIG(status) == SIGTTIN) {
            fprintf(stderr, "[Background process %d tried to read from terminal."
                            " Killing.]\n", pid);
            // Because our shell provides no mechanism to attach background
            // processes to foreground, a process which stops because it tried
            // to read standard in will never be able to recover. Thus, we
            // kill it instead.
            kill(pid, SIGKILL);
            // Because the kill has to go through the kernel, reap() could
            // finish before this process is dead. Thus, we directly wait for it
            // and go to cleanup to ensure it's handled on this call to reap()
            // rather than the next.
            while (waitpid(pid, &status, 0) == -1 && errno == EINTR) {}
            goto CLEANUP;
        }
    }
}

/**
 * @brief SIGINT and SIGTSTP handler which forwards the signal to 
 * non-background children.
 * 
 * @param signum The signal sent
 */
void sig_forward(int signum) {
    if (foreground == NULL) {
        return;
    }
    for (size_t i = 0; i < foreground->num_running; i++) {
        kill(foreground->running[i], signum);
    }
}

/**
 * @brief Changes one of the current thread's streams
 * Implemented by assigning file descriptors.
 * 
 * @param fd1 The file descriptor to overwrite (and close)
 * @param fd2 The file descriptor to use in its place
 * @param close_old Whether or not to close the old file descriptor
 *      Generally should be true, unless the descriptor is getting reused
*/
void replace_fd(int fd1, int fd2, bool close_old) {
    child_checked(dup2(fd1, fd2));
    if (close_old) {
        child_checked(close(fd1));
    }
}

/**
 * @brief Applies the requested updates to file descriptors
 * Affects only the current process
 * @param com The process object containing the requested file descriptors
*/
void update_fds(process_t *com) {
    if (com->fd_in != -1) {
        replace_fd(com->fd_in, STDIN_FILENO, true);
    }
    if (com->fd_out != -1) {
        replace_fd(com->fd_out, STDOUT_FILENO, com->fd_out != com->fd_err);
    }
    if (com->fd_err != -1) {
        replace_fd(com->fd_err, STDERR_FILENO, true);
    }
    for (size_t i = 0; i < 2; i++) {
        if (com->fd_close[i] != -1) {
            child_checked(close(com->fd_close[i]));
        }
    }
}

/**
 * @brief Opens a file on behalf of the given output node
 * 
 * @param com The governing process
 * @param node The node to open the file corresponding to
 * @return The new file descriptor for that file
*/
int open_output_file(output_node_t *node) {
    char *fpath = node->path;

    int flags = O_WRONLY | O_CREAT;
    if (node->append) {
        flags = flags | O_APPEND;
    } else {
        flags = flags | O_TRUNC;
    }

    return child_checked(open(fpath, flags, DEFAULT_FILE_MODE));
}

/**
 * @brief Creates a process object corresponding to a given command node
 * Borrows the path and arguments immutably
 * 
 * @param node A command node to start a process for
 * @param prev_pipes An array of file descriptors corresponding to those of the
 *      pipe preceding this node, or NULL if there isn't one
 * @param next_pipes As with `prev_pipes`, but for the subsequent pipe
 * @return The new process object
 */
process_t *create_process(command_node_t *node, int prev_pipes[2],
        int next_pipes[2]) {
    process_t *com = checked_malloc(sizeof(process_t));

    com->path = node->path;
    com->argv = node->args;
    com->fd_close[WRITE_END] = com->fd_close[READ_END] = -1;

    switch (node->input_node->type) {
        case INPUT: { // open file
            char *fpath = node->input_node->path;
            com->fd_in = child_checked(open(fpath, O_RDONLY));
            break;
        }
        case COMMAND: { // link up pipes
            com->fd_in = prev_pipes[WRITE_END];
            com->fd_close[READ_END] = prev_pipes[READ_END];
            break;
        }
        default:
        case SHELLIN: {
            com->fd_in = -1;
            break;
        }
    }

    if (node->error_node != STREAM_DUPLICATED) {
        switch (node->error_node->type) {
            case OUTPUT: {
                com->fd_err = 
                    open_output_file((output_node_t *) node->error_node);
                break;
            }
            default:
            case SHELLOUT: { // normal error stream
                com->fd_err = -1;
                break;
            }
        }
    }

    if (node->output_node != STREAM_DUPLICATED) {
        switch (node->output_node->type) {
            case OUTPUT: {
                com->fd_out = 
                    open_output_file((output_node_t *) node->output_node);
                break;
            }
            case COMMAND: { // link up pipes
                com->fd_out = next_pipes[READ_END];
                com->fd_close[WRITE_END] = next_pipes[WRITE_END];
                break;
            }
            default:
            case SHELLOUT: {
                com->fd_out = -1;
                break;
            }
        }
    }

    if (node->output_node == STREAM_DUPLICATED) {
        com->fd_out = com->fd_err;
    }
    if (node->error_node == STREAM_DUPLICATED) {
        com->fd_err = com->fd_out;
    }

    return com;
}

/**
 * @brief Prints the contents of history, numbered from 1, to STDOUT.
 */
void print_history() {
    int i = 1;
    history_set_pos(0);
    for (HIST_ENTRY *entry = current_history(); entry != NULL;
         entry = next_history()) {
        printf("%3d: %s\n", i++, entry->line);
    }
}

/**
 * @brief Starts a process running on a new thread
 * 
 * Creates a process object with its file descriptors, based on a node
 * 
 * @param pid_ptr Value will be set to the pid of the parent thread
 * @param node A command node to start a thread for
 * @param prev_pipes An array of file descriptors corresponding to those of the
 *      pipe preceding this node, or NULL if there isn't one
 * @param next_pipes As with `prev_pipes`, but for the subsequent pipe
 * @param decouple_stdin Indicates whether the stdin of this process should be
 *      decoupled from the shell's stdin
 * @return pid_t the pid of the child or -1 on error. errno is set if an error
 * occurs.
 */
pid_t start_process(command_node_t *node, int prev_pipes[2],
        int next_pipes[2], bool decouple_stdin) {

    pid_t pid = fork();
    if (pid != 0) { // parent (shell) goes back
        return pid;
    }

    if (node->builtin == HISTORY) {
        print_history();
        exit(SUCCESS_CODE);
    } else {
        // otherwise, create child
        process_t *com = create_process(node, prev_pipes, next_pipes);
        if (com->fd_in == -1 && decouple_stdin) {
            setpgid(0, getpid());
        }
        update_fds(com);

        execvp(com->path, com->argv);

        exit(errno);
    }
}

/**
 * @brief Creates all pipe file descriptors needed for an AST
 * 
 * @param ast The AST to create pipes for
 * @param fds An empty array, dimensions [number of pipes][2] which will
 *      be populated with the file descriptors of each pipe, in the usual
 *      WRITE_END, READ_END order
 * @return `SUCCESS_CODE` on success, or an error code otherwise
 */
int create_pipes(ast_t *ast, int fds[ast->num_commands - 1][2]) {
    for (size_t i = 0; i < ast->num_commands - 1; i++) {
        if (pipe(fds[i])) return errno;
    }
    return SUCCESS_CODE;
}

/**
 * @brief Executes the built-in command corresponding to a node
 * Assumes the node is actually a built-in
 * 
 * @param node The node containing the built-in command
 * @return `SUCCESS_CODE` on success, `EXIT_CODE` if the shell should exit,
 *      or an error code if one occurs
 */
int execute_builtin(command_node_t *node) {
    switch (node->builtin) {
        case EXIT: {
            return EXIT_CODE;
        }
        case CD: {
            if (chdir(node->args[1])) return errno;
            break;
        }
        case HISTORY: {
            break;
        }
        default: {
            fprintf(stderr, "Unimplemented builtin.\n");
            return SERR_CODE;
        }
    }
    return SUCCESS_CODE;
}

/**
 * @brief Allocates a new unreaped command. Asserts success of memory allocation
 * 
 * @param num_commands The number of commands to allocate room for
 * @param command The text of the command, or NULL
 * @return unreaped_command_t* 
 */
unreaped_command_t *new_unreaped(size_t num_commands, char *command) {
    unreaped_command_t *cmd = checked_malloc(sizeof(unreaped_command_t));
    cmd->running = checked_malloc(num_commands * sizeof(pid_t));
    cmd->num_running = num_commands;
    cmd->command = command;
    return cmd;
}

/**
 * @brief Frees memory associated with a `unreaped_command_t` struct.
 * 
 * @param cmd struct to free
 */
void free_unreaped(unreaped_command_t *cmd) {
    if (cmd->command != NULL) {
        free(cmd->command);
    }
    free(cmd->running);
    free(cmd);
}

/**
 * @brief Executes all the commands in an AST
 * Assumes it is not a built-in command
 * 
 * @param ast The AST to execute
 * @param background Whether the command will be running in the background
 * @return A handle to the created command or NULL on failure. If failure occurs
 * errno is set to an error code.
 */
unreaped_command_t *execute_ast(ast_t *ast, bool background) {
    command_node_t *root = ast->root;
    size_t num_pipes = ast->num_commands - 1;
    unreaped_command_t *cmd = new_unreaped(ast->num_commands, NULL);
    pid_t pid;

    if (num_pipes > 0) {
        // create all pipes
        int pipes[num_pipes][2];
        errno = create_pipes(ast, pipes);
        if (errno) goto FAIL;

        // start root
        pid = start_process(root, NULL, pipes[0], background);
        cmd->running[0] = pid;
        if (pid == -1) goto FAIL;

        command_node_t *node = (command_node_t *)root->output_node;
        for (size_t i = 0; i < num_pipes; i++) {
            pid = start_process(node, pipes[i],
                    i < num_pipes - 1 ? pipes[i + 1] : NULL, false);
            if (pid == -1) goto FAIL;
            cmd->running[i + 1] = pid;
            if (i == num_pipes - 1) {
                cmd->last = pid;
            }

            // parent closes its pipes as they're used
            if (close(pipes[i][WRITE_END]) | close(pipes[i][READ_END])) {
                goto FAIL;
            }

            node = (command_node_t *) node->output_node;
        }
    } else { // one command
        pid = start_process(root, NULL, NULL, background);
        if (pid == -1) goto FAIL;
        cmd->last = cmd->running[0] = pid;
    }

    if (false) {
        FAIL:
        free_unreaped(cmd);
        cmd = NULL;
    }
    free_ast(ast);
    return cmd;
}

/**
 * @brief Reports the completion of a background command.
 */
void report_background(unreaped_command_t *cmd) {
    char exit[] = "exited";
    char term[] = "terminated";
    int status = cmd->status;
    char *exit_msg = WIFEXITED(status) ? exit : term;
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : 
                                   WTERMSIG(status);
    printf("[Background %s(%d)] %s\n", exit_msg, code, cmd->command);
}

/**
 * @brief Cleans up finished background commands
 * 
 * This reports their completion, removes them from the list of background
 * commands, and frees any memory associated with managing them.
 */
void cleanup_background() {
    for (size_t i = 0; i < num_background; i++) {
        if (background[i]->num_running == 0) {
            unreaped_command_t *cmd = background[i];
            // swap remove the current index and decrement the index so that 
            // the new element in place `i` is also considered.
            background[i--] = background[--num_background];
            report_background(cmd);
            free_unreaped(cmd);
        }
    }
}

/**
 * @brief Registers a running command as a background command.
 * 
 * Calling this while `reap_lock` is `false` will result in a race condition.
 */
void add_background(unreaped_command_t *cmd) {
    static size_t size_background = 0;
    if (num_background == size_background) {
        size_background = size_background > 0 ? 2 * size_background : 1;
        background = checked_realloc(background, 
                        size_background * sizeof(unreaped_command_t *));
    }
    background[num_background++] = cmd;
}

/**
 * @brief Given a string of user input, parses and runs it
 * 
 * @param command The user input
 * @return 0 on success, `EXIT_CODE` if the shell should exit,
 *      or an error code if one occurs
 */
int parse_input(const char *command) {
    // tokenize and create_ast print error messages and return NULL on failure.
    // We return SERR_CODE to indicate that an error message has already been
    // printed.
    token_t *tokens = tokenize(command);
    if (tokens == NULL) return SERR_CODE;
    ast_t *ast = create_ast(tokens);
    if (ast == NULL) return SERR_CODE;
    
    if (ast->root == NULL) return SUCCESS_CODE;
    size_t num_commands = ast->num_commands;

    command_node_t *root = ast->root;
    if (root->builtin && root->builtin != HISTORY) {
        int err = execute_builtin(root);
        free_ast(ast);
        return err;
    }

    if (ast->background) {
        unreaped_command_t *cmd = execute_ast(ast, true);
        if (cmd == NULL) return errno;

        cmd->command = checked_ptr(strdup(command));
        add_background(cmd);
        printf("[Background started] ");
        for (size_t i = 0; i < cmd->num_running; i++) {
            printf("%d ", cmd->running[i]);
        }
        printf("\n");
        return SUCCESS_CODE;
    } else {
        unreaped_command_t *cmd = execute_ast(ast, false);
        if (cmd == NULL) return errno;
        
        foreground = cmd;

        // wait for children
        int status;
        for (size_t i = 0; i < num_commands; i++) {
            // if waitpid fails because it was interrupted by a signal, simply
            // try again
            while (waitpid(cmd->running[i], &status, 0) == -1 && errno == EINTR)
                ;
        }

        foreground = NULL;
        free_unreaped(cmd);
        return WIFEXITED(status) ? WEXITSTATUS(status) : 0;;
    }
}

/**
 * @brief Prints the prompt and collects user input
 * 
 * @param input A buffer to store the input in
 * @return `SUCCESS_CODE` on success, an error code on error
 */
int get_input(char input[MAX_COMMAND_LENGTH]) {
    // For ease of multiline processing, we initialize it as having a trailing
    // newline.
    input[0] = '\\';
    size_t input_len = 1;
    
    // username
    struct passwd *user = getpwuid(getuid());
    if (user == NULL) return errno;

    // directory
    char cwd_buf[MAX_PATH_LENGTH + 1];
    char *cwd = getcwd(cwd_buf, MAX_PATH_LENGTH + 1);
    if (cwd == NULL) return errno;

    // prompt
    const size_t PROMPT_SIZE = MAX_PATH_LENGTH + _SC_LOGIN_NAME_MAX + 3 - 1;
    char prompt[PROMPT_SIZE];
    snprintf(prompt, PROMPT_SIZE, "%s:%s> ", user->pw_name, cwd);

    while (input[input_len - 1] == '\\') {
        // remove the current trailing line
        input[--input_len] = '\0';

        // user input
        char *line = readline(prompt);
        if (line == NULL) return EXIT_CODE;

        size_t line_len = strlen(line);
        input_len += strlen(line);
        if (input_len >= MAX_COMMAND_LENGTH) {
            free(line);
            fprintf(stderr, "The command you tried to enter is too long.\n"
                    "Max command length is 1023 characters.\n");
            return SERR_CODE;
        }

        strcat(input, line);
        free(line);
        if (line_len == 0) break;

        prompt[0] = '>';
        prompt[1] = ' ';
        prompt[2] = '\0';
    }
    if (input[0] == '!') {
        char *end;
        int index = strtol(input + 1, &end, 10);
        if (*end != '\0') return errno;

        HIST_ENTRY *entry = history_get(history_base + index - 1);
        if (entry == NULL) {
            fprintf(stderr, "Invalid history index.\n");
            return SERR_CODE;
        }
        printf("%d > %s\n", index, entry->line);
        strcpy(input, entry->line);
    } else if (input[0] != '\0') {
        add_history(input);
    }
    return SUCCESS_CODE;
}

int main() {
    using_history();
    stifle_history(MAX_HISTORY);
    if (read_history(HISTORY_FILE)) {
        if (errno == 2) {
            if (close(open(HISTORY_FILE, O_CREAT, DEFAULT_FILE_MODE))) {
                fprintf(stderr, "Could not create history file: %s\n",
                        strerror(errno));
            }
        } else {
            fprintf(stderr, "Could not get history: %s\n", strerror(errno));
        }
    }
    signal(SIGINT, sig_forward);
    signal(SIGTSTP, sig_forward);
    while (true) {
        // prompt for input
        char input[MAX_COMMAND_LENGTH];
        int err = get_input(input);

        // Run the command
        if (!err) {
            err = parse_input(input);
        }

        // Error handling
        if (err == EXIT_CODE) {
            break;
        } else if (err == ERR_CODE) {
            fprintf(stderr, "Internal shell error\n");
        } else if (err != 0 && err != SERR_CODE) {
            fprintf(stderr, "Ran with error code %d: \"%s\" or command error\n",
                    err, strerror(err));
        }

        reap();
        cleanup_background();
    }
    if (write_history(HISTORY_FILE)) {
        fprintf(stderr, "Couldn't save history: %s\n", strerror(errno));
    }

    return 0;
}