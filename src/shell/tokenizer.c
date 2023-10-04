#include "tokenizer.h"
#include "err_utils.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <ctype.h>

inline bool is_end_token(token_t *token) {
    return token->type == END;
}

/**
 * @brief Gives a pointer to the first non-whitespace character in the given
 * string.
 * 
 * @param str Borrowed immutably.
 * @return char* The first non-whitespace character or the end of the string,
 * whichever comes first.
 */
const char *discard_whitespace(const char *s) {
    char c = s[0];
    while (isspace(c)) {
        c = *(++s);
    }
    return s;
}

/**
 * @brief Reads an output redirect token, starting with >.
 * 
 * Handles >, >>, n>, n>>, n>&m.
 * 
 * In cases like 2>, the 2 is passed in as fd. > should be called with fd = 1.
 * 
 * @param out Output pointer. Borrowed mutably.
 * @param in Command to read first token from. Borrowed immutably.
 * @param fd The file descriptor of the file being redirected
 * @return char* Pointer to the beginning of the next token in `in`.
 */
const char *read_out_redirect_token(token_t *out, const char *in, int fd) {
    if (in[1] == '&') {
        out->type = DUPLICATE;
        out->body.fds[0] = fd;
        out->body.fds[1] = in[2] - '0';
        in += 3;
    } else if (in[1] == '>') {
        out->type = REDIRECT_APPEND;
        out->body.fds[0] = fd;
        in += 2;
    } else {
        out->type = REDIRECT_WRITE;
        out->body.fds[0] = fd;
        in += 1;
    }
    return in;
}

/**
 * @brief Reads a word token until the a character satisfying end_pred, 
 * exclusive.
 * 
 * @param out Output pointer. Borrowed mutably.
 * @param in Command to read first token from. Borrowed immutably.
 * @param end_pred Predicate specifying when to stop
 * @return char* Pointer to the beginning of the next token in `in`. 
 * Returns `NULL` on error.
 */
const char *read_token_until(token_t *out, const char *in, 
                             bool (*end_pred)(char)) {
    size_t i = 0;
    out->type = WORD;
    bool escape_next = false;
    char buf[MAX_COMMAND_LENGTH];
    while (escape_next || !end_pred(*in)) {
        if (*in == '\0') {
            fprintf(stderr, "Parse error: unbalanced quotation marks.\n");
            return NULL;
        } else if (*in == '\\' && !escape_next) {
            in++;
            escape_next = true;
            continue;
        } else {
            escape_next = false;
            buf[i++] = *in++;
        }
    }
    buf[i] = '\0';
    out->body.text = checked_malloc((i + 1) * sizeof(char));
    strcpy(out->body.text, buf);
    return in;
}

/**
 * @brief Specifies whether a character should end a non-quoted word token.
 */
bool is_word_token_end_char(char c) {
    return isspace(c) || strchr("\"<>|", c) != NULL;
}

/**
 * @brief Reads a bare word token.
 * 
 * @param out Output pointer. Borrowed mutably.
 * @param in Command to read first token from. Borrowed immutably.
 * @return char* Pointer to the beginning of the next token in `in`. 
 */
const char *read_word_token(token_t *out, const char *in) {
    return read_token_until(out, in, is_word_token_end_char);
}

/**
 * @brief Specifies whether character is a quote.
 */
bool is_quote(char c) {
    return c == '"';
}

/**
 * @brief Reads a quoted word token.
 * 
 * @param out Output pointer. Borrowed mutably.
 * @param in Command to read first token from. Borrowed immutably.
 * @return char* Pointer to the beginning of the next token in `in`. 
 * Returns `NULL` on error.
 */
const char *read_quote_token(token_t *out, const char *in) {
    // skip the opening and closing quote
    const char *next = read_token_until(out, in + 1, is_quote);
    return next == NULL ? NULL : next + 1;
}

/**
 * @brief Reads the first token from in and puts it in out, then discards
 * whitespace until the start of the next token.
 * 
 * If the string is empty, a token is written such that is_end_token 
 * returns true.
 * 
 * @param out Output pointer. Borrowed mutably.
 * @param in Command to read first token from. Borrowed immutably.
 * @return char* Pointer to the beginning of the next token in `in` or `in`
 * if the string is empty. Returns `NULL` on error.
 */
const char *read_token(token_t *out, const char *in) {
    switch (in[0]) {
        case '&': {
            out->body.background = true;
            out->type = END;
            return in;
        }
        case '\0': {
            out->body.background = false;
            out->type = END;
            return in;
        }
        case '"': {
            const char *next = read_quote_token(out, in);
            return next == NULL ? NULL : discard_whitespace(next);
        }
        case '>': {
            return discard_whitespace(read_out_redirect_token(out, in, 1));
        }
        case '<': {
            out->type = REDIRECT_READ;
            return discard_whitespace(in + 1);
        }
        case '|': {
            out->type = PIPE;
            return discard_whitespace(in + 1);
        }
        case '0' ... '9': {
            if (in[1] == '>') {
                return discard_whitespace(
                    read_out_redirect_token(out, in + 1, in[0] - '0'));
            }
            break;
        }
        default: {
            break;
        }
    }
    return discard_whitespace(read_word_token(out, in));
}

token_t *tokenize(const char *command) {
    token_t *tokens = checked_malloc(sizeof(token_t));
    size_t i = 0;
    size_t tokens_size = 1;
    while (true) {
        const char *next = read_token(&tokens[i++], command);
        if (next == NULL) {
            // Encountered a parse error
            for (size_t j = 0; j < i - 1; j++) {
                if (tokens[j].type == WORD) {
                    free(tokens[j].body.text);
                }
            }
            free(tokens);
            return NULL;
        } else if (is_end_token(&tokens[i - 1])) {
            // Reached the end
            break;
        }
        if (i == tokens_size) {
            tokens_size *= 2;
            tokens = checked_realloc(tokens, sizeof(token_t) * tokens_size);
        }
        command = next;
    }
    tokens = checked_realloc(tokens, sizeof(token_t) * i);
    return tokens;
}

void print_token(token_t *token) {
    switch (token->type) {
        case END:
            printf("END(%d)", token->body.background);
            return;
        case WORD:
            printf("WORD(%s)", token->body.text);
            return;
        case REDIRECT_READ:
            printf("REDIRECT_READ");
            return;
        case REDIRECT_WRITE:
            printf("REDIRECT_WRITE(%d)", token->body.fds[0]);
            return;
        case PIPE:
            printf("PIPE");
            return;
        case REDIRECT_APPEND:
            printf("REDIRECT_APPEND(%d)", token->body.fds[0]);
            return;
        case DUPLICATE:
            printf("DUPLICATE(%d, %d)", token->body.fds[0], token->body.fds[1]);
            return;
        default:
            printf("[UNDEFINED TOKEN]");
    }
}

char *token_name(token_t *token) {
    switch (token->type) {
        case END: return "END";
        case WORD: return "word";
        case REDIRECT_READ: return "redirect in";
        case REDIRECT_WRITE: return "redirect out";
        case PIPE: return "pipe";
        case REDIRECT_APPEND: return "redirect out append";
        case DUPLICATE: return "duplicate stream";
        default: return "[UNDEFINED TOKEN]";
    }
}

void print_tokens(token_t *tokens) {
    printf("Tokens: [");
    while (true) {
        token_t *token = tokens++;
        print_token(token);
        if (is_end_token(token)) {
            break;
        } else {
            printf(", ");
        }
    }
    printf("]\n");
}