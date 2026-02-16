#include "jcpp.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static char *jcpp_strdup(const char *text) {
    size_t len = strlen(text);
    char *out = (char *)malloc(len + 1);
    if (!out) {
        return NULL;
    }
    memcpy(out, text, len + 1);
    return out;
}

static void token_list_init(TokenList *list) {
    list->data = NULL;
    list->count = 0;
    list->capacity = 0;
}

static void token_list_push(TokenList *list, Token token) {
    if (list->count >= list->capacity) {
        int next = list->capacity == 0 ? 16 : list->capacity * 2;
        Token *data = (Token *)realloc(list->data, sizeof(Token) * next);
        if (!data) {
            return;
        }
        list->data = data;
        list->capacity = next;
    }
    list->data[list->count++] = token;
}

void token_list_free(TokenList *list) {
    if (!list) {
        return;
    }
    for (int i = 0; i < list->count; i++) {
        free(list->data[i].text);
        free(list->data[i].filename);
    }
    free(list->data);
    list->data = NULL;
    list->count = 0;
    list->capacity = 0;
}

void macro_table_init(MacroTable *table) {
    table->data = NULL;
    table->count = 0;
    table->capacity = 0;
}

static void macro_free(Macro *macro) {
    free(macro->name);
    for (int i = 0; i < macro->replacement_count; i++) {
        free(macro->replacement[i].text);
        free(macro->replacement[i].filename);
    }
    free(macro->replacement);
    for (int i = 0; i < macro->param_count; i++) {
        free(macro->params[i]);
    }
    free(macro->params);
}

void macro_table_free(MacroTable *table) {
    if (!table) {
        return;
    }
    for (int i = 0; i < table->count; i++) {
        macro_free(&table->data[i]);
    }
    free(table->data);
    table->data = NULL;
    table->count = 0;
    table->capacity = 0;
}

static Macro *macro_table_find(MacroTable *table, const char *name) {
    for (int i = 0; i < table->count; i++) {
        if (!table->data[i].name) {
            continue;
        }
        if (strcmp(table->data[i].name, name) == 0) {
            return &table->data[i];
        }
    }
    return NULL;
}

static void macro_table_remove(MacroTable *table, const char *name) {
    for (int i = 0; i < table->count; i++) {
        if (!table->data[i].name) {
            continue;
        }
        if (strcmp(table->data[i].name, name) == 0) {
            macro_free(&table->data[i]);
            table->data[i] = table->data[table->count - 1];
            table->count--;
            return;
        }
    }
}

static Macro *macro_table_add(MacroTable *table, const char *name) {
    Macro *existing = macro_table_find(table, name);
    if (existing) {
        macro_free(existing);
        memset(existing, 0, sizeof(Macro));
        existing->name = jcpp_strdup(name);
        return existing;
    }

    if (table->count >= table->capacity) {
        int next = table->capacity == 0 ? 16 : table->capacity * 2;
        Macro *data = (Macro *)realloc(table->data, sizeof(Macro) * next);
        if (!data) {
            return NULL;
        }
        table->data = data;
        table->capacity = next;
    }
    Macro *macro = &table->data[table->count++];
    memset(macro, 0, sizeof(Macro));
    macro->name = jcpp_strdup(name);
    return macro;
}

void include_stack_init(IncludeStack *stack) {
    stack->data = NULL;
    stack->count = 0;
    stack->capacity = 0;
}

void include_stack_free(IncludeStack *stack) {
    if (!stack) {
        return;
    }
    for (int i = 0; i < stack->count; i++) {
        fclose(stack->data[i].file);
        free(stack->data[i].filename);
    }
    free(stack->data);
    stack->data = NULL;
    stack->count = 0;
    stack->capacity = 0;
}

static void include_stack_push(IncludeStack *stack, FILE *file, const char *filename, int line) {
    if (stack->count >= stack->capacity) {
        int next = stack->capacity == 0 ? 8 : stack->capacity * 2;
        FileContext *data = (FileContext *)realloc(stack->data, sizeof(FileContext) * next);
        if (!data) {
            return;
        }
        stack->data = data;
        stack->capacity = next;
    }
    FileContext ctx = {0};
    ctx.file = file;
    ctx.filename = jcpp_strdup(filename);
    ctx.line = line;
    stack->data[stack->count++] = ctx;
}

static FileContext include_stack_pop(IncludeStack *stack) {
    FileContext empty = {0};
    if (stack->count == 0) {
        return empty;
    }
    return stack->data[--stack->count];
}

static bool is_ident_start(int c) {
    return isalpha(c) || c == '_';
}

static bool is_ident_char(int c) {
    return isalnum(c) || c == '_';
}

static char *read_file_all(FILE *file, size_t *out_len) {
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    if (size < 0) {
        return NULL;
    }
    char *data = (char *)malloc((size_t)size + 1);
    if (!data) {
        return NULL;
    }
    size_t read = fread(data, 1, (size_t)size, file);
    data[read] = '\0';
    if (out_len) {
        *out_len = read;
    }
    return data;
}

static void token_clone_into(const Token *src, Token *dst) {
    dst->type = src->type;
    dst->text = jcpp_strdup(src->text ? src->text : "");
    dst->line = src->line;
    dst->filename = jcpp_strdup(src->filename ? src->filename : "");
}

static void tokenize_text(const char *filename, const char *text, TokenList *out) {
    int line = 1;
    size_t i = 0;
    while (text[i]) {
        char c = text[i];
        if (c == '\n') {
            Token token = {TOKEN_NEWLINE, jcpp_strdup("\n"), line, jcpp_strdup(filename)};
            token_list_push(out, token);
            i++;
            line++;
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v') {
            size_t start = i;
            while (text[i] && text[i] != '\n' && isspace((unsigned char)text[i])) {
                i++;
            }
            size_t len = i - start;
            char *buf = (char *)malloc(len + 1);
            memcpy(buf, text + start, len);
            buf[len] = '\0';
            Token token = {TOKEN_WHITESPACE, buf, line, jcpp_strdup(filename)};
            token_list_push(out, token);
            continue;
        }
        if (is_ident_start((unsigned char)c)) {
            size_t start = i;
            i++;
            while (is_ident_char((unsigned char)text[i])) {
                i++;
            }
            size_t len = i - start;
            char *buf = (char *)malloc(len + 1);
            memcpy(buf, text + start, len);
            buf[len] = '\0';
            Token token = {TOKEN_IDENTIFIER, buf, line, jcpp_strdup(filename)};
            token_list_push(out, token);
            continue;
        }
        if (isdigit((unsigned char)c)) {
            size_t start = i;
            i++;
            while (isdigit((unsigned char)text[i])) {
                i++;
            }
            size_t len = i - start;
            char *buf = (char *)malloc(len + 1);
            memcpy(buf, text + start, len);
            buf[len] = '\0';
            Token token = {TOKEN_NUMBER, buf, line, jcpp_strdup(filename)};
            token_list_push(out, token);
            continue;
        }
        if (c == '"') {
            size_t start = i;
            i++;
            while (text[i] && text[i] != '"') {
                if (text[i] == '\\' && text[i + 1]) {
                    i += 2;
                } else {
                    i++;
                }
            }
            if (text[i] == '"') {
                i++;
            }
            size_t len = i - start;
            char *buf = (char *)malloc(len + 1);
            memcpy(buf, text + start, len);
            buf[len] = '\0';
            Token token = {TOKEN_STRING, buf, line, jcpp_strdup(filename)};
            token_list_push(out, token);
            continue;
        }
        if (c == '\'') {
            size_t start = i;
            i++;
            while (text[i] && text[i] != '\'') {
                if (text[i] == '\\' && text[i + 1]) {
                    i += 2;
                } else {
                    i++;
                }
            }
            if (text[i] == '\'') {
                i++;
            }
            size_t len = i - start;
            char *buf = (char *)malloc(len + 1);
            memcpy(buf, text + start, len);
            buf[len] = '\0';
            Token token = {TOKEN_CHAR, buf, line, jcpp_strdup(filename)};
            token_list_push(out, token);
            continue;
        }

        if (c == '#' && text[i + 1] == '#') {
            Token token = {TOKEN_PUNCT, jcpp_strdup("##"), line, jcpp_strdup(filename)};
            token_list_push(out, token);
            i += 2;
            continue;
        }

        char punct[2] = {c, '\0'};
        Token token = {TOKEN_PUNCT, jcpp_strdup(punct), line, jcpp_strdup(filename)};
        token_list_push(out, token);
        i++;
    }
}

static bool token_is_punct(const Token *token, const char *text) {
    return token->type == TOKEN_PUNCT && strcmp(token->text, text) == 0;
}

static bool token_is_whitespace(const Token *token) {
    return token->type == TOKEN_WHITESPACE || token->type == TOKEN_NEWLINE;
}

static int find_param_index(const Macro *macro, const char *name) {
    for (int i = 0; i < macro->param_count; i++) {
        if (strcmp(macro->params[i], name) == 0) {
            return i;
        }
    }
    return -1;
}

static void stringize_tokens(const TokenList *tokens, Token *out, const char *filename, int line) {
    size_t total = 2;
    for (int i = 0; i < tokens->count; i++) {
        total += strlen(tokens->data[i].text);
    }
    char *buf = (char *)malloc(total + 1);
    size_t pos = 0;
    buf[pos++] = '"';
    for (int i = 0; i < tokens->count; i++) {
        size_t len = strlen(tokens->data[i].text);
        memcpy(buf + pos, tokens->data[i].text, len);
        pos += len;
    }
    buf[pos++] = '"';
    buf[pos] = '\0';
    out->type = TOKEN_STRING;
    out->text = buf;
    out->line = line;
    out->filename = jcpp_strdup(filename);
}

static void concat_tokens(const Token *left, const Token *right, Token *out) {
    size_t left_len = strlen(left->text);
    size_t right_len = strlen(right->text);
    char *buf = (char *)malloc(left_len + right_len + 1);
    memcpy(buf, left->text, left_len);
    memcpy(buf + left_len, right->text, right_len);
    buf[left_len + right_len] = '\0';
    out->type = TOKEN_IDENTIFIER;
    out->text = buf;
    out->line = left->line;
    out->filename = jcpp_strdup(left->filename);
}

static bool eval_if_expr(const TokenList *tokens, MacroTable *table) {
    int i = 0;
    while (i < tokens->count && token_is_whitespace(&tokens->data[i])) {
        i++;
    }
    if (i >= tokens->count) {
        return false;
    }
    Token *tok = &tokens->data[i];
    if (tok->type == TOKEN_NUMBER) {
        return atoi(tok->text) != 0;
    }
    if (tok->type == TOKEN_IDENTIFIER && strcmp(tok->text, "defined") == 0) {
        i++;
        while (i < tokens->count && token_is_whitespace(&tokens->data[i])) {
            i++;
        }
        if (i < tokens->count && token_is_punct(&tokens->data[i], "(")) {
            i++;
            while (i < tokens->count && token_is_whitespace(&tokens->data[i])) {
                i++;
            }
            if (i < tokens->count && tokens->data[i].type == TOKEN_IDENTIFIER) {
                bool defined = macro_table_find(table, tokens->data[i].text) != NULL;
                return defined;
            }
        } else if (i < tokens->count && tokens->data[i].type == TOKEN_IDENTIFIER) {
            bool defined = macro_table_find(table, tokens->data[i].text) != NULL;
            return defined;
        }
    }
    return false;
}

typedef struct CondFrame {
    bool parent_active;
    bool active;
    bool any_taken;
} CondFrame;

typedef struct CondStack {
    CondFrame *data;
    int count;
    int capacity;
} CondStack;

static void cond_stack_init(CondStack *stack) {
    stack->data = NULL;
    stack->count = 0;
    stack->capacity = 0;
}

static void cond_stack_free(CondStack *stack) {
    free(stack->data);
    stack->data = NULL;
    stack->count = 0;
    stack->capacity = 0;
}

static void cond_stack_push(CondStack *stack, CondFrame frame) {
    if (stack->count >= stack->capacity) {
        int next = stack->capacity == 0 ? 8 : stack->capacity * 2;
        CondFrame *data = (CondFrame *)realloc(stack->data, sizeof(CondFrame) * next);
        if (!data) {
            return;
        }
        stack->data = data;
        stack->capacity = next;
    }
    stack->data[stack->count++] = frame;
}

static CondFrame *cond_stack_top(CondStack *stack) {
    if (stack->count == 0) {
        return NULL;
    }
    return &stack->data[stack->count - 1];
}

static void cond_stack_pop(CondStack *stack) {
    if (stack->count > 0) {
        stack->count--;
    }
}

static bool cond_stack_is_active(CondStack *stack) {
    CondFrame *top = cond_stack_top(stack);
    if (!top) {
        return true;
    }
    return top->active;
}

static void read_tokens_from_file(FILE *file, const char *filename, TokenList *out) {
    size_t len = 0;
    char *text = read_file_all(file, &len);
    if (!text) {
        return;
    }
    tokenize_text(filename, text, out);
    free(text);
}

static void expand_macro(Macro *macro, TokenList *args, TokenList *out) {
    TokenList temp;
    token_list_init(&temp);

    for (int i = 0; i < macro->replacement_count; i++) {
        Token *rep = &macro->replacement[i];
        if (rep->type == TOKEN_PUNCT && strcmp(rep->text, "#") == 0) {
            if (i + 1 < macro->replacement_count) {
                Token *next = &macro->replacement[i + 1];
                if (next->type == TOKEN_IDENTIFIER) {
                    int param_index = find_param_index(macro, next->text);
                    if (param_index >= 0 && param_index < macro->param_count) {
                        Token stringized = {0};
                        stringize_tokens(&args[param_index], &stringized, next->filename, next->line);
                        token_list_push(&temp, stringized);
                        i++;
                        continue;
                    }
                }
            }
        }
        if (rep->type == TOKEN_IDENTIFIER) {
            int param_index = find_param_index(macro, rep->text);
            if (param_index >= 0 && param_index < macro->param_count) {
                for (int t = 0; t < args[param_index].count; t++) {
                    Token cloned;
                    token_clone_into(&args[param_index].data[t], &cloned);
                    token_list_push(&temp, cloned);
                }
                continue;
            }
        }
        Token cloned;
        token_clone_into(rep, &cloned);
        token_list_push(&temp, cloned);
    }

    for (int i = 0; i < temp.count; i++) {
        Token *tok = &temp.data[i];
        if (token_is_punct(tok, "##") && i > 0 && i + 1 < temp.count) {
            Token concat = {0};
            concat_tokens(&temp.data[i - 1], &temp.data[i + 1], &concat);
            free(temp.data[i - 1].text);
            free(temp.data[i - 1].filename);
            temp.data[i - 1] = concat;
            free(temp.data[i].text);
            free(temp.data[i].filename);
            free(temp.data[i + 1].text);
            free(temp.data[i + 1].filename);
            temp.data[i].text = jcpp_strdup("");
            temp.data[i + 1].text = jcpp_strdup("");
            temp.data[i].type = TOKEN_WHITESPACE;
            temp.data[i + 1].type = TOKEN_WHITESPACE;
            temp.data[i].filename = NULL;
            temp.data[i + 1].filename = NULL;
        }
    }

    for (int i = 0; i < temp.count; i++) {
        if (temp.data[i].text[0] == '\0') {
            continue;
        }
        Token cloned;
        token_clone_into(&temp.data[i], &cloned);
        token_list_push(out, cloned);
    }

    token_list_free(&temp);
}

static bool try_parse_macro_args(const TokenList *tokens, int *index, Macro *macro, TokenList **out_args) {
    int i = *index;
    int depth = 0;
    int max_args = macro->param_count;
    if (max_args <= 0) {
        return false;
    }
    while (i < tokens->count && token_is_whitespace(&tokens->data[i])) {
        i++;
    }
    if (i >= tokens->count || !token_is_punct(&tokens->data[i], "(")) {
        return false;
    }
    TokenList *args = (TokenList *)calloc((size_t)max_args, sizeof(TokenList));
    for (int a = 0; a < max_args; a++) {
        token_list_init(&args[a]);
    }

    int arg_index = 0;
    for (; i < tokens->count; i++) {
        Token *tok = &tokens->data[i];
        if (token_is_whitespace(tok) && depth == 0) {
            continue;
        }
        if (token_is_punct(tok, "(")) {
            if (depth == 0) {
                depth = 1;
                continue;
            }
        }
        if (token_is_punct(tok, ")")) {
            depth--;
            if (depth == 0) {
                i++;
                break;
            }
        }
        if (depth == 1 && token_is_punct(tok, ",")) {
            if (arg_index + 1 < max_args) {
                arg_index++;
                continue;
            }
        }
        if (depth > 0) {
            Token cloned;
            token_clone_into(tok, &cloned);
            token_list_push(&args[arg_index], cloned);
        }
    }

    if (depth != 0) {
        for (int a = 0; a < max_args; a++) {
            token_list_free(&args[a]);
        }
        free(args);
        return false;
    }

    *index = i;
    *out_args = args;
    return true;
}

static void free_macro_args(Macro *macro, TokenList *args) {
    if (!args) {
        return;
    }
    for (int i = 0; i < macro->param_count; i++) {
        token_list_free(&args[i]);
    }
    free(args);
}

static void expand_tokens(TokenList *tokens, TokenList *out, MacroTable *table, CondStack *cond_stack) {
    for (int i = 0; i < tokens->count; i++) {
        Token *tok = &tokens->data[i];
        if (!cond_stack_is_active(cond_stack)) {
            Token cloned;
            token_clone_into(tok, &cloned);
            token_list_push(out, cloned);
            continue;
        }
        if (tok->type == TOKEN_IDENTIFIER) {
            if (strcmp(tok->text, "__FILE__") == 0) {
                Token file_tok = {TOKEN_STRING, NULL, tok->line, NULL};
                size_t len = strlen(tok->filename) + 3;
                char *buf = (char *)malloc(len);
                snprintf(buf, len, "\"%s\"", tok->filename);
                file_tok.text = buf;
                file_tok.filename = jcpp_strdup(tok->filename);
                token_list_push(out, file_tok);
                continue;
            }
            if (strcmp(tok->text, "__LINE__") == 0) {
                char buf[32];
                snprintf(buf, sizeof(buf), "%d", tok->line);
                Token line_tok = {TOKEN_NUMBER, jcpp_strdup(buf), tok->line, jcpp_strdup(tok->filename)};
                token_list_push(out, line_tok);
                continue;
            }

            Macro *macro = macro_table_find(table, tok->text);
            if (macro && macro->disabled_count == 0) {
                macro->disabled_count++;
                if (macro->is_function_like) {
                    int scan = i + 1;
                    TokenList *args = NULL;
                    if (try_parse_macro_args(tokens, &scan, macro, &args)) {
                        TokenList expanded;
                        token_list_init(&expanded);
                        expand_macro(macro, args, &expanded);
                        expand_tokens(&expanded, out, table, cond_stack);
                        token_list_free(&expanded);
                        free_macro_args(macro, args);
                        macro->disabled_count--;
                        i = scan - 1;
                        continue;
                    }
                    macro->disabled_count--;
                    Token cloned;
                    token_clone_into(tok, &cloned);
                    token_list_push(out, cloned);
                    continue;
                }
                TokenList expanded;
                token_list_init(&expanded);
                TokenList no_args;
                token_list_init(&no_args);
                expand_macro(macro, &no_args, &expanded);
                expand_tokens(&expanded, out, table, cond_stack);
                token_list_free(&no_args);
                token_list_free(&expanded);
                macro->disabled_count--;
                continue;
            }
        }
        Token cloned;
        token_clone_into(tok, &cloned);
        token_list_push(out, cloned);
    }
}

static bool starts_line_directive(TokenList *tokens, int index) {
    int i = index;
    while (i > 0) {
        Token *prev = &tokens->data[i - 1];
        if (prev->type == TOKEN_NEWLINE) {
            break;
        }
        if (prev->type != TOKEN_WHITESPACE) {
            return false;
        }
        i--;
    }
    return true;
}

static char *path_join(const char *left, const char *right) {
    size_t left_len = strlen(left);
    size_t right_len = strlen(right);
    size_t total = left_len + right_len + 2;
    char *buf = (char *)malloc(total);
    snprintf(buf, total, "%s/%s", left, right);
    return buf;
}

static char *dir_from_path(const char *path) {
    const char *slash = strrchr(path, '/');
    if (!slash) {
        return jcpp_strdup(".");
    }
    size_t len = (size_t)(slash - path);
    char *buf = (char *)malloc(len + 1);
    memcpy(buf, path, len);
    buf[len] = '\0';
    return buf;
}

static char *strip_quotes(const char *text) {
    size_t len = strlen(text);
    if (len >= 2 && ((text[0] == '"' && text[len - 1] == '"') || (text[0] == '<' && text[len - 1] == '>'))) {
        char *buf = (char *)malloc(len - 1);
        memcpy(buf, text + 1, len - 2);
        buf[len - 2] = '\0';
        return buf;
    }
    return jcpp_strdup(text);
}

static char *resolve_include(const char *current_file, const char *include_text, bool is_quote) {
    char *name = strip_quotes(include_text);
    char *candidate = NULL;
    if (is_quote && current_file) {
        char *dir = dir_from_path(current_file);
        candidate = path_join(dir, name);
        free(dir);
        FILE *file = fopen(candidate, "rb");
        if (file) {
            fclose(file);
            free(name);
            return candidate;
        }
        free(candidate);
    }

    candidate = path_join("include", name);
    FILE *file = fopen(candidate, "rb");
    if (file) {
        fclose(file);
        free(name);
        return candidate;
    }
    free(candidate);
    free(name);
    return NULL;
}

static void define_macro_from_tokens(TokenList *tokens, int *index, MacroTable *table) {
    int i = *index;
    while (i < tokens->count && token_is_whitespace(&tokens->data[i])) {
        i++;
    }
    if (i >= tokens->count || tokens->data[i].type != TOKEN_IDENTIFIER) {
        *index = i;
        return;
    }
    char *name = tokens->data[i].text;
    Macro *macro = macro_table_add(table, name);
    if (!macro) {
        return;
    }
    i++;

    if (i < tokens->count && token_is_punct(&tokens->data[i], "(")) {
        macro->is_function_like = true;
        i++;
        while (i < tokens->count && !token_is_punct(&tokens->data[i], ")")) {
            if (tokens->data[i].type == TOKEN_IDENTIFIER) {
                macro->params = (char **)realloc(macro->params, sizeof(char *) * (macro->param_count + 1));
                macro->params[macro->param_count++] = jcpp_strdup(tokens->data[i].text);
            }
            i++;
        }
        if (i < tokens->count && token_is_punct(&tokens->data[i], ")")) {
            i++;
        }
    }

    TokenList repl;
    token_list_init(&repl);
    while (i < tokens->count && tokens->data[i].type != TOKEN_NEWLINE) {
        Token cloned;
        token_clone_into(&tokens->data[i], &cloned);
        token_list_push(&repl, cloned);
        i++;
    }
    macro->replacement = repl.data;
    macro->replacement_count = repl.count;

    *index = i;
}

static void handle_directive(TokenList *tokens, int *index, TokenList *out, MacroTable *table, CondStack *cond_stack, const char *current_file, IncludeStack *stack);

static void preprocess_file_raw(const char *filename, TokenList *out, MacroTable *table, IncludeStack *stack) {
    FILE *file = fopen(filename, "rb");
    if (!file) {
        return;
    }
    include_stack_push(stack, file, filename, 1);

    TokenList tokens;
    token_list_init(&tokens);
    read_tokens_from_file(file, filename, &tokens);

    CondStack cond_stack;
    cond_stack_init(&cond_stack);

    for (int i = 0; i < tokens.count; i++) {
        Token *tok = &tokens.data[i];
        if (starts_line_directive(&tokens, i) && token_is_punct(tok, "#")) {
            handle_directive(&tokens, &i, out, table, &cond_stack, filename, stack);
            continue;
        }
        if (!cond_stack_is_active(&cond_stack)) {
            continue;
        }
        Token cloned;
        token_clone_into(tok, &cloned);
        token_list_push(out, cloned);
    }

    cond_stack_free(&cond_stack);
    token_list_free(&tokens);

    FileContext ctx = include_stack_pop(stack);
    if (ctx.file) {
        fclose(ctx.file);
    }
    free(ctx.filename);
}

static void handle_directive(TokenList *tokens, int *index, TokenList *out, MacroTable *table, CondStack *cond_stack, const char *current_file, IncludeStack *stack) {
    int i = *index;
    while (i < tokens->count && token_is_whitespace(&tokens->data[i])) {
        i++;
    }
    if (i >= tokens->count || !token_is_punct(&tokens->data[i], "#")) {
        *index = i;
        return;
    }
    i++;
    while (i < tokens->count && token_is_whitespace(&tokens->data[i])) {
        i++;
    }
    if (i >= tokens->count || tokens->data[i].type != TOKEN_IDENTIFIER) {
        *index = i;
        return;
    }

    const char *directive = tokens->data[i].text;
    i++;

    if (strcmp(directive, "define") == 0) {
        if (cond_stack_is_active(cond_stack)) {
            define_macro_from_tokens(tokens, &i, table);
        }
    } else if (strcmp(directive, "undef") == 0) {
        while (i < tokens->count && token_is_whitespace(&tokens->data[i])) {
            i++;
        }
        if (i < tokens->count && tokens->data[i].type == TOKEN_IDENTIFIER) {
            if (cond_stack_is_active(cond_stack)) {
                macro_table_remove(table, tokens->data[i].text);
            }
            i++;
        }
    } else if (strcmp(directive, "include") == 0) {
        if (cond_stack_is_active(cond_stack)) {
            while (i < tokens->count && token_is_whitespace(&tokens->data[i])) {
                i++;
            }
            if (i < tokens->count && (tokens->data[i].type == TOKEN_STRING || (tokens->data[i].type == TOKEN_PUNCT && (strcmp(tokens->data[i].text, "<") == 0)))) {
                bool is_quote = tokens->data[i].type == TOKEN_STRING;
                char include_buf[512] = {0};
                if (is_quote) {
                    snprintf(include_buf, sizeof(include_buf), "%s", tokens->data[i].text);
                    i++;
                } else {
                    size_t pos = 0;
                    if (pos + 1 < sizeof(include_buf)) {
                        include_buf[pos++] = '<';
                    }
                    i++;
                    while (i < tokens->count && !token_is_punct(&tokens->data[i], ">")) {
                        size_t len = strlen(tokens->data[i].text);
                        if (pos + len + 1 < sizeof(include_buf)) {
                            memcpy(include_buf + pos, tokens->data[i].text, len);
                            pos += len;
                        }
                        i++;
                    }
                    if (pos + 1 < sizeof(include_buf)) {
                        include_buf[pos++] = '>';
                    }
                    include_buf[pos] = '\0';
                    if (i < tokens->count && token_is_punct(&tokens->data[i], ">")) {
                        i++;
                    }
                }
                char *path = resolve_include(current_file, include_buf, is_quote);
                if (path) {
                    preprocess_file_raw(path, out, table, stack);
                    free(path);
                }
            }
        }
    } else if (strcmp(directive, "ifdef") == 0) {
        while (i < tokens->count && token_is_whitespace(&tokens->data[i])) {
            i++;
        }
        bool defined = false;
        if (i < tokens->count && tokens->data[i].type == TOKEN_IDENTIFIER) {
            defined = macro_table_find(table, tokens->data[i].text) != NULL;
            i++;
        }
        CondFrame frame = {0};
        frame.parent_active = cond_stack_is_active(cond_stack);
        frame.active = frame.parent_active && defined;
        frame.any_taken = frame.active;
        cond_stack_push(cond_stack, frame);
    } else if (strcmp(directive, "ifndef") == 0) {
        while (i < tokens->count && token_is_whitespace(&tokens->data[i])) {
            i++;
        }
        bool defined = false;
        if (i < tokens->count && tokens->data[i].type == TOKEN_IDENTIFIER) {
            defined = macro_table_find(table, tokens->data[i].text) != NULL;
            i++;
        }
        CondFrame frame = {0};
        frame.parent_active = cond_stack_is_active(cond_stack);
        frame.active = frame.parent_active && !defined;
        frame.any_taken = frame.active;
        cond_stack_push(cond_stack, frame);
    } else if (strcmp(directive, "if") == 0) {
        TokenList expr;
        token_list_init(&expr);
        while (i < tokens->count && tokens->data[i].type != TOKEN_NEWLINE) {
            Token cloned;
            token_clone_into(&tokens->data[i], &cloned);
            token_list_push(&expr, cloned);
            i++;
        }
        bool ok = eval_if_expr(&expr, table);
        token_list_free(&expr);
        CondFrame frame = {0};
        frame.parent_active = cond_stack_is_active(cond_stack);
        frame.active = frame.parent_active && ok;
        frame.any_taken = frame.active;
        cond_stack_push(cond_stack, frame);
    } else if (strcmp(directive, "elif") == 0) {
        TokenList expr;
        token_list_init(&expr);
        while (i < tokens->count && tokens->data[i].type != TOKEN_NEWLINE) {
            Token cloned;
            token_clone_into(&tokens->data[i], &cloned);
            token_list_push(&expr, cloned);
            i++;
        }
        bool ok = eval_if_expr(&expr, table);
        token_list_free(&expr);
        CondFrame *frame = cond_stack_top(cond_stack);
        if (frame) {
            if (frame->any_taken) {
                frame->active = false;
            } else {
                frame->active = frame->parent_active && ok;
                frame->any_taken = frame->active;
            }
        }
    } else if (strcmp(directive, "else") == 0) {
        CondFrame *frame = cond_stack_top(cond_stack);
        if (frame) {
            if (frame->any_taken) {
                frame->active = false;
            } else {
                frame->active = frame->parent_active;
                frame->any_taken = frame->active;
            }
        }
    } else if (strcmp(directive, "endif") == 0) {
        cond_stack_pop(cond_stack);
    }

    while (i < tokens->count && tokens->data[i].type != TOKEN_NEWLINE) {
        i++;
    }
    *index = i;
}

void preprocess_file(const char *filename, TokenList *out, MacroTable *table, IncludeStack *stack) {
    TokenList raw;
    token_list_init(&raw);
    preprocess_file_raw(filename, &raw, table, stack);

    CondStack cond_stack;
    cond_stack_init(&cond_stack);
    expand_tokens(&raw, out, table, &cond_stack);
    cond_stack_free(&cond_stack);
    token_list_free(&raw);
}
