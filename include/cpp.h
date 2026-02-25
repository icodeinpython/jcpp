#pragma once

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* -------------------------------------------------------------------------
 * Arena allocator
 * ---------------------------------------------------------------------- */

typedef struct ArenaBlock {
    struct ArenaBlock *next;
    size_t             used;
    size_t             cap;
    char               data[];
} ArenaBlock;

typedef struct {
    ArenaBlock *head;
} Arena;

void  arena_init(Arena *a);
void *arena_alloc(Arena *a, size_t sz);
char *arena_strdup(Arena *a, const char *s, size_t len);
void  arena_free_all(Arena *a);

/* -------------------------------------------------------------------------
 * String interning
 * ---------------------------------------------------------------------- */

typedef struct InternEntry {
    struct InternEntry *next;
    unsigned            hash;
    size_t              len;
    char                str[];
} InternEntry;

typedef struct {
    InternEntry **buckets;
    unsigned      cap;
    unsigned      count;
} InternTable;

void        intern_init(InternTable *t);
const char *intern(InternTable *t, const char *s, size_t len);
const char *intern_cstr(InternTable *t, const char *s);
void        intern_free(InternTable *t);

/* -------------------------------------------------------------------------
 * Source location
 * ---------------------------------------------------------------------- */

typedef struct {
    const char *filename; /* interned */
    unsigned    line;
    unsigned    col;
} SrcLoc;

/* -------------------------------------------------------------------------
 * Preprocessing tokens
 * ---------------------------------------------------------------------- */

typedef enum {
    TOK_EOF = 0,
    TOK_IDENT,
    TOK_PP_NUMBER,
    TOK_STRING,
    TOK_CHAR,
    TOK_PUNCT,
    TOK_OTHER,
    TOK_SPACE,
    TOK_NEWLINE,
    TOK_PLACEMARKER,
    TOK_HEADER_NAME,
} TokKind;

/* hide-set: singly-linked list of interned macro names */
typedef struct HideEntry {
    struct HideEntry *next;
    const char       *name; /* interned */
} HideEntry;

#define TOK_INLINE_CAP 23

typedef struct Token {
    struct Token *next;

    TokKind   kind;
    SrcLoc    loc;

    bool      no_expand; /* true if hide set is non-empty */
    HideEntry *hide;

    unsigned  len;
    union {
        char  buf[TOK_INLINE_CAP + 1];
        char *ptr;
    } text;
} Token;

typedef struct {
    Token *head;
    Token *tail;
} TokList;

/* -------------------------------------------------------------------------
 * Macro table
 * ---------------------------------------------------------------------- */

typedef enum {
    MACRO_OBJECT,
    MACRO_FUNCTION,
    MACRO_BUILTIN,
} MacroKind;

typedef struct Macro {
    struct Macro *next; /* hash chain */
    const char   *name; /* interned */
    MacroKind     kind;
    bool          is_variadic;
    bool          va_named;    /* GNU: name... */
    const char   *va_name;     /* interned if va_named */

    unsigned      param_count;
    const char  **params;      /* interned */

    TokList       body;
} Macro;

typedef struct {
    Macro   **buckets;
    unsigned  cap;
    unsigned  count;
} MacroTable;

/* -------------------------------------------------------------------------
 * Include stack
 * ---------------------------------------------------------------------- */

#define MAX_INCLUDE_DEPTH 200

typedef struct {
    const char *logical_name; /* interned */
    const char *real_path;    /* interned */
    char       *buf;
    size_t      buf_len;
    size_t      pos;
    unsigned    line;
    unsigned    col;
    bool        is_system;
    int         cond_depth; /* conds.depth when this file was entered */
} IncludeFrame;

typedef struct {
    IncludeFrame frames[MAX_INCLUDE_DEPTH];
    int          depth; /* -1 = empty */
} IncludeStack;

/* -------------------------------------------------------------------------
 * Include guard cache
 * ---------------------------------------------------------------------- */

typedef struct GuardEntry {
    struct GuardEntry *next;
    const char        *real_path; /* interned */
    const char        *guard;     /* interned macro name, or NULL */
} GuardEntry;

typedef struct {
    GuardEntry **buckets;
    unsigned     cap;
} GuardCache;

/* -------------------------------------------------------------------------
 * Conditional stack
 * ---------------------------------------------------------------------- */

typedef enum {
    COND_ACTIVE,
    COND_SKIP,
    COND_DONE,
} CondState;

typedef struct {
    CondState state;
    SrcLoc    loc;
    bool      had_else;
} CondFrame;

#define MAX_COND_DEPTH 512

typedef struct {
    CondFrame frames[MAX_COND_DEPTH];
    int       depth; /* -1 = empty */
} CondStack;

/* -------------------------------------------------------------------------
 * Top-level preprocessor context
 * ---------------------------------------------------------------------- */

#define MAX_SEARCH_PATHS 256

typedef struct CPP {
    Arena        arena;
    InternTable  strings;
    MacroTable   macros;
    IncludeStack includes;
    CondStack    conds;
    GuardCache   guards;

    const char  *quote_paths[MAX_SEARCH_PATHS];
    unsigned     quote_path_count;
    const char  *sys_paths[MAX_SEARCH_PATHS];
    unsigned     sys_path_count;

    FILE        *out;

    unsigned     error_count;
    unsigned     warning_count;

    char         date_str[20]; /* "\"Mmm dd yyyy\"" */
    char         time_str[14]; /* "\"hh:mm:ss\""  */
    unsigned     counter;

    /* result token stream built during processing */
    TokList      pending;

    unsigned     last_line;
    const char  *last_file;
} CPP;

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

void cpp_init(CPP *cpp, FILE *out);
void cpp_free(CPP *cpp);
void cpp_add_include_path(CPP *cpp, const char *path, bool is_system);
void cpp_define(CPP *cpp, const char *def);
void cpp_undef(CPP *cpp, const char *name);
int  cpp_process_file(CPP *cpp, const char *path);