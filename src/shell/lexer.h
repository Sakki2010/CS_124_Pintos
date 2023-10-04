#ifndef SHELL_LEXER_H
#define SHELL_LEXER_H

#include "tokenizer.h"
#include <stdlib.h>

// Indicates a process's stream has been duplicated
#define STREAM_DUPLICATED NULL

/**
 * @brief The kinds of tokens which can be used as nodes.
*/
typedef enum node_type {
    COMMAND, INPUT, OUTPUT, SHELLIN, SHELLOUT
} node_type;

/**
 * @brief Specifies which type of built-in is represented by a command node
*/
typedef enum builtin {
    NO_BUILTIN = 0, // Other kind of command
    EXIT, CD, HISTORY
} builtin_t;

/**
 * @brief A token, with the ability to connect to other tokens.
*/
typedef struct ast_node {
    node_type type; // This node's type, to allow it to be identified
    char *path; // The path to the token's file or command
} ast_node_t;

/**
 * @brief A node, reprsenting a token which is a command.
*/
typedef struct command_node {
    node_type type;
    char *path;

    // Null-terminated list of the arguments for the command, 
    // including the process
    char **args;
    ast_node_t *input_node; // This node's input
    ast_node_t *output_node; // This node's output
    ast_node_t *error_node; // This node's error output

    builtin_t builtin; // Whether it's a built in, and if so, which one
} command_node_t;

/**
 * @brief A node, representing a token which is a file used as input.
 * 
 * This node itself has no input. It can only be used in the root.
*/
typedef struct input_node {
    node_type type;
    char *path;
    ast_node_t *output_node; // This node's output
} input_node_t;

/**
 * @brief A node, representing a token which is a file used as output.
*/
typedef struct output_node {
    node_type type;
    char *path;
    ast_node_t *input_node; // This node's input
    bool append; // True if it should be appended to the file, false otherwise
} output_node_t;

/**
 * @brief An abstract syntax tree representing tokens how tokens relate.
*/
typedef struct ast {
    command_node_t *root; // The root node of the tree
    size_t num_commands; // How many commands are in this AST
    bool background; // Whether or not it should be run in the background
} ast_t;

/**
 * @brief Applies lexer to array of tokens.
 * 
 * @param tokens Heap-allocated tokens representing a shell command, in order.
 *      Takes ownership.
 * @return A heap-allocated tree of nodes representing how the tokens relate to 
 * each other, or NULL on failure.
*/
ast_t *create_ast(token_t *tokens);

/**
 * @brief Frees an AST
*/
void free_ast(ast_t *ast);

/**
 * @brief Prints an AST
*/
void print_ast(ast_t *ast);

#endif /* LEXER_H */