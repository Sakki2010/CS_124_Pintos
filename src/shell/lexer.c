#include "lexer.h"
#include "err_utils.h"

#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <sys/errno.h>
#include <sys/stat.h>

/**
 * @brief Represents the shell's input, globally.
*/
const ast_node_t SHELLIN_NODE = {.type = SHELLIN, .path = NULL};
/**
 * @brief Represents the shell's output, globally.
*/
const ast_node_t SHELLOUT_NODE = {.type = SHELLOUT, .path = NULL};

/**
 * @brief A list of built-in commands
*/
const char *CD_STR = "cd";
const char *CHDIR_STR = "chdir";
const char *EXIT_STR = "exit";
const char *HISTORY_STR = "history";

/**
 * When aborting creating an AST, cleans up afterwards.
 * 
 * @param tokens The tokens which were being used for the AST. Takes ownership.
 * @param ast The AST. Takes ownership.
 * @param new An optional node, recently created, to also free. 
 * Set to NULL if not needed.
*/
void abort_create_tree(token_t *tokens, ast_t *ast, command_node_t *new);

/**
 * @brief Finds the end of the current command, starting at start
 * @param start An array of tokens to begin with
 * @return The length of the current command
*/
size_t command_len(token_t *start) {
    size_t i = 0;
    while (!is_end_token(start + i) && (start + i)->type != PIPE) {
        i++;
    }
    return i;
}

/**
 * @brief Creates an input node from a token
 * @param token The token containing the file path
 * @return A heap-allocated input node, without an assigned output
*/
input_node_t *create_input_node(token_t *token) {
    input_node_t *node = checked_malloc(sizeof(input_node_t));
    node->type = INPUT;
    node->path = token->body.text;
    return node;
}

/**
 * @brief Creates an output node from a token
 * @param token The token containing the file path
 * @return A heap-allocated input node, without an assigned input
*/
output_node_t *create_output_node(token_t *token) {
    output_node_t *node = checked_malloc(sizeof(output_node_t));
    node->type = OUTPUT;
    node->path = token->body.text;
    node->append = false;
    return node;
}

/**
 * @brief Handles when a token which depends on the subsequent one.
 * Helper for command node parsing.
 * 
 * @param cmd The command node being constructed
 * @param first The first token
 * @param second The subsequent token
 * @return True on success, false on error
*/
bool parse_two_tokens(command_node_t *cmd, token_t *first, token_t *second) {
    if (second->type != WORD) {
        fprintf(stderr, "Expected file but encountered token of type %s.\n", 
                token_name(second));
        return false;
    }

    // Parse by type
    switch (first->type) {
        case REDIRECT_READ: {
            if (cmd->input_node->type != SHELLIN) {
                fprintf(stderr, "Process %s cannot accept both "
                        "%s and %s as input streams.\n",
                        cmd->path, cmd->input_node->path, second->body.text);
                return false;
            }
            input_node_t *input = create_input_node(second);
            input->output_node = (ast_node_t *) cmd;
            cmd->input_node = (ast_node_t *) input;
            break;
        }
        case REDIRECT_WRITE:
        case REDIRECT_APPEND: {
            // Create node
            output_node_t *output = create_output_node(second);
            if (first->type == REDIRECT_APPEND) {
                output->append = true;
            }
            output->input_node = (ast_node_t *) cmd;

            // Link up
            switch (first->body.fds[0]) {
                case STDOUT_FILENO: {
                    if (cmd->output_node == STREAM_DUPLICATED) {
                        fprintf(stderr, "Process %s cannot have stream %d "
                            "redirected and duplicated.\n",
                            cmd->path, STDOUT_FILENO);
                        return false;
                    }
                    if (cmd->output_node->type != SHELLOUT) {
                        fprintf(stderr, "Process %s cannot accept both "
                                "%s and %s as output streams.\n",
                                cmd->path, cmd->output_node->path,
                                second->body.text);
                        return false;
                    }
                    cmd->output_node = (ast_node_t *) output;
                    break;
                }
                case STDERR_FILENO: {
                    if (cmd->error_node == STREAM_DUPLICATED) {
                        fprintf(stderr, "Process %s cannot have stream %d "
                            "redirected and duplicated.\n",
                            cmd->path, STDERR_FILENO);
                        return false;
                    }
                    if (cmd->error_node->type != SHELLOUT) {
                        fprintf(stderr, "Process %s cannot accept both"
                                " %s and %s as error streams.\n",
                                cmd->path, cmd->error_node->path,
                                second->body.text);
                        return false;
                    }
                    cmd->error_node = (ast_node_t *) output;
                    break;
                }
                default: {
                    fprintf(stderr, "Cannot validly write from stream "
                            "%d of process %s.\n",
                            first->body.fds[0], cmd->path);
                    return false;
                }
            } 
            break;
        }
        default: {
            break;
        }
    }

    return true;
}

/**
 * @brief Handles when a token is a duplicate token
 * @param cmd The command node being constructed
 * @param token The duplicate token
 * @return True on success, false on error
*/
bool parse_duplicate_token(command_node_t *cmd, token_t *token) {
    int from = token->body.fds[0];
    int to = token->body.fds[1];

    if (cmd->output_node == STREAM_DUPLICATED ||
            cmd->error_node == STREAM_DUPLICATED) {
        fprintf(stderr, "Cannot duplicate stream %d to %d for process %s "
            "since another duplication has already happened.\n",
            from, to, cmd->path);
        return false;
    }

    switch(from) {
        case STDOUT_FILENO: {
            if (cmd->output_node->type != SHELLOUT) {
                fprintf(stderr, "Process %s cannot accept use %s as an output "
                        "stream while duplicating.\n",
                        cmd->path, cmd->output_node->path);
                return false;
            }
            if (to == STDERR_FILENO) {
                cmd->output_node = STREAM_DUPLICATED;
            }
            else {
                fprintf(stderr, "Duplicating from stream %d to stream %d "
                        "is not supported.\n", from, to);
                return false;
            }
            break;
        }
        case STDERR_FILENO: {
            if (cmd->error_node->type != SHELLOUT) {
                fprintf(stderr, "Process %s cannot accept use %s as an error "
                        "stream while duplicating.\n",
                        cmd->path, cmd->error_node->path);
                return false;
            }
            if (to == STDOUT_FILENO) {
                cmd->error_node = STREAM_DUPLICATED;
            }
            else {
                fprintf(stderr, "Duplicating from stream %d to stream %d "
                "is not supported.\n", from, to);
                return false;
            }
            break;
        }
        default: {
            fprintf(stderr, "Cannot validly duplicate from stream %d "
            "of process %s.\n", from, cmd->path);
            return false;
        }
    }

    return true;
}

/**
 * @brief Creates a command node from a set of tokens
 * @param start The token to start from
 * @param len The length of the command, in tokens
 * @return A heap-allocated command node, or NULL on error
*/
command_node_t *create_command_node(token_t *start, size_t len) {
    // setup
    if (start->type != WORD) {
        fprintf(stderr, "Expected command but encountered token of type %s.\n",
                token_name(start));
        return NULL;
    }

    command_node_t *cmd = checked_malloc(sizeof(command_node_t));

    cmd->path = start->body.text;
    cmd->type = COMMAND;
    cmd->input_node = (ast_node_t *) (&SHELLIN_NODE);
    cmd->output_node = (ast_node_t *) (&SHELLOUT_NODE);
    cmd->error_node = (ast_node_t *) (&SHELLOUT_NODE);

    if (!strcmp(start->body.text, CD_STR) ||
            !strcmp(start->body.text, CHDIR_STR)) {
        cmd->builtin = CD;
    } else if (!strcmp(start->body.text, EXIT_STR)) {
        cmd->builtin = EXIT;
    } else if (!strcmp(start->body.text, HISTORY_STR)) {
        cmd->builtin = HISTORY;
    } else {
        cmd->builtin = NO_BUILTIN;
    }

    // set up arguments array
    cmd->args = checked_calloc(len + 2, sizeof(char*));
    size_t args_i = 0;

    for (size_t i = 0; i < len; i++) {
        token_t *curr = start + i;
        switch (curr->type) {
            case WORD: { // Argument
                cmd->args[args_i] = curr->body.text;
                args_i++;
                break;
            }
            case DUPLICATE: {
                if (!parse_duplicate_token(cmd, curr)) {
                    return NULL;
                }
                break;
            }
            default: { // Token requires the next one too
                i++;
                if (i >= len) {
                    fprintf(stderr, 
                            "Unexpected end of process while parsing %s.\n",
                            cmd->path);
                    return NULL;
                }
                if (!parse_two_tokens(cmd, curr, start + i)) {
                    return NULL;
                }
                break;
            }
        }
    }
    cmd->args[args_i] = NULL; // null-terminated
    return cmd;
}

/**
 * @brief Frees a node AND all nodes it links to downstream, recursively
 * @param node The node to free
*/
void free_node(ast_node_t *node) {
    if (node != STREAM_DUPLICATED) {
        switch(node->type) {
            case COMMAND: {
                command_node_t *cmd = (command_node_t *)node;
                size_t i = 0;
                while (cmd->args[i] != NULL) {
                    free(cmd->args[i]);
                    i++;
                }
                free(cmd->args);
                free_node(cmd->output_node);
                free_node(cmd->error_node);
                break;
            }
            case OUTPUT: {
                free(node->path);
                free(node);
                break;
            }
            // input is ignored, since we only go downstream
            // shellin and shellout have nothing to free
            default: {
                break;
            }
        }
    }
}

ast_t *create_ast(token_t *tokens) {
    ast_t *ast = checked_malloc(sizeof(ast_t));
    ast->num_commands = 0;

    if (is_end_token(&tokens[0])) {
        free(tokens);
        ast->background = false;
        ast->num_commands = 0;
        ast->root = NULL;
        return ast;
    }

    size_t len = command_len(tokens);

    // First process, no pipe yet
    command_node_t *first = create_command_node(tokens, len);
    if (first == NULL) {
        free(tokens);
        free_node((ast_node_t *) first);
        return NULL;
    }
    ast->root = first;

    if (ast->root->builtin && ast->root->builtin != HISTORY &&
        (ast->root->error_node->type != SHELLOUT ||
        ast->root->output_node->type != SHELLOUT ||
        ast->root->input_node->type != SHELLIN)) {
            fprintf(stderr, "Built-in %s may not be piped or redirected.\n",
                    ast->root->path);
            abort_create_tree(tokens, ast, NULL);
            return NULL;
    }

    ast->num_commands++;

    // Handle subsequent pipes
    token_t *second = tokens + len;
    while (second->type == PIPE) {
        second++;
        len = command_len(second);
        if (len <= 0) {
            fprintf(stderr, "Unexpected end of command.\n");
            abort_create_tree(tokens, ast, NULL);
            return NULL;
        }

        // Parse next command and link up
        command_node_t *next = create_command_node(second, len);
        if (next == NULL) {
            return NULL;
        }
        if (first->output_node->type != SHELLOUT) {
            fprintf(stderr, "Process %s cannot be redirected to %s and "
                    "piped to %s.\n",
                    first->path, first->output_node->path, next->path);
            abort_create_tree(tokens, ast, next);
            return NULL;
        }
        if (next->input_node->type != SHELLIN) {
            fprintf(stderr, "Process %s cannot have %s redirected to it and "
                    "%s piped to it.\n",
                    next->path, next->input_node->path, first->path);
            abort_create_tree(tokens, ast, next);
            return NULL;
        }
        if (next->builtin || first->builtin) {
            fprintf(stderr, "Cannot pipe built-ins, from %s to %s.\n",
                    first->path, next->path);
            abort_create_tree(tokens, ast, next);
            return NULL;
        }
        first->output_node = (ast_node_t *) next;
        next->input_node = (ast_node_t *) first;
        ast->num_commands++;

        // Set up for next loop
        first = next;
        second += len;
    }

    // Take background flag from END token and attach to AST
    ast->background = second->body.background;

    free(tokens);
    return ast;
}

/**
 * @brief Frees a node, if it is an input node. Does NOT free downstream.
 * Helper for the root, which can have an input node
 * @param node The input node
*/
void free_input_node(ast_node_t *node) {
    if (node->type == INPUT) {
        free(node->path);
        free(node);
    }
}

void free_ast(ast_t *ast) {
    free_input_node(ast->root->input_node);
    free_node((ast_node_t *) ast->root);
    free(ast);
}

void abort_create_tree(token_t *tokens, ast_t *ast, command_node_t *new) {
    if (new != NULL) {
        free_node((ast_node_t *) new);
    }
    free_ast(ast);
    free(tokens);
}

void print_ast(ast_t *ast) {
    fprintf(stderr, "%zu: ", ast->num_commands);
    if (ast->root->input_node->type != SHELLIN) {
        fprintf(stderr, "%s > ", ast->root->input_node->path);
    }
    ast_node_t *node = (ast_node_t *) ast->root;
    while (node != STREAM_DUPLICATED && node->type == COMMAND) {
        command_node_t *curr = (command_node_t *)node;
        fprintf(stderr, "%s [", curr->path);
        size_t i = 1;
        while (curr->args[i] != NULL) {
            fprintf(stderr, "%s, ", curr->args[i]);
            i++;
        }
        fprintf(stderr, "] ");

        if (curr->error_node == STREAM_DUPLICATED) {
            fprintf(stderr, "(err: STDOUT) ");
        } else if (curr->error_node->type != SHELLOUT) {
            fprintf(stderr, "(err: %s) ", curr->error_node->path);
        }

        fprintf(stderr, "> ");
        node = curr->output_node;
    }
    if (node == STREAM_DUPLICATED) {
        fprintf(stderr, "STDERR");
    } else if (node->type == OUTPUT) {
        fprintf(stderr, "%s", node->path);
    }
    fprintf(stderr, "\n");
}