#ifndef SHELL_TOKENIZER_H
#define SHELL_TOKENIZER_H
#include <stdbool.h>

#define MAX_COMMAND_LENGTH 1024

typedef enum {
    END, WORD, PIPE, REDIRECT_READ, REDIRECT_WRITE, REDIRECT_APPEND, DUPLICATE
} token_type_t;

/**
 * @brief Represents a token for the lexer to work with.
 */
typedef struct {
    // The type of the token
    token_type_t type;
    // Additional data associated with the token beyond its type.
    // .text field is defined for WORD tokens
    // .fds[0] is defined for REDIRECT_WRITE/APPEND and DUPLICATE
    // .fds[1] is defiend for DUPLICATE
    // .background is defined for END
    // Note that PIPE and REDIRECT_READ have no associated data.
    union {char *text; int fds[2]; bool background;} body;
} token_t;

/**
 * @brief Determines whether the given token terminates the array returns by
 * tokenize.
 * 
 * @param token The token to check
 * @return `true` if it is or `false`otherwise.
 */
bool is_end_token(token_t *token);

/**
 * @brief Tokenizes a shell command.
 * 
 * Tokens are anything wrapped in quotes and otherwise
 *  - whitespace separated
 *  - control characters are new tokens
 * 
 * @param command The command to tokenize. Borrowed imutable.
 * @return Array of tokens or `NULL` on failure. The array is
 * terminated by a token on which `is_end_token` returns `true.`
 */
token_t *tokenize(const char *command);

/**
 * @brief Prints the token to stdout. Debugging method.
 * 
 * @param token 
 */
void print_token(token_t *token);

/**
 * @brief Gets the name of the token type passed in for error handling.
 * 
 * @param token 
 * @return char* a string literal, which does not need to be freed.
 */
char *token_name(token_t *token);

/**
 * @brief Prints an array of tokens, terminated by a token on which 
 * `is_end_token` returns `true`.
 * 
 * @param tokens
 */
void print_tokens(token_t *tokens);

#endif