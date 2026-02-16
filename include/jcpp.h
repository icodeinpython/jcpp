#ifndef JCPP_H
#define JCPP_H

#include <stdbool.h>
#include <stdio.h>

typedef enum TokenType {
    TOKEN_IDENTIFIER,
    TOKEN_NUMBER,
    TOKEN_STRING,
    TOKEN_CHAR,
    TOKEN_PUNCT,
    TOKEN_WHITESPACE,
    TOKEN_NEWLINE
} TokenType;

typedef struct Token {
    TokenType type;
    char *text;
    int line;
    char *filename;
} Token;

typedef struct TokenList {
    Token *data;
    int count;
    int capacity;
} TokenList;

typedef struct Macro {
    char *name;
    Token *replacement;
    int replacement_count;

    bool is_function_like;
    char **params;
    int param_count;

    int disabled_count;
} Macro;

typedef struct MacroTable {
    Macro *data;
    int count;
    int capacity;
} MacroTable;

typedef struct FileContext {
    FILE *file;
    char *filename;
    int line;
} FileContext;

typedef struct IncludeStack {
    FileContext *data;
    int count;
    int capacity;
} IncludeStack;

void macro_table_init(MacroTable *table);
void macro_table_free(MacroTable *table);

void include_stack_init(IncludeStack *stack);
void include_stack_free(IncludeStack *stack);

void preprocess_file(const char *filename, TokenList *out, MacroTable *table, IncludeStack *stack);
void token_list_free(TokenList *list);

#endif
