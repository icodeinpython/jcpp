/*
 * jcpp — A C preprocessor
 * ISO C17 / GNU extensions / glibc compatible
 */

#include "cpp.h"

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <linux/limits.h>

/* Stringify a macro value (two-level expansion to handle numeric macros). */
#define JCPP_STRINGIFY(x) JCPP_STRINGIFY2(x)
#define JCPP_STRINGIFY2(x) #x

/* =========================================================================
 * Helpers
 * ====================================================================== */

static void die(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

static void cpp_error(CPP *cpp, SrcLoc loc, const char *fmt, ...) {
    va_list ap;
    fprintf(stderr, "%s:%u:%u: error: ", loc.filename ? loc.filename : "<?>",
            loc.line, loc.col);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    cpp->error_count++;
}

static void cpp_warn(CPP *cpp, SrcLoc loc, const char *fmt, ...) {
    va_list ap;
    fprintf(stderr, "%s:%u:%u: warning: ", loc.filename ? loc.filename : "<?>",
            loc.line, loc.col);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    cpp->warning_count++;
}

/* =========================================================================
 * Arena allocator
 * ====================================================================== */

#define ARENA_BLOCK_SIZE (1024 * 256)

void arena_init(Arena *a) {
    a->head = NULL;
}

void *arena_alloc(Arena *a, size_t sz) {
    sz = (sz + 7) & ~(size_t)7; /* 8-byte align */
    if (!a->head || a->head->used + sz > a->head->cap) {
        size_t bsz = sz > ARENA_BLOCK_SIZE ? sz : ARENA_BLOCK_SIZE;
        ArenaBlock *b = malloc(sizeof(ArenaBlock) + bsz);
        if (!b) die("out of memory");
        b->next = a->head;
        b->used = 0;
        b->cap  = bsz;
        a->head = b;
    }
    void *p = a->head->data + a->head->used;
    a->head->used += sz;
    return p;
}

char *arena_strdup(Arena *a, const char *s, size_t len) {
    char *p = arena_alloc(a, len + 1);
    memcpy(p, s, len);
    p[len] = '\0';
    return p;
}

void arena_free_all(Arena *a) {
    ArenaBlock *b = a->head;
    while (b) {
        ArenaBlock *next = b->next;
        free(b);
        b = next;
    }
    a->head = NULL;
}

/* =========================================================================
 * String interning
 * ====================================================================== */

static unsigned fnv1a(const char *s, size_t len) {
    unsigned h = 2166136261u;
    for (size_t i = 0; i < len; i++)
        h = (h ^ (unsigned char)s[i]) * 16777619u;
    return h;
}

void intern_init(InternTable *t) {
    t->cap     = 1024;
    t->count   = 0;
    t->buckets = calloc(t->cap, sizeof(InternEntry *));
    if (!t->buckets) die("out of memory");
}

const char *intern(InternTable *t, const char *s, size_t len) {
    unsigned h   = fnv1a(s, len);
    unsigned idx = h & (t->cap - 1);

    for (InternEntry *e = t->buckets[idx]; e; e = e->next)
        if (e->hash == h && e->len == len && memcmp(e->str, s, len) == 0)
            return e->str;

    /* grow if needed */
    if (t->count * 2 >= t->cap) {
        unsigned newcap = t->cap * 2;
        InternEntry **nb = calloc(newcap, sizeof(InternEntry *));
        if (!nb) die("out of memory");
        for (unsigned i = 0; i < t->cap; i++) {
            for (InternEntry *e = t->buckets[i]; e;) {
                InternEntry *nx = e->next;
                unsigned ni = e->hash & (newcap - 1);
                e->next = nb[ni];
                nb[ni]  = e;
                e = nx;
            }
        }
        free(t->buckets);
        t->buckets = nb;
        t->cap     = newcap;
        idx        = h & (newcap - 1);
    }

    InternEntry *e = malloc(sizeof(InternEntry) + len + 1);
    if (!e) die("out of memory");
    e->hash = h;
    e->len  = len;
    memcpy(e->str, s, len);
    e->str[len] = '\0';
    e->next     = t->buckets[idx];
    t->buckets[idx] = e;
    t->count++;
    return e->str;
}

const char *intern_cstr(InternTable *t, const char *s) {
    return intern(t, s, strlen(s));
}

void intern_free(InternTable *t) {
    for (unsigned i = 0; i < t->cap; i++) {
        InternEntry *e = t->buckets[i];
        while (e) {
            InternEntry *nx = e->next;
            free(e);
            e = nx;
        }
    }
    free(t->buckets);
    t->buckets = NULL;
}

/* =========================================================================
 * Token management
 * ====================================================================== */

static const char *tok_text(const Token *t) {
    return t->len <= TOK_INLINE_CAP ? t->text.buf : t->text.ptr;
}

Token *tok_new(Arena *a, TokKind kind, const char *text, size_t len, SrcLoc loc) {
    Token *t  = arena_alloc(a, sizeof(Token));
    t->next     = NULL;
    t->kind     = kind;
    t->loc      = loc;
    t->no_expand = false;
    t->hide     = NULL;
    t->len      = (unsigned)len;
    if (len <= TOK_INLINE_CAP) {
        memcpy(t->text.buf, text, len);
        t->text.buf[len] = '\0';
    } else {
        t->text.ptr = arena_strdup(a, text, len);
    }
    return t;
}

static Token *tok_clone(Arena *a, const Token *src) {
    Token *t = tok_new(a, src->kind, tok_text(src), src->len, src->loc);
    t->no_expand = src->no_expand;
    t->hide      = src->hide; /* share hide set — it's immutable */
    return t;
}

static void toklist_append(TokList *l, Token *t) {
    t->next = NULL;
    if (!l->head) l->head = t;
    else          l->tail->next = t;
    l->tail = t;
}

static void toklist_prepend_list(TokList *dst, TokList src) {
    if (!src.head) return;
    src.tail->next = dst->head;
    dst->head = src.head;
    if (!dst->tail) dst->tail = src.tail;
}

static Token *toklist_shift(TokList *l) {
    if (!l->head) return NULL;
    Token *t  = l->head;
    l->head   = t->next;
    if (!l->head) l->tail = NULL;
    t->next   = NULL;
    return t;
}

/* clone a token list */
static TokList toklist_clone(Arena *a, const TokList *src) {
    TokList dst = {NULL, NULL};
    for (Token *t = src->head; t; t = t->next)
        toklist_append(&dst, tok_clone(a, t));
    return dst;
}

/* =========================================================================
 * Hide-set helpers
 * ====================================================================== */

static bool hideset_contains(HideEntry *h, const char *name) {
    for (; h; h = h->next)
        if (h->name == name) return true;
    return false;
}

static HideEntry *hideset_add(Arena *a, HideEntry *h, const char *name) {
    if (hideset_contains(h, name)) return h;
    HideEntry *e = arena_alloc(a, sizeof(HideEntry));
    e->name = name;
    e->next = h;
    return e;
}

/* Add macro name to hide set of every token in list */
static void hideset_add_list(Arena *a, TokList *l, const char *name) {
    for (Token *t = l->head; t; t = t->next) {
        t->hide      = hideset_add(a, t->hide, name);
        t->no_expand = true;
    }
}

/* Intersect two hide sets (used after ## pasting) */
static HideEntry *hideset_intersect(Arena *a, HideEntry *a1, HideEntry *b) {
    HideEntry *result = NULL;
    for (HideEntry *e = a1; e; e = e->next)
        if (hideset_contains(b, e->name))
            result = hideset_add(a, result, e->name);
    return result;
}

/* =========================================================================
 * Macro table
 * ====================================================================== */

#define MACRO_TABLE_INIT_CAP 256

static void macro_table_init(MacroTable *mt) {
    mt->cap     = MACRO_TABLE_INIT_CAP;
    mt->count   = 0;
    mt->buckets = calloc(mt->cap, sizeof(Macro *));
    if (!mt->buckets) die("out of memory");
}

static unsigned macro_hash(const char *name) {
    return fnv1a(name, strlen(name));
}

static Macro *macro_find(MacroTable *mt, const char *name) {
    unsigned idx = macro_hash(name) & (mt->cap - 1);
    for (Macro *m = mt->buckets[idx]; m; m = m->next)
        if (m->name == name) return m;
    return NULL;
}

static void macro_insert(MacroTable *mt, Macro *m) {
    if (mt->count * 2 >= mt->cap) {
        unsigned newcap = mt->cap * 2;
        Macro **nb = calloc(newcap, sizeof(Macro *));
        if (!nb) die("out of memory");
        for (unsigned i = 0; i < mt->cap; i++) {
            for (Macro *e = mt->buckets[i]; e;) {
                Macro *nx = e->next;
                unsigned ni = macro_hash(e->name) & (newcap - 1);
                e->next = nb[ni];
                nb[ni]  = e;
                e = nx;
            }
        }
        free(mt->buckets);
        mt->buckets = nb;
        mt->cap     = newcap;
    }
    unsigned idx = macro_hash(m->name) & (mt->cap - 1);
    m->next = mt->buckets[idx];
    mt->buckets[idx] = m;
    mt->count++;
}

static void macro_remove(MacroTable *mt, const char *name) {
    unsigned idx = macro_hash(name) & (mt->cap - 1);
    Macro **pp = &mt->buckets[idx];
    while (*pp) {
        if ((*pp)->name == name) {
            Macro *dead = *pp;
            *pp = dead->next;
            mt->count--;
            /* memory is in arena, no free needed */
            return;
        }
        pp = &(*pp)->next;
    }
}

/* =========================================================================
 * Guard cache
 * ====================================================================== */

static void guard_cache_init(GuardCache *gc) {
    gc->cap     = 512;
    gc->buckets = calloc(gc->cap, sizeof(GuardEntry *));
    if (!gc->buckets) die("out of memory");
}

static void guard_cache_set(GuardCache *gc, const char *real_path,
                             const char *guard) {
    unsigned idx = fnv1a(real_path, strlen(real_path)) & (gc->cap - 1);
    for (GuardEntry *e = gc->buckets[idx]; e; e = e->next)
        if (e->real_path == real_path) { e->guard = guard; return; }
    GuardEntry *e = malloc(sizeof(GuardEntry));
    if (!e) die("out of memory");
    e->real_path = real_path;
    e->guard     = guard;
    e->next      = gc->buckets[idx];
    gc->buckets[idx] = e;
}

static const char *guard_cache_get(GuardCache *gc, const char *real_path) {
    unsigned idx = fnv1a(real_path, strlen(real_path)) & (gc->cap - 1);
    for (GuardEntry *e = gc->buckets[idx]; e; e = e->next)
        if (e->real_path == real_path) return e->guard;
    return NULL; /* not cached */
}

static bool guard_cache_has(GuardCache *gc, const char *real_path) {
    unsigned idx = fnv1a(real_path, strlen(real_path)) & (gc->cap - 1);
    for (GuardEntry *e = gc->buckets[idx]; e; e = e->next)
        if (e->real_path == real_path) return true;
    return false;
}

/* =========================================================================
 * Source reader — handles trigraph + backslash-newline splicing
 * ====================================================================== */

typedef struct {
    const char *buf;
    size_t      pos;
    size_t      len;
    unsigned    line;
    unsigned    col;
    const char *filename; /* interned */
} Reader;

static void reader_init(Reader *r, const char *buf, size_t len,
                        const char *filename) {
    r->buf      = buf;
    r->pos      = 0;
    r->len      = len;
    r->line     = 1;
    r->col      = 1;
    r->filename = filename;
}

static SrcLoc reader_loc(const Reader *r) {
    return (SrcLoc){ r->filename, r->line, r->col };
}

/* Peek at raw byte without consuming */
static int reader_raw_peek(const Reader *r, size_t offset) {
    size_t p = r->pos + offset;
    return p < r->len ? (unsigned char)r->buf[p] : -1;
}

/* Resolve trigraph at r->pos. Returns replacement char or -1 if none. */
static int try_trigraph(const Reader *r) {
    if (reader_raw_peek(r, 0) != '?' || reader_raw_peek(r, 1) != '?')
        return -1;
    switch (reader_raw_peek(r, 2)) {
        case '=':  return '#';
        case '/':  return '\\';
        case '\'': return '^';
        case '(':  return '[';
        case ')':  return ']';
        case '!':  return '|';
        case '<':  return '{';
        case '>':  return '}';
        case '-':  return '~';
    }
    return -1;
}

/* Get one logical character (after trigraph + splice processing).
   Returns -1 at EOF. */
static int reader_get(Reader *r) {
    for (;;) {
        if (r->pos >= r->len) return -1;

        /* trigraph */
        int tg = try_trigraph(r);
        int c;
        unsigned adv;
        if (tg >= 0) {
            c = tg; adv = 3;
        } else {
            c = (unsigned char)r->buf[r->pos]; adv = 1;
        }

        r->pos += adv;

        /* backslash-newline splice */
        if (c == '\\') {
            /* peek next logical char for newline */
            size_t saved_pos  = r->pos;
            unsigned saved_ln = r->line;
            unsigned saved_col = r->col;
            int nc;
            /* check for trigraph \n */
            int tg2 = try_trigraph(r);
            if (tg2 >= 0) nc = tg2;
            else          nc = reader_raw_peek(r, 0);
            if (nc == '\n') {
                if (tg2 >= 0) r->pos += 3; else r->pos++;
                r->line++;
                r->col = 1;
                (void)saved_col;
                (void)saved_ln;
                continue; /* splice: discard both, continue */
            }
            /* not a splice — restore and return the backslash */
            r->pos = saved_pos;
            r->col++;
            return c;
        }

        if (c == '\n') { r->line++; r->col = 1; }
        else           { r->col++; }

        return c;
    }
}

/* Peek one logical character without consuming */
static int reader_peek(Reader *r) {
    size_t   sp = r->pos;
    unsigned sl = r->line, sc = r->col;
    int c = reader_get(r);
    r->pos  = sp;
    r->line = sl;
    r->col  = sc;
    return c;
}

/* =========================================================================
 * Lexer — produces preprocessing tokens from a Reader
 * ====================================================================== */

typedef struct {
    Reader  r;
    Arena  *arena;
    CPP    *cpp;
} Lexer;

static void lexer_init(Lexer *l, CPP *cpp, const char *buf, size_t len,
                       const char *filename) {
    reader_init(&l->r, buf, len, filename);
    l->arena = &cpp->arena;
    l->cpp   = cpp;
}

/* string builder */
typedef struct {
    char    *buf;
    size_t   len;
    size_t   cap;
} StrBuf;

static void sb_init(StrBuf *sb) { sb->buf = NULL; sb->len = 0; sb->cap = 0; }
static void sb_push(StrBuf *sb, char c) {
    if (sb->len + 1 >= sb->cap) {
        sb->cap = sb->cap ? sb->cap * 2 : 64;
        sb->buf = realloc(sb->buf, sb->cap);
        if (!sb->buf) die("out of memory");
    }
    sb->buf[sb->len++] = c;
    sb->buf[sb->len]   = '\0';
}
static void sb_free(StrBuf *sb) { free(sb->buf); sb->buf = NULL; sb->len = sb->cap = 0; }

static bool is_ident_start(int c) {
    return isalpha(c) || c == '_';
}
static bool is_ident_cont(int c) {
    return isalnum(c) || c == '_';
}

static Token *lex_next(Lexer *l) {
    Reader *r = &l->r;
    StrBuf  sb;
    sb_init(&sb);
    Token  *tok = NULL;

    for (;;) {
        int c = reader_peek(r);
        if (c == -1) {
            tok = tok_new(l->arena, TOK_EOF, "", 0, reader_loc(r));
            break;
        }

        SrcLoc loc = reader_loc(r);

        /* whitespace (non-newline) */
        if (c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v') {
            sb_push(&sb, (char)reader_get(r));
            while ((c = reader_peek(r)) == ' ' || c == '\t' || c == '\r' ||
                   c == '\f' || c == '\v')
                sb_push(&sb, (char)reader_get(r));
            tok = tok_new(l->arena, TOK_SPACE, sb.buf, sb.len, loc);
            break;
        }

        /* newline */
        if (c == '\n') {
            reader_get(r);
            tok = tok_new(l->arena, TOK_NEWLINE, "\n", 1, loc);
            break;
        }

        /* line comment */
        if (c == '/' && ({
                size_t sp = r->pos; unsigned sl = r->line, sc = r->col;
                reader_get(r); int nc = reader_peek(r);
                r->pos = sp; r->line = sl; r->col = sc;
                nc == '/'; })) {
            reader_get(r); reader_get(r); /* consume // */
            while ((c = reader_peek(r)) != -1 && c != '\n')
                reader_get(r);
            tok = tok_new(l->arena, TOK_SPACE, " ", 1, loc);
            break;
        }

        /* block comment */
        if (c == '/' && ({
                size_t sp = r->pos; unsigned sl = r->line, sc = r->col;
                reader_get(r); int nc = reader_peek(r);
                r->pos = sp; r->line = sl; r->col = sc;
                nc == '*'; })) {
            reader_get(r); reader_get(r); /* consume slash-star */
            int prev = 0;
            while ((c = reader_get(r)) != -1) {
                if (prev == '*' && c == '/') break;
                prev = c;
            }
            tok = tok_new(l->arena, TOK_SPACE, " ", 1, loc);
            break;
        }

        /* identifier or keyword */
        if (is_ident_start(c)) {
            /* check string prefix: u8, u, U, L */
            int peek2 = -1;
            {
                size_t sp = r->pos; unsigned sl = r->line, sc = r->col;
                int fc = reader_get(r);
                (void)fc;
                peek2 = reader_peek(r);
                r->pos = sp; r->line = sl; r->col = sc;
            }
            if ((c == 'u' || c == 'U' || c == 'L') &&
                (peek2 == '"' || peek2 == '\'')) {
                /* string/char with encoding prefix */
                sb_push(&sb, (char)reader_get(r));
                goto do_string_or_char;
            }
            if (c == 'u' && peek2 == '8') {
                /* check for u8" */
                size_t sp = r->pos; unsigned sl = r->line, sc = r->col;
                reader_get(r); reader_get(r);
                int p3 = reader_peek(r);
                r->pos = sp; r->line = sl; r->col = sc;
                if (p3 == '"') {
                    sb_push(&sb, (char)reader_get(r));
                    sb_push(&sb, (char)reader_get(r));
                    goto do_string_or_char;
                }
            }
            sb_push(&sb, (char)reader_get(r));
            while (is_ident_cont(reader_peek(r)))
                sb_push(&sb, (char)reader_get(r));
            tok = tok_new(l->arena, TOK_IDENT, sb.buf, sb.len, loc);
            break;
        }

        /* pp-number */
        if (isdigit(c) || (c == '.' && ({
                size_t sp = r->pos; unsigned sl = r->line, sc = r->col;
                reader_get(r); int nc = reader_peek(r);
                r->pos = sp; r->line = sl; r->col = sc;
                isdigit(nc); }))) {
            sb_push(&sb, (char)reader_get(r));
            for (;;) {
                c = reader_peek(r);
                if (c == 'e' || c == 'E' || c == 'p' || c == 'P') {
                    size_t sp = r->pos; unsigned sl = r->line, sc = r->col;
                    sb_push(&sb, (char)reader_get(r));
                    int nc = reader_peek(r);
                    if (nc == '+' || nc == '-') {
                        sb_push(&sb, (char)reader_get(r));
                    } else {
                        r->pos = sp; r->line = sl; r->col = sc;
                        sb.len--;
                        break;
                    }
                } else if (isalnum(c) || c == '_' || c == '.') {
                    sb_push(&sb, (char)reader_get(r));
                } else {
                    break;
                }
            }
            tok = tok_new(l->arena, TOK_PP_NUMBER, sb.buf, sb.len, loc);
            break;
        }

        /* string / char literal */
        do_string_or_char:
        if (c == '"' || c == '\'') {
            int delim = (c == '"') ? '"' : '\'';
            TokKind kind = (delim == '"') ? TOK_STRING : TOK_CHAR;
            sb_push(&sb, (char)reader_get(r));
            while ((c = reader_get(r)) != -1) {
                sb_push(&sb, (char)c);
                if (c == '\\') {
                    int nc = reader_get(r);
                    if (nc != -1) sb_push(&sb, (char)nc);
                } else if (c == delim) {
                    break;
                } else if (c == '\n') {
                    cpp_warn(l->cpp, loc, "unterminated string/char literal");
                    break;
                }
            }
            tok = tok_new(l->arena, kind, sb.buf, sb.len, loc);
            break;
        }
        /* handle case where we set prefix and then fell through */
        if (sb.len > 0 && (c == '"' || c == '\'')) goto do_string_or_char;

        /* punctuation / operators — multi-char first */
        reader_get(r); /* consume first char */
        int c2 = reader_peek(r);
        char pc[4]; pc[0] = (char)c; pc[3] = '\0';

        /* 3-char operators */
        if (c2 != -1) {
            size_t sp = r->pos; unsigned sl = r->line, sc = r->col;
            reader_get(r);
            int c3 = reader_peek(r);
            r->pos = sp; r->line = sl; r->col = sc;
            if (c == '.' && c2 == '.' && c3 == '.') {
                reader_get(r); reader_get(r);
                tok = tok_new(l->arena, TOK_PUNCT, "...", 3, loc); break;
            }
            if (c == '<' && c2 == '<' && c3 == '=') {
                reader_get(r); reader_get(r);
                tok = tok_new(l->arena, TOK_PUNCT, "<<=", 3, loc); break;
            }
            if (c == '>' && c2 == '>' && c3 == '=') {
                reader_get(r); reader_get(r);
                tok = tok_new(l->arena, TOK_PUNCT, ">>=", 3, loc); break;
            }
            if (c == '#' && c2 == '#') {
                reader_get(r);
                tok = tok_new(l->arena, TOK_PUNCT, "##", 2, loc); break;
            }
        }

        /* 2-char operators */
#define CHECK2(a, b, s) if ((int)(unsigned char)(a) == c && c2 == (b)) \
    { reader_get(r); tok = tok_new(l->arena, TOK_PUNCT, s, 2, loc); break; }
        CHECK2('<','<', "<<") CHECK2('>','>', ">>")
        CHECK2('<','=', "<=") CHECK2('>','=', ">=")
        CHECK2('=','=', "==") CHECK2('!','=', "!=")
        CHECK2('&','&', "&&") CHECK2('|','|', "||")
        CHECK2('+','=', "+=") CHECK2('-','=', "-=")
        CHECK2('*','=', "*=") CHECK2('/','=', "/=")
        CHECK2('%','=', "%=") CHECK2('&','=', "&=")
        CHECK2('|','=', "|=") CHECK2('^','=', "^=")
        CHECK2('+','+', "++") CHECK2('-','-', "--")
        CHECK2('-','>', "->") CHECK2(':',':', "::")
        CHECK2('#','#', "##")
#undef CHECK2

        /* single char */
        pc[1] = '\0';
        tok = tok_new(l->arena, TOK_PUNCT, pc, 1, loc);
        break;
    }

    sb_free(&sb);
    return tok;
}

/* Lex an entire buffer into a TokList (terminated with TOK_EOF) */
static TokList lex_buffer(CPP *cpp, const char *buf, size_t len,
                           const char *filename) {
    Lexer l;
    lexer_init(&l, cpp, buf, len, filename);
    TokList out = {NULL, NULL};
    for (;;) {
        Token *t = lex_next(&l);
        toklist_append(&out, t);
        if (t->kind == TOK_EOF) break;
    }
    return out;
}

/* =========================================================================
 * File I/O helpers
 * ====================================================================== */

static char *read_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    char *buf = malloc((size_t)sz + 2);
    if (!buf) { fclose(f); die("out of memory"); }
    size_t nr = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[nr] = '\n'; /* ensure file ends with newline */
    buf[nr + 1] = '\0';
    *out_len = nr + 1;
    return buf;
}

/* =========================================================================
 * Path resolution helpers
 * ====================================================================== */

static const char *path_dirname(CPP *cpp, const char *path) {
    const char *last = strrchr(path, '/');
    if (!last) return intern_cstr(&cpp->strings, ".");
    size_t len = (size_t)(last - path);
    return intern((&cpp->strings), path, len);
}

static const char *path_join(CPP *cpp, const char *dir, const char *file) {
    char buf[PATH_MAX];
    if (dir[0] == '\0' || strcmp(dir, ".") == 0)
        snprintf(buf, sizeof(buf), "%s", file);
    else
        snprintf(buf, sizeof(buf), "%s/%s", dir, file);
    return intern_cstr(&cpp->strings, buf);
}

static bool file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

/* =========================================================================
 * Forward declarations for mutual recursion
 * ====================================================================== */

static TokList expand(CPP *cpp, TokList tokens);
static void push_file(CPP *cpp, const char *real_path, const char *logical,
                      bool is_system);

/* =========================================================================
 * Constant expression evaluator (#if / #elif)
 * ====================================================================== */

typedef struct {
    TokList  tokens;
    Token   *cur;
    CPP     *cpp;
    SrcLoc   loc;
    bool     ok;
} ExprParser;

static long long eval_expr(ExprParser *ep);

static Token *ep_peek(ExprParser *ep) { return ep->cur; }

static int ep_peek_kind(ExprParser *ep) {
    return ep->cur ? ep->cur->kind : TOK_EOF;
}

static const char *ep_peek_text(ExprParser *ep) {
    return ep->cur ? tok_text(ep->cur) : "";
}

static void ep_advance(ExprParser *ep) {
    if (ep->cur && ep->cur->kind != TOK_EOF) {
        ep->cur = ep->cur->next;
        /* skip spaces */
        while (ep->cur && ep->cur->kind == TOK_SPACE)
            ep->cur = ep->cur->next;
    }
}

static long long parse_primary(ExprParser *ep) {
    Token *t = ep_peek(ep);
    if (!t || t->kind == TOK_EOF) { ep->ok = false; return 0; }

    /* defined(X) or defined X */
    if (t->kind == TOK_IDENT && strcmp(tok_text(t), "defined") == 0) {
        ep_advance(ep);
        bool paren = false;
        if (ep_peek_kind(ep) == TOK_PUNCT &&
            strcmp(ep_peek_text(ep), "(") == 0) {
            paren = true;
            ep_advance(ep);
        }
        const char *name = ep_peek_kind(ep) == TOK_IDENT
                           ? tok_text(ep_peek(ep)) : "";
        const char *iname = intern_cstr(&ep->cpp->strings, name);
        ep_advance(ep);
        if (paren) {
            if (!(ep_peek_kind(ep) == TOK_PUNCT &&
                  strcmp(ep_peek_text(ep), ")") == 0)) {
                cpp_error(ep->cpp, ep->loc, "missing ')' after defined");
                ep->ok = false;
            } else {
                ep_advance(ep);
            }
        }
        return macro_find(&ep->cpp->macros, iname) ? 1 : 0;
    }

    /* identifier not defined => 0 */
    if (t->kind == TOK_IDENT) {
        ep_advance(ep);
        return 0;
    }

    /* integer constant */
    if (t->kind == TOK_PP_NUMBER) {
        ep_advance(ep);
        const char *s = tok_text(t);
        char *end;
        long long v;
        /* handle 0x, 0b, octal */
        if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
            v = (long long)strtoull(s + 2, &end, 16);
        else if (s[0] == '0' && (s[1] == 'b' || s[1] == 'B'))
            v = (long long)strtoull(s + 2, &end, 2);
        else
            v = strtoll(s, &end, 0);
        return v;
    }

    /* parenthesised */
    if (t->kind == TOK_PUNCT && strcmp(tok_text(t), "(") == 0) {
        ep_advance(ep);
        long long v = eval_expr(ep);
        if (!(ep_peek_kind(ep) == TOK_PUNCT &&
              strcmp(ep_peek_text(ep), ")") == 0)) {
            cpp_error(ep->cpp, ep->loc, "missing ')' in #if expression");
            ep->ok = false;
        } else {
            ep_advance(ep);
        }
        return v;
    }

    /* char constant — simplified */
    if (t->kind == TOK_CHAR) {
        ep_advance(ep);
        const char *s = tok_text(t);
        if (s[0] == '\'' && s[1] != '\0')
            return (unsigned char)s[1];
        return 0;
    }

    cpp_error(ep->cpp, t->loc, "unexpected token in #if: %s", tok_text(t));
    ep->ok = false;
    ep_advance(ep);
    return 0;
}

static long long parse_unary(ExprParser *ep) {
    if (ep_peek_kind(ep) == TOK_PUNCT) {
        const char *s = ep_peek_text(ep);
        if (strcmp(s, "+") == 0) { ep_advance(ep); return  parse_unary(ep); }
        if (strcmp(s, "-") == 0) { ep_advance(ep); return -parse_unary(ep); }
        if (strcmp(s, "~") == 0) { ep_advance(ep); return ~parse_unary(ep); }
        if (strcmp(s, "!") == 0) { ep_advance(ep); return !parse_unary(ep); }
    }
    return parse_primary(ep);
}

static long long parse_mul(ExprParser *ep) {
    long long v = parse_unary(ep);
    for (;;) {
        if (ep_peek_kind(ep) != TOK_PUNCT) break;
        const char *s = ep_peek_text(ep);
        if (strcmp(s, "*") == 0) { ep_advance(ep); v *= parse_unary(ep); }
        else if (strcmp(s, "/") == 0) {
            ep_advance(ep);
            long long r = parse_unary(ep);
            if (r == 0) { cpp_error(ep->cpp, ep->loc, "division by zero in #if"); ep->ok = false; return 0; }
            v /= r;
        } else if (strcmp(s, "%") == 0) {
            ep_advance(ep);
            long long r = parse_unary(ep);
            if (r == 0) { cpp_error(ep->cpp, ep->loc, "modulo by zero in #if"); ep->ok = false; return 0; }
            v %= r;
        } else break;
    }
    return v;
}

static long long parse_add(ExprParser *ep) {
    long long v = parse_mul(ep);
    for (;;) {
        if (ep_peek_kind(ep) != TOK_PUNCT) break;
        const char *s = ep_peek_text(ep);
        if      (strcmp(s, "+") == 0) { ep_advance(ep); v += parse_mul(ep); }
        else if (strcmp(s, "-") == 0) { ep_advance(ep); v -= parse_mul(ep); }
        else break;
    }
    return v;
}

static long long parse_shift(ExprParser *ep) {
    long long v = parse_add(ep);
    for (;;) {
        if (ep_peek_kind(ep) != TOK_PUNCT) break;
        const char *s = ep_peek_text(ep);
        if      (strcmp(s, "<<") == 0) { ep_advance(ep); v <<= parse_add(ep); }
        else if (strcmp(s, ">>") == 0) { ep_advance(ep); v >>= parse_add(ep); }
        else break;
    }
    return v;
}

static long long parse_rel(ExprParser *ep) {
    long long v = parse_shift(ep);
    for (;;) {
        if (ep_peek_kind(ep) != TOK_PUNCT) break;
        const char *s = ep_peek_text(ep);
        if      (strcmp(s, "<")  == 0) { ep_advance(ep); v = v <  parse_shift(ep); }
        else if (strcmp(s, ">")  == 0) { ep_advance(ep); v = v >  parse_shift(ep); }
        else if (strcmp(s, "<=") == 0) { ep_advance(ep); v = v <= parse_shift(ep); }
        else if (strcmp(s, ">=") == 0) { ep_advance(ep); v = v >= parse_shift(ep); }
        else break;
    }
    return v;
}

static long long parse_eq(ExprParser *ep) {
    long long v = parse_rel(ep);
    for (;;) {
        if (ep_peek_kind(ep) != TOK_PUNCT) break;
        const char *s = ep_peek_text(ep);
        if      (strcmp(s, "==") == 0) { ep_advance(ep); v = v == parse_rel(ep); }
        else if (strcmp(s, "!=") == 0) { ep_advance(ep); v = v != parse_rel(ep); }
        else break;
    }
    return v;
}

static long long parse_band(ExprParser *ep) {
    long long v = parse_eq(ep);
    while (ep_peek_kind(ep) == TOK_PUNCT && strcmp(ep_peek_text(ep), "&") == 0) {
        ep_advance(ep); v &= parse_eq(ep);
    }
    return v;
}

static long long parse_bxor(ExprParser *ep) {
    long long v = parse_band(ep);
    while (ep_peek_kind(ep) == TOK_PUNCT && strcmp(ep_peek_text(ep), "^") == 0) {
        ep_advance(ep); v ^= parse_band(ep);
    }
    return v;
}

static long long parse_bor(ExprParser *ep) {
    long long v = parse_bxor(ep);
    while (ep_peek_kind(ep) == TOK_PUNCT && strcmp(ep_peek_text(ep), "|") == 0) {
        ep_advance(ep); v |= parse_bxor(ep);
    }
    return v;
}

static long long parse_land(ExprParser *ep) {
    long long v = parse_bor(ep);
    while (ep_peek_kind(ep) == TOK_PUNCT && strcmp(ep_peek_text(ep), "&&") == 0) {
        ep_advance(ep);
        long long r = parse_bor(ep);
        v = v && r;
    }
    return v;
}

static long long parse_lor(ExprParser *ep) {
    long long v = parse_land(ep);
    while (ep_peek_kind(ep) == TOK_PUNCT && strcmp(ep_peek_text(ep), "||") == 0) {
        ep_advance(ep);
        long long r = parse_land(ep);
        v = v || r;
    }
    return v;
}

static long long eval_expr(ExprParser *ep) {
    long long cond = parse_lor(ep);
    if (ep_peek_kind(ep) == TOK_PUNCT && strcmp(ep_peek_text(ep), "?") == 0) {
        ep_advance(ep);
        long long t = eval_expr(ep);
        if (!(ep_peek_kind(ep) == TOK_PUNCT && strcmp(ep_peek_text(ep), ":") == 0)) {
            cpp_error(ep->cpp, ep->loc, "missing ':' in ternary");
            ep->ok = false;
            return 0;
        }
        ep_advance(ep);
        long long f = eval_expr(ep);
        return cond ? t : f;
    }
    return cond;
}

/* Evaluate a token list as a constant expression.
   Performs defined() extraction then macro expansion first. */
static long long eval_if_expr(CPP *cpp, TokList line, SrcLoc loc) {
    /* Step 1: replace defined(X)/defined X with 0/1 BEFORE expansion */
    TokList predefined = {NULL, NULL};
    for (Token *t = line.head; t && t->kind != TOK_EOF && t->kind != TOK_NEWLINE; ) {
        if (t->kind == TOK_IDENT && strcmp(tok_text(t), "defined") == 0) {
            Token *next = t->next;
            /* skip spaces */
            while (next && next->kind == TOK_SPACE) next = next->next;
            bool paren = false;
            if (next && next->kind == TOK_PUNCT && strcmp(tok_text(next), "(") == 0) {
                paren = true;
                next = next->next;
                while (next && next->kind == TOK_SPACE) next = next->next;
            }
            const char *mname = "";
            if (next && next->kind == TOK_IDENT) {
                mname = intern_cstr(&cpp->strings, tok_text(next));
                next = next->next;
            }
            if (paren) {
                while (next && next->kind == TOK_SPACE) next = next->next;
                if (next && next->kind == TOK_PUNCT && strcmp(tok_text(next), ")") == 0)
                    next = next->next;
            }
            long long val = macro_find(&cpp->macros, mname) ? 1 : 0;
            char buf[4]; snprintf(buf, sizeof(buf), "%lld", val);
            Token *nt = tok_new(&cpp->arena, TOK_PP_NUMBER, buf, strlen(buf), t->loc);
            toklist_append(&predefined, nt);
            t = next;
        } else {
            Token *nt = tok_clone(&cpp->arena, t);
            /* mark all idents (except defined) as no_expand for builtin
               macros — but we want expansion, so DON'T mark; just append */
            toklist_append(&predefined, nt);
            t = t->next;
        }
    }

    /* Step 2: macro-expand the result */
    TokList expanded = expand(cpp, predefined);

    /* Step 3: evaluate */
    ExprParser ep;
    ep.tokens = expanded;
    ep.cpp    = cpp;
    ep.loc    = loc;
    ep.ok     = true;
    /* skip leading spaces */
    ep.cur = expanded.head;
    while (ep.cur && ep.cur->kind == TOK_SPACE) ep.cur = ep.cur->next;

    long long v = eval_expr(&ep);
    return v;
}

/* =========================================================================
 * Macro expansion engine
 * ====================================================================== */

/* Read function-like macro arguments.
   Assumes the opening '(' has already been consumed.
   Returns array of TokLists (one per arg). */
/* Trim trailing TOK_SPACE tokens from an arg list */
static void trim_arg_trailing(TokList *arg) {
    while (arg->tail && arg->tail->kind == TOK_SPACE) {
        if (arg->head == arg->tail) { arg->head = arg->tail = NULL; return; }
        Token *p = arg->head;
        while (p->next != arg->tail) p = p->next;
        p->next = NULL; arg->tail = p;
    }
}

static TokList *read_args(CPP *cpp, TokList *stream, unsigned *out_count,
                           unsigned expected, bool is_variadic) {
    unsigned cap  = (expected > 2 ? expected : 2) + 4;
    TokList *args = arena_alloc(&cpp->arena, sizeof(TokList) * cap);
    unsigned count = 0;
    memset(args, 0, sizeof(TokList) * cap);

    int  depth = 0;
    bool done  = false;

    while (!done) {
        Token *t = toklist_shift(stream);
        if (!t || t->kind == TOK_EOF) break;

        /* newlines inside argument list count as whitespace */
        if (t->kind == TOK_NEWLINE) {
            t->kind = TOK_SPACE;
            t->len  = 1;
        }

        if (t->kind == TOK_PUNCT) {
            const char *s = tok_text(t);

            if (strcmp(s, "(") == 0) {
                depth++;
                if (count >= cap) goto grow;
                toklist_append(&args[count], t);
                continue;
            }
            if (strcmp(s, ")") == 0) {
                if (depth == 0) { done = true; break; }
                depth--;
                if (count >= cap) goto grow;
                toklist_append(&args[count], t);
                continue;
            }
            if (strcmp(s, ",") == 0 && depth == 0) {
                bool is_va_slot = is_variadic &&
                                  (count + 1 >= expected);
                if (!is_va_slot) {
                    trim_arg_trailing(&args[count]);
                    count++;
                    if (count >= cap) {
                        TokList *na = arena_alloc(&cpp->arena,
                                          sizeof(TokList) * cap * 2);
                        memcpy(na, args, sizeof(TokList) * cap);
                        memset(na + cap, 0, sizeof(TokList) * cap);
                        args = na; cap *= 2;
                    }
                    continue;
                }
                /* variadic: include the comma in the VA arg */
                if (count >= cap) goto grow;
                toklist_append(&args[count], t);
                continue;
            }
        }

        /* skip leading whitespace at start of each arg */
        if (t->kind == TOK_SPACE && !args[count].head) continue;

        if (count >= cap) {
            grow: {
                TokList *na = arena_alloc(&cpp->arena,
                                  sizeof(TokList) * cap * 2);
                memcpy(na, args, sizeof(TokList) * cap);
                memset(na + cap, 0, sizeof(TokList) * cap);
                args = na; cap *= 2;
            }
        }
        toklist_append(&args[count], t);
    }

    /* trim trailing whitespace from last arg */
    if (count < cap) trim_arg_trailing(&args[count]);

    /* F() with no args should yield one empty arg for arity-1 macros */
    if (count == 0 && expected > 0 && !args[0].head)
        *out_count = 1;
    else
        *out_count = count + (done ? 1 : 0);

    return args;
}

/* Stringify a raw argument token list */
static Token *stringify_arg(CPP *cpp, TokList arg, SrcLoc loc) {
    StrBuf sb;
    sb_init(&sb);
    sb_push(&sb, '"');

    bool first = true;
    for (Token *t = arg.head; t; t = t->next) {
        if (t->kind == TOK_SPACE || t->kind == TOK_NEWLINE) {
            if (!first) sb_push(&sb, ' ');
            continue;
        }
        first = false;
        const char *s = tok_text(t);
        /* Escape backslashes and quotes */
        if (t->kind == TOK_STRING || t->kind == TOK_CHAR) {
            for (size_t i = 0; s[i]; i++) {
                if (s[i] == '"' || s[i] == '\\') sb_push(&sb, '\\');
                sb_push(&sb, s[i]);
            }
        } else {
            for (size_t i = 0; s[i]; i++)
                sb_push(&sb, s[i]);
        }
    }
    sb_push(&sb, '"');

    Token *r = tok_new(&cpp->arena, TOK_STRING, sb.buf, sb.len, loc);
    sb_free(&sb);
    return r;
}

/* Paste two tokens: return new token or NULL on error */
static Token *paste_tokens(CPP *cpp, Token *lhs, Token *rhs) {
    if (lhs->kind == TOK_PLACEMARKER) return tok_clone(&cpp->arena, rhs);
    if (rhs->kind == TOK_PLACEMARKER) return tok_clone(&cpp->arena, lhs);

    size_t llen = lhs->len, rlen = rhs->len;
    char *combined = malloc(llen + rlen + 1);
    if (!combined) die("out of memory");
    memcpy(combined, tok_text(lhs), llen);
    memcpy(combined + llen, tok_text(rhs), rlen);
    combined[llen + rlen] = '\0';

    /* Re-lex the combined string as a single token */
    TokList result = lex_buffer(cpp, combined, llen + rlen,
                                 lhs->loc.filename);
    free(combined);

    Token *t = result.head;
    /* skip to first non-space real token */
    while (t && (t->kind == TOK_SPACE || t->kind == TOK_NEWLINE)) t = t->next;

    if (!t || t->kind == TOK_EOF) {
        /* empty result — placemarker */
        return tok_new(&cpp->arena, TOK_PLACEMARKER, "", 0, lhs->loc);
    }

    /* should be exactly one token */
    Token *result_tok = tok_clone(&cpp->arena, t);
    result_tok->loc = lhs->loc;

    /* intersect hide sets */
    result_tok->hide = hideset_intersect(&cpp->arena, lhs->hide, rhs->hide);
    result_tok->no_expand = (result_tok->hide != NULL);

    return result_tok;
}

/* Substitute parameters in macro body.
   raw_args: un-expanded args (for #)
   exp_args: pre-expanded args (for normal substitution)
   Returns a new TokList. */
static TokList substitute(CPP *cpp, const Macro *m, TokList *raw_args,
                           TokList *exp_args) {
    TokList out = {NULL, NULL};

    for (Token *t = m->body.head; t; t = t->next) {
        /* # stringification */
        if (t->kind == TOK_PUNCT && strcmp(tok_text(t), "#") == 0) {
            Token *next = t->next;
            while (next && next->kind == TOK_SPACE) next = next->next;
            if (!next) continue;
            /* find param index */
            int pi = -1;
            if (next->kind == TOK_IDENT) {
                const char *pn = intern_cstr(&cpp->strings, tok_text(next));
                for (unsigned i = 0; i < m->param_count; i++)
                    if (m->params[i] == pn) { pi = (int)i; break; }
                if (m->is_variadic) {
                    const char *va = m->va_named ? m->va_name
                                                 : intern_cstr(&cpp->strings, "__VA_ARGS__");
                    if (pn == va) pi = (int)m->param_count;
                }
            }
            if (pi >= 0) {
                Token *str = stringify_arg(cpp,
                    pi < (int)m->param_count ? raw_args[pi] :
                    raw_args[m->param_count], t->loc);
                toklist_append(&out, str);
                t = next;
            }
            continue;
        }

        /* ## token paste */
        if (t->kind == TOK_PUNCT && strcmp(tok_text(t), "##") == 0) {
            Token *rhs_tok = t->next;
            while (rhs_tok && rhs_tok->kind == TOK_SPACE)
                rhs_tok = rhs_tok->next;
            if (!rhs_tok) continue;

            /* get LHS (last token in out, or placemarker) */
            Token *lhs_tok;
            if (out.tail) {
                lhs_tok = out.tail;
                /* detach tail */
                if (out.head == out.tail) {
                    out.head = out.tail = NULL;
                } else {
                    Token *p = out.head;
                    while (p->next != out.tail) p = p->next;
                    p->next = NULL;
                    out.tail = p;
                }
            } else {
                lhs_tok = tok_new(&cpp->arena, TOK_PLACEMARKER, "", 0, t->loc);
            }

            /* resolve rhs: if it's a param, use raw arg */
            Token *rhs_resolved = NULL;
            if (rhs_tok->kind == TOK_IDENT) {
                const char *pn = intern_cstr(&cpp->strings, tok_text(rhs_tok));
                int rpi = -1;
                for (unsigned i = 0; i < m->param_count; i++)
                    if (m->params[i] == pn) { rpi = (int)i; break; }
                if (m->is_variadic) {
                    const char *va = m->va_named ? m->va_name
                                                 : intern_cstr(&cpp->strings, "__VA_ARGS__");
                    if (pn == va) rpi = (int)m->param_count;
                }
                if (rpi >= 0) {
                    TokList ra = (rpi < (int)m->param_count)
                                 ? raw_args[rpi] : raw_args[m->param_count];
                    if (!ra.head) {
                        rhs_resolved = tok_new(&cpp->arena, TOK_PLACEMARKER,
                                               "", 0, rhs_tok->loc);
                    } else {
                        /* paste each token of the arg individually? For VA
                           with multiple tokens, paste yields a horror.
                           Standard says: paste arg token by token.
                           Simple: paste lhs with first token, result with rest */
                        rhs_resolved = tok_clone(&cpp->arena, ra.head);
                        Token *pasted = paste_tokens(cpp, lhs_tok, rhs_resolved);
                        if (pasted) toklist_append(&out, pasted);
                        /* append rest of arg */
                        for (Token *at = ra.head ? ra.head->next : NULL;
                             at; at = at->next)
                            toklist_append(&out, tok_clone(&cpp->arena, at));
                        t = rhs_tok;
                        continue;
                    }
                }
            }
            if (!rhs_resolved) rhs_resolved = tok_clone(&cpp->arena, rhs_tok);

            Token *pasted = paste_tokens(cpp, lhs_tok, rhs_resolved);
            if (pasted) toklist_append(&out, pasted);
            t = rhs_tok;
            continue;
        }

        /* parameter substitution */
        if (t->kind == TOK_IDENT) {
            const char *pn = intern_cstr(&cpp->strings, tok_text(t));
            int pi = -1;
            for (unsigned i = 0; i < m->param_count; i++)
                if (m->params[i] == pn) { pi = (int)i; break; }

            /* __VA_ARGS__ / named va */
            if (m->is_variadic && pi < 0) {
                const char *va = m->va_named ? m->va_name
                                             : intern_cstr(&cpp->strings, "__VA_ARGS__");
                if (pn == va) pi = (int)m->param_count;
            }

            if (pi >= 0) {
                /* check if adjacent to ## on the right */
                Token *nxt = t->next;
                while (nxt && nxt->kind == TOK_SPACE) nxt = nxt->next;
                bool raw_mode = (nxt && nxt->kind == TOK_PUNCT &&
                                 strcmp(tok_text(nxt), "##") == 0);

                TokList arg = raw_mode
                    ? ((pi < (int)m->param_count) ? raw_args[pi]
                                                   : raw_args[m->param_count])
                    : ((pi < (int)m->param_count) ? exp_args[pi]
                                                   : exp_args[m->param_count]);

                if (!arg.head) {
                    /* empty arg => placemarker */
                    if (raw_mode) {
                        toklist_append(&out,
                            tok_new(&cpp->arena, TOK_PLACEMARKER, "", 0, t->loc));
                    }
                } else {
                    for (Token *at = arg.head; at; at = at->next)
                        toklist_append(&out, tok_clone(&cpp->arena, at));
                }
                continue;
            }
        }

        /* ordinary token */
        toklist_append(&out, tok_clone(&cpp->arena, t));
    }

    return out;
}

/* The main expansion engine.
   Takes a token stream and returns the fully expanded version. */
static TokList expand(CPP *cpp, TokList input) {
    TokList out    = {NULL, NULL};
    TokList stream = input; /* working stream */

    while (stream.head) {
        Token *t = toklist_shift(&stream);

        /* non-idents pass through */
        if (t->kind != TOK_IDENT) { toklist_append(&out, t); continue; }

        /* suppress expansion? */
        const char *name = intern_cstr(&cpp->strings, tok_text(t));

        if (t->no_expand && hideset_contains(t->hide, name)) {
            toklist_append(&out, t);
            continue;
        }

        /* built-in macros */
        if (name == intern_cstr(&cpp->strings, "__FILE__")) {
            const char *fn = cpp->includes.depth >= 0
                ? intern_cstr(&cpp->strings,
                    cpp->includes.frames[cpp->includes.depth].logical_name)
                : intern_cstr(&cpp->strings, "<unknown>");
            size_t flen = strlen(fn);
            char *s = malloc(flen + 3);
            s[0] = '"'; memcpy(s + 1, fn, flen); s[flen + 1] = '"'; s[flen + 2] = '\0';
            Token *r = tok_new(&cpp->arena, TOK_STRING, s, flen + 2, t->loc);
            free(s);
            toklist_append(&out, r);
            continue;
        }
        if (name == intern_cstr(&cpp->strings, "__LINE__")) {
            unsigned ln = cpp->includes.depth >= 0
                ? cpp->includes.frames[cpp->includes.depth].line : 0;
            char buf[16]; snprintf(buf, sizeof(buf), "%u", ln);
            toklist_append(&out, tok_new(&cpp->arena, TOK_PP_NUMBER,
                                          buf, strlen(buf), t->loc));
            continue;
        }
        if (name == intern_cstr(&cpp->strings, "__COUNTER__")) {
            char buf[16]; snprintf(buf, sizeof(buf), "%u", cpp->counter++);
            toklist_append(&out, tok_new(&cpp->arena, TOK_PP_NUMBER,
                                          buf, strlen(buf), t->loc));
            continue;
        }
        if (name == intern_cstr(&cpp->strings, "__DATE__")) {
            toklist_append(&out, tok_new(&cpp->arena, TOK_STRING,
                cpp->date_str, strlen(cpp->date_str), t->loc));
            continue;
        }
        if (name == intern_cstr(&cpp->strings, "__TIME__")) {
            toklist_append(&out, tok_new(&cpp->arena, TOK_STRING,
                cpp->time_str, strlen(cpp->time_str), t->loc));
            continue;
        }

        Macro *m = macro_find(&cpp->macros, name);
        if (!m) { toklist_append(&out, t); continue; }

        if (m->kind == MACRO_OBJECT) {
            /* expand body and prepend to stream */
            TokList body = toklist_clone(&cpp->arena, &m->body);
            hideset_add_list(&cpp->arena, &body, name);
            /* give tokens the call site location */
            for (Token *tok = body.head; tok; tok = tok->next)
                tok->loc = t->loc;
            toklist_prepend_list(&stream, body);
            continue;
        }

        if (m->kind == MACRO_FUNCTION) {
            /* find '(' — skip spaces */
            Token *peek = stream.head;
            while (peek && peek->kind == TOK_SPACE) peek = peek->next;

            if (!peek || peek->kind != TOK_PUNCT ||
                strcmp(tok_text(peek), "(") != 0) {
                /* not a call — treat as non-macro */
                toklist_append(&out, t);
                continue;
            }

            /* consume tokens up to and including '(' */
            while (stream.head && stream.head->kind == TOK_SPACE)
                toklist_shift(&stream);
            toklist_shift(&stream); /* consume '(' */

            unsigned arg_count;
            TokList *raw_args = read_args(cpp, &stream, &arg_count,
                                           m->param_count, m->is_variadic);

            /* validate arg count */
            unsigned expected = m->param_count + (m->is_variadic ? 1 : 0);
            if (!m->is_variadic && arg_count != m->param_count) {
                cpp_error(cpp, t->loc,
                    "macro '%s' requires %u arguments, got %u",
                    m->name, m->param_count, arg_count);
            }

            /* ensure va arg exists even if empty */
            if (m->is_variadic && arg_count <= m->param_count) {
                /* already allocated by read_args, just zero */
            }

            /* GNU: ##__VA_ARGS__ comma swallowing:
               If variadic arg is empty and the token before ## is a comma,
               remove the comma. We handle this in substitute() via placemarker. */

            /* pre-expand each named arg */
            TokList *exp_args = arena_alloc(&cpp->arena,
                                            sizeof(TokList) * (expected + 1));
            for (unsigned i = 0; i <= expected; i++) {
                TokList raw = (i < arg_count) ? raw_args[i]
                                               : (TokList){NULL, NULL};
                exp_args[i] = expand(cpp, toklist_clone(&cpp->arena, &raw));
            }

            TokList body = substitute(cpp, m, raw_args, exp_args);
            hideset_add_list(&cpp->arena, &body, name);
            for (Token *tok = body.head; tok; tok = tok->next)
                tok->loc = t->loc;
            toklist_prepend_list(&stream, body);
            continue;
        }

        toklist_append(&out, t);
    }

    return out;
}

/* =========================================================================
 * #define parser
 * ====================================================================== */

static void parse_define(CPP *cpp, TokList rest, SrcLoc loc) {
    /* skip leading spaces */
    while (rest.head && rest.head->kind == TOK_SPACE)
        toklist_shift(&rest);

    Token *name_tok = toklist_shift(&rest);
    if (!name_tok || name_tok->kind != TOK_IDENT) {
        cpp_error(cpp, loc, "#define requires a macro name");
        return;
    }
    const char *name = intern_cstr(&cpp->strings, tok_text(name_tok));

    Macro *m = arena_alloc(&cpp->arena, sizeof(Macro));
    memset(m, 0, sizeof(Macro));
    m->name = name;

    /* Peek raw: if immediately followed by '(' (no space), function-like */
    Token *peek = rest.head;
    if (peek && peek->kind == TOK_PUNCT && strcmp(tok_text(peek), "(") == 0) {
        /* function-like */
        m->kind = MACRO_FUNCTION;
        toklist_shift(&rest); /* consume '(' */

        /* parse parameter list */
        unsigned cap = 8;
        m->params = arena_alloc(&cpp->arena, sizeof(const char *) * cap);
        m->param_count = 0;

        while (rest.head) {
            while (rest.head && rest.head->kind == TOK_SPACE)
                toklist_shift(&rest);
            Token *pt = toklist_shift(&rest);
            if (!pt) break;
            if (pt->kind == TOK_PUNCT && strcmp(tok_text(pt), ")") == 0) break;
            if (pt->kind == TOK_PUNCT && strcmp(tok_text(pt), "...") == 0) {
                m->is_variadic = true;
                /* skip to ')' */
                while (rest.head && !(rest.head->kind == TOK_PUNCT &&
                       strcmp(tok_text(rest.head), ")") == 0))
                    toklist_shift(&rest);
                toklist_shift(&rest); /* consume ')' */
                break;
            }
            if (pt->kind == TOK_IDENT) {
                /* GNU: name... */
                while (rest.head && rest.head->kind == TOK_SPACE)
                    toklist_shift(&rest);
                if (rest.head && rest.head->kind == TOK_PUNCT &&
                    strcmp(tok_text(rest.head), "...") == 0) {
                    toklist_shift(&rest); /* consume ... */
                    m->is_variadic = true;
                    m->va_named    = true;
                    m->va_name     = intern_cstr(&cpp->strings, tok_text(pt));
                    while (rest.head && !(rest.head->kind == TOK_PUNCT &&
                           strcmp(tok_text(rest.head), ")") == 0))
                        toklist_shift(&rest);
                    toklist_shift(&rest);
                    break;
                }
                if (m->param_count >= cap) {
                    const char **np = arena_alloc(&cpp->arena,
                                                   sizeof(const char *) * cap * 2);
                    memcpy(np, m->params, sizeof(const char *) * cap);
                    m->params = np; cap *= 2;
                }
                m->params[m->param_count++] = intern_cstr(&cpp->strings, tok_text(pt));
                /* skip comma */
                while (rest.head && rest.head->kind == TOK_SPACE)
                    toklist_shift(&rest);
                if (rest.head && rest.head->kind == TOK_PUNCT &&
                    strcmp(tok_text(rest.head), ",") == 0)
                    toklist_shift(&rest);
            } else if (pt->kind == TOK_PUNCT && strcmp(tok_text(pt), ",") == 0) {
                continue;
            } else {
                cpp_error(cpp, pt->loc, "invalid parameter in #define");
            }
        }
    } else if (peek && peek->kind == TOK_SPACE) {
        /* object-like — skip one space */
        toklist_shift(&rest);
        m->kind = MACRO_OBJECT;
    } else {
        m->kind = MACRO_OBJECT;
    }

    /* remainder is the replacement list — collect, strip leading+trailing space */
    TokList body = {NULL, NULL};
    for (Token *t = rest.head; t && t->kind != TOK_NEWLINE &&
         t->kind != TOK_EOF; t = t->next) {
        toklist_append(&body, tok_clone(&cpp->arena, t));
    }
    /* strip leading whitespace */
    while (body.head && body.head->kind == TOK_SPACE) {
        body.head = body.head->next;
        if (!body.head) body.tail = NULL;
    }
    /* strip trailing whitespace */
    while (body.tail && body.tail->kind == TOK_SPACE) {
        if (body.head == body.tail) { body.head = body.tail = NULL; break; }
        Token *p = body.head;
        while (p->next != body.tail) p = p->next;
        p->next = NULL; body.tail = p;
    }
    m->body = body;

    /* Check for redefinition */
    Macro *existing = macro_find(&cpp->macros, name);
    if (existing) {
        /* TODO: compare bodies for identical redefinition (ISO allows), warn otherwise */
        macro_remove(&cpp->macros, name);
    }

    macro_insert(&cpp->macros, m);
}

/* =========================================================================
 * #include path resolution
 * ====================================================================== */

static const char *find_include(CPP *cpp, const char *filename, bool is_angled,
                                 bool is_next) {
    /* current file directory */
    const char *cur_dir = ".";
    int start_frame = cpp->includes.depth;
    if (start_frame >= 0) {
        cur_dir = path_dirname(cpp,
            cpp->includes.frames[start_frame].real_path);
    }

    if (!is_angled && !is_next) {
        /* try current directory first */
        const char *p = path_join(cpp, cur_dir, filename);
        if (file_exists(p)) return p;
        /* then quote paths */
        for (unsigned i = 0; i < cpp->quote_path_count; i++) {
            p = path_join(cpp, cpp->quote_paths[i], filename);
            if (file_exists(p)) return p;
        }
    }

    if (is_next) {
        /* find where current file is in the search paths, start after */
        bool found_current = false;
        /* check quote paths */
        for (unsigned i = 0; i < cpp->quote_path_count; i++) {
            const char *p = path_join(cpp, cpp->quote_paths[i], filename);
            if (!found_current) {
                /* check if current file comes from this path */
                const char *maybe = path_join(cpp, cpp->quote_paths[i],
                    cpp->includes.frames[start_frame].logical_name);
                if (maybe == cpp->includes.frames[start_frame].real_path ||
                    (file_exists(maybe) && strcmp(maybe,
                     cpp->includes.frames[start_frame].real_path) == 0)) {
                    found_current = true; continue;
                }
                continue;
            }
            if (file_exists(p)) return p;
        }
        for (unsigned i = 0; i < cpp->sys_path_count; i++) {
            const char *p = path_join(cpp, cpp->sys_paths[i], filename);
            if (!found_current) {
                const char *maybe = path_join(cpp, cpp->sys_paths[i],
                    cpp->includes.frames[start_frame].logical_name);
                if (file_exists(maybe) && strcmp(maybe,
                     cpp->includes.frames[start_frame].real_path) == 0) {
                    found_current = true; continue;
                }
                continue;
            }
            if (file_exists(p)) return p;
        }
        return NULL;
    }

    /* system paths */
    for (unsigned i = 0; i < cpp->sys_path_count; i++) {
        const char *p = path_join(cpp, cpp->sys_paths[i], filename);
        if (file_exists(p)) return p;
    }
    return NULL;
}

/* =========================================================================
 * Output emitter
 * ====================================================================== */

static void emit_line_marker(CPP *cpp, unsigned line, const char *file,
                              int flag) {
    fprintf(cpp->out, "# %u \"%s\"", line, file);
    /* flags is a bitmask: bit 0 → flag 1 (new file), bit 1 → flag 2 (return),
     * bit 2 → flag 3 (system header), bit 3 → flag 4 (extern "C").
     * Emit each set bit as its individual flag number, space-separated. */
    for (int i = 0; i < 4; i++)
        if (flag & (1 << i)) fprintf(cpp->out, " %d", i + 1);
    fputc('\n', cpp->out);
    cpp->last_line = line;
    cpp->last_file = file;
}

/* =========================================================================
 * Directive processing
 * ====================================================================== */

static void do_pragma(CPP *cpp, TokList rest, SrcLoc loc) {
    /* skip spaces */
    while (rest.head && rest.head->kind == TOK_SPACE) toklist_shift(&rest);
    if (!rest.head || rest.head->kind == TOK_NEWLINE ||
        rest.head->kind == TOK_EOF) return;

    /* GCC system_header */
    if (rest.head->kind == TOK_IDENT &&
        strcmp(tok_text(rest.head), "GCC") == 0) {
        Token *t = rest.head->next;
        while (t && t->kind == TOK_SPACE) t = t->next;
        if (t && t->kind == TOK_IDENT &&
            strcmp(tok_text(t), "system_header") == 0) {
            if (cpp->includes.depth >= 0)
                cpp->includes.frames[cpp->includes.depth].is_system = true;
            return;
        }
    }

    /* #pragma once */
    if (rest.head->kind == TOK_IDENT &&
        strcmp(tok_text(rest.head), "once") == 0) {
        if (cpp->includes.depth >= 0) {
            const char *rp =
                cpp->includes.frames[cpp->includes.depth].real_path;
            guard_cache_set(&cpp->guards, rp, NULL);
        }
        return;
    }

    /* pass through unknown pragmas */
    fprintf(cpp->out, "#pragma");
    for (Token *t = rest.head; t && t->kind != TOK_NEWLINE &&
         t->kind != TOK_EOF; t = t->next)
        fputs(tok_text(t), cpp->out);
    fputc('\n', cpp->out);
    (void)loc;
}

static void push_file(CPP *cpp, const char *real_path, const char *logical,
                       bool is_system) {
    if (cpp->includes.depth + 1 >= MAX_INCLUDE_DEPTH) {
        SrcLoc loc = {logical, 0, 0};
        cpp_error(cpp, loc, "#include nested too deeply");
        return;
    }

    /* Check pragma once / guard cache */
    const char *ipath = intern_cstr(&cpp->strings, real_path);
    if (guard_cache_has(&cpp->guards, ipath)) {
        const char *guard = guard_cache_get(&cpp->guards, ipath);
        if (guard == NULL) return; /* #pragma once — already included */
        if (macro_find(&cpp->macros, guard)) return; /* guard macro defined */
    }

    size_t buflen;
    char *buf = read_file(real_path, &buflen);
    if (!buf) {
        SrcLoc loc = {logical, 0, 0};
        cpp_error(cpp, loc, "cannot open file '%s': %s",
                  real_path, strerror(errno));
        return;
    }

    cpp->includes.depth++;
    IncludeFrame *f = &cpp->includes.frames[cpp->includes.depth];
    f->logical_name = intern_cstr(&cpp->strings, logical);
    f->real_path    = ipath;
    f->buf          = buf;
    f->buf_len      = buflen;
    f->pos          = 0;
    f->line         = 1;
    f->col          = 1;
    f->is_system    = is_system;
    f->cond_depth   = cpp->conds.depth; /* snapshot for unterminated-#if check */

    emit_line_marker(cpp, 1, logical, 1 | (is_system ? 4 : 0));
}

static void pop_file(CPP *cpp) {
    if (cpp->includes.depth < 0) return;
    IncludeFrame *f = &cpp->includes.frames[cpp->includes.depth];
    free(f->buf);
    f->buf = NULL;
    cpp->includes.depth--;

    if (cpp->includes.depth >= 0) {
        IncludeFrame *pf = &cpp->includes.frames[cpp->includes.depth];
        emit_line_marker(cpp, pf->line, pf->logical_name, 2 |
                         (pf->is_system ? 4 : 0));
    }
}

/* Process a single #include directive */
static void do_include(CPP *cpp, TokList rest, SrcLoc loc, bool is_next) {
    /* skip spaces */
    while (rest.head && rest.head->kind == TOK_SPACE) toklist_shift(&rest);
    if (!rest.head) { cpp_error(cpp, loc, "empty #include"); return; }

    bool is_angled = false;
    const char *filename = NULL;

    Token *t = rest.head;
    if (t->kind == TOK_STRING) {
        /* "file.h" */
        const char *s = tok_text(t);
        size_t len = strlen(s);
        /* strip quotes */
        char *fn = arena_strdup(&cpp->arena, s + 1, len - 2);
        filename = fn;
        is_angled = false;
    } else if (t->kind == TOK_PUNCT && strcmp(tok_text(t), "<") == 0) {
        /* <file.h> — collect until > */
        StrBuf sb; sb_init(&sb);
        t = t->next;
        while (t && !(t->kind == TOK_PUNCT && strcmp(tok_text(t), ">") == 0)) {
            if (t->kind == TOK_NEWLINE || t->kind == TOK_EOF) break;
            const char *s = tok_text(t);
            for (size_t i = 0; s[i]; i++) sb_push(&sb, s[i]);
            t = t->next;
        }
        filename = arena_strdup(&cpp->arena, sb.buf, sb.len);
        sb_free(&sb);
        is_angled = true;
    } else if (t->kind == TOK_IDENT) {
        /* macro-generated include */
        TokList sublist = {NULL, NULL};
        for (Token *tok = t; tok && tok->kind != TOK_NEWLINE &&
             tok->kind != TOK_EOF; tok = tok->next)
            toklist_append(&sublist, tok_clone(&cpp->arena, tok));
        TokList expanded = expand(cpp, sublist);

        /* re-parse */
        TokList fake = {NULL, NULL};
        toklist_append(&fake, tok_new(&cpp->arena, TOK_PUNCT, "#", 1, loc));
        Token *inc = tok_new(&cpp->arena, TOK_IDENT, "include", 7, loc);
        toklist_append(&fake, inc);
        Token *sp = tok_new(&cpp->arena, TOK_SPACE, " ", 1, loc);
        toklist_append(&fake, sp);
        for (Token *e = expanded.head; e; e = e->next)
            toklist_append(&fake, tok_clone(&cpp->arena, e));
        do_include(cpp, expanded, loc, is_next);
        return;
    } else {
        cpp_error(cpp, loc, "invalid #include syntax");
        return;
    }

    const char *real_path = find_include(cpp, filename, is_angled, is_next);
    if (!real_path) {
        cpp_error(cpp, loc, "%s file not found: '%s'",
                  is_angled ? "system" : "", filename);
        return;
    }

    push_file(cpp, real_path, filename, is_angled);
}

/* Convert token list to a null-terminated string (for error messages) */
static char *toklist_to_str(CPP *cpp, TokList tl) {
    StrBuf sb; sb_init(&sb);
    for (Token *t = tl.head; t && t->kind != TOK_NEWLINE &&
         t->kind != TOK_EOF; t = t->next) {
        const char *s = tok_text(t);
        for (size_t i = 0; s[i]; i++) sb_push(&sb, s[i]);
    }
    char *r = arena_strdup(&cpp->arena, sb.buf, sb.len);
    sb_free(&sb);
    return r;
}

/* Process one directive line (tokens after the '#') */
static void process_directive(CPP *cpp, TokList rest, SrcLoc loc) {
    /* skip spaces */
    while (rest.head && rest.head->kind == TOK_SPACE) toklist_shift(&rest);
    if (!rest.head || rest.head->kind == TOK_NEWLINE ||
        rest.head->kind == TOK_EOF) return; /* null directive */

    Token *dir = toklist_shift(&rest);
    if (dir->kind != TOK_IDENT && dir->kind != TOK_PP_NUMBER) {
        cpp_error(cpp, loc, "invalid preprocessing directive");
        return;
    }

    const char *dname = tok_text(dir);
    int depth = cpp->conds.depth;
    bool active = (depth < 0 || cpp->conds.frames[depth].state == COND_ACTIVE);

    /* Conditionals must be processed even in inactive branches */
    if (strcmp(dname, "if") == 0) {
        if (depth + 1 >= MAX_COND_DEPTH) {
            cpp_error(cpp, loc, "#if nesting too deep"); return;
        }
        cpp->conds.depth++;
        CondFrame *cf = &cpp->conds.frames[cpp->conds.depth];
        cf->loc       = loc;
        cf->had_else  = false;
        if (!active) {
            cf->state = COND_DONE; /* parent inactive */
        } else {
            long long v = eval_if_expr(cpp, rest, loc);
            cf->state = v ? COND_ACTIVE : COND_SKIP;
        }
        return;
    }
    if (strcmp(dname, "ifdef") == 0) {
        while (rest.head && rest.head->kind == TOK_SPACE) toklist_shift(&rest);
        const char *mname = (rest.head && rest.head->kind == TOK_IDENT)
            ? intern_cstr(&cpp->strings, tok_text(rest.head)) : "";
        if (depth + 1 >= MAX_COND_DEPTH) {
            cpp_error(cpp, loc, "#ifdef nesting too deep"); return;
        }
        cpp->conds.depth++;
        CondFrame *cf = &cpp->conds.frames[cpp->conds.depth];
        cf->loc = loc; cf->had_else = false;
        if (!active)
            cf->state = COND_DONE;
        else
            cf->state = macro_find(&cpp->macros, mname) ? COND_ACTIVE : COND_SKIP;
        return;
    }
    if (strcmp(dname, "ifndef") == 0) {
        while (rest.head && rest.head->kind == TOK_SPACE) toklist_shift(&rest);
        const char *mname = (rest.head && rest.head->kind == TOK_IDENT)
            ? intern_cstr(&cpp->strings, tok_text(rest.head)) : "";
        if (depth + 1 >= MAX_COND_DEPTH) {
            cpp_error(cpp, loc, "#ifndef nesting too deep"); return;
        }
        cpp->conds.depth++;
        CondFrame *cf = &cpp->conds.frames[cpp->conds.depth];
        cf->loc = loc; cf->had_else = false;
        if (!active)
            cf->state = COND_DONE;
        else
            cf->state = !macro_find(&cpp->macros, mname) ? COND_ACTIVE : COND_SKIP;
        return;
    }
    if (strcmp(dname, "elif") == 0) {
        if (depth < 0) { cpp_error(cpp, loc, "#elif without #if"); return; }
        CondFrame *cf = &cpp->conds.frames[depth];
        if (cf->had_else) { cpp_error(cpp, loc, "#elif after #else"); return; }
        if (cf->state == COND_ACTIVE) {
            cf->state = COND_DONE;
        } else if (cf->state == COND_SKIP) {
            /* evaluate condition only if parent is active */
            bool parent_active = (depth == 0 ||
                cpp->conds.frames[depth - 1].state == COND_ACTIVE);
            if (parent_active) {
                long long v = eval_if_expr(cpp, rest, loc);
                cf->state = v ? COND_ACTIVE : COND_SKIP;
            }
        }
        /* COND_DONE stays DONE */
        return;
    }
    if (strcmp(dname, "else") == 0) {
        if (depth < 0) { cpp_error(cpp, loc, "#else without #if"); return; }
        CondFrame *cf = &cpp->conds.frames[depth];
        if (cf->had_else) { cpp_error(cpp, loc, "duplicate #else"); return; }
        cf->had_else = true;
        if      (cf->state == COND_ACTIVE) cf->state = COND_DONE;
        else if (cf->state == COND_SKIP)   cf->state = COND_ACTIVE;
        /* DONE stays DONE */
        return;
    }
    if (strcmp(dname, "endif") == 0) {
        if (depth < 0) { cpp_error(cpp, loc, "#endif without #if"); return; }
        cpp->conds.depth--;
        return;
    }

    /* all other directives: only process in active branch */
    if (!active) return;

    if (strcmp(dname, "define") == 0) {
        parse_define(cpp, rest, loc);
    } else if (strcmp(dname, "undef") == 0) {
        while (rest.head && rest.head->kind == TOK_SPACE) toklist_shift(&rest);
        if (rest.head && rest.head->kind == TOK_IDENT) {
            const char *mname = intern_cstr(&cpp->strings, tok_text(rest.head));
            macro_remove(&cpp->macros, mname);
        }
    } else if (strcmp(dname, "include") == 0) {
        /* expand macros in the include path first */
        do_include(cpp, rest, loc, false);
        return; /* push_file handles the frame */
    } else if (strcmp(dname, "include_next") == 0) {
        do_include(cpp, rest, loc, true);
        return;
    } else if (strcmp(dname, "line") == 0) {
        while (rest.head && rest.head->kind == TOK_SPACE) toklist_shift(&rest);
        if (rest.head && rest.head->kind == TOK_PP_NUMBER) {
            unsigned ln = (unsigned)strtoul(tok_text(rest.head), NULL, 10);
            if (cpp->includes.depth >= 0)
                cpp->includes.frames[cpp->includes.depth].line = ln;
            rest.head = rest.head->next;
            while (rest.head && rest.head->kind == TOK_SPACE)
                rest.head = rest.head->next;
            if (rest.head && rest.head->kind == TOK_STRING) {
                const char *s = tok_text(rest.head);
                size_t slen = strlen(s);
                char *fn = arena_strdup(&cpp->arena, s + 1, slen - 2);
                const char *ifn = intern_cstr(&cpp->strings, fn);
                if (cpp->includes.depth >= 0)
                    cpp->includes.frames[cpp->includes.depth].logical_name = ifn;
            }
        }
    } else if (strcmp(dname, "error") == 0) {
        char *msg = toklist_to_str(cpp, rest);
        cpp_error(cpp, loc, "#error%s%s", msg[0] ? " " : "", msg);
    } else if (strcmp(dname, "warning") == 0) {
        char *msg = toklist_to_str(cpp, rest);
        cpp_warn(cpp, loc, "#warning%s%s", msg[0] ? " " : "", msg);
    } else if (strcmp(dname, "pragma") == 0) {
        do_pragma(cpp, rest, loc);
    } else {
        /* unknown directive */
        if (cpp->includes.depth < 0 ||
            !cpp->includes.frames[cpp->includes.depth].is_system) {
            cpp_warn(cpp, loc, "unknown directive: #%s", dname);
        }
    }
}

/* =========================================================================
 * Main processing loop
 * ====================================================================== */

/* Check whether we're in an active conditional branch */
static bool is_active(CPP *cpp) {
    int d = cpp->conds.depth;
    return d < 0 || cpp->conds.frames[d].state == COND_ACTIVE;
}

/* Lex one logical line from a frame (advances frame position/line/col).
   Returns a TokList without the trailing newline.
   Sets *hit_eof when the file is exhausted. */
static TokList lex_one_line(CPP *cpp, IncludeFrame *f, bool *hit_eof) {
    Lexer l;
    /* Create a lexer starting at the frame's current read position */
    reader_init(&l.r, f->buf + f->pos, f->buf_len - f->pos, f->logical_name);
    l.r.line = f->line;
    l.r.col  = f->col;
    l.arena  = &cpp->arena;
    l.cpp    = cpp;

    TokList line = {NULL, NULL};
    *hit_eof = false;

    for (;;) {
        Token *t = lex_next(&l);
        if (t->kind == TOK_EOF) {
            *hit_eof = true;
            break;
        }
        if (t->kind == TOK_NEWLINE) break;
        toklist_append(&line, t);
    }

    /* Commit consumed bytes back to the frame */
    f->pos  += l.r.pos;
    f->line  = l.r.line;
    f->col   = l.r.col;
    return line;
}

/* Emit one expanded line (no trailing newline in toks) */
static void emit_line_tokens(CPP *cpp, TokList toks, unsigned line,
                              const char *filename) {
    /* Line marker if file or line changed discontinuously */
    if (filename != cpp->last_file ||
        line != cpp->last_line + 1) {
        emit_line_marker(cpp, line, filename, 0);
    }
    cpp->last_file = filename;
    cpp->last_line = line;

    bool need_space = false;
    for (Token *t = toks.head; t; t = t->next) {
        if (t->kind == TOK_PLACEMARKER || t->kind == TOK_EOF ||
            t->kind == TOK_NEWLINE) continue;
        if (t->kind == TOK_SPACE) { need_space = true; continue; }
        if (need_space) { fputc(' ', cpp->out); need_space = false; }
        fputs(tok_text(t), cpp->out);
    }
    fputc('\n', cpp->out);
}

/* Handle _Pragma operator: remove it, execute the pragma, return cleaned list */
static TokList handle_pragma_operator(CPP *cpp, TokList toks) {
    TokList out = {NULL, NULL};
    for (Token *t = toks.head; t; ) {
        Token *next = t->next; /* save before toklist_append zeroes t->next */
        if (t->kind == TOK_IDENT &&
            strcmp(tok_text(t), "_Pragma") == 0) {
            Token *n = next;
            while (n && n->kind == TOK_SPACE) n = n->next;
            if (n && n->kind == TOK_PUNCT && strcmp(tok_text(n), "(") == 0) {
                n = n->next;
                while (n && n->kind == TOK_SPACE) n = n->next;
                if (n && n->kind == TOK_STRING) {
                    Token *str = n;
                    n = n->next;
                    while (n && n->kind == TOK_SPACE) n = n->next;
                    if (n && n->kind == TOK_PUNCT &&
                        strcmp(tok_text(n), ")") == 0) {
                        const char *s = tok_text(str);
                        size_t slen = strlen(s);
                        if (slen >= 2) {
                            char *inner = arena_strdup(&cpp->arena, s + 1,
                                                       slen - 2);
                            TokList pl = lex_buffer(cpp, inner, strlen(inner),
                                                    str->loc.filename);
                            do_pragma(cpp, pl, str->loc);
                        }
                        t = n->next; /* skip past ) */
                        continue;
                    }
                }
            }
        }
        toklist_append(&out, t);
        t = next;
    }
    return out;
}

/* Count net unmatched '(' in a token stream (ignoring ones inside strings) */
static int count_paren_depth(TokList toks) {
    int depth = 0;
    for (Token *t = toks.head; t; t = t->next) {
        if (t->kind == TOK_PUNCT) {
            if (strcmp(tok_text(t), "(") == 0) depth++;
            else if (strcmp(tok_text(t), ")") == 0) depth--;
        }
    }
    return depth;
}

/* Append all tokens from src onto dst (destructively unlinks from src) */
static void toklist_concat(TokList *dst, TokList src) {
    if (!src.head) return;
    /* insert a single TOK_SPACE to represent the line boundary */
    if (dst->tail && dst->tail->kind != TOK_SPACE) {
        /* we don't have an arena here; we reuse the src.head's prev space
           by noting that lex_one_line strips the newline, so just connect */
    }
    if (!dst->head) { *dst = src; return; }
    dst->tail->next = src.head;
    dst->tail       = src.tail;
}

/* Main preprocessor loop —
   reads and processes one logical line at a time from the include stack.
   Multi-line function-like macro calls are accumulated across lines. */
static void run_cpp(CPP *cpp) {
    while (cpp->includes.depth >= 0) {
        IncludeFrame *f = &cpp->includes.frames[cpp->includes.depth];

        bool hit_eof;
        TokList line = lex_one_line(cpp, f, &hit_eof);

        /* Only process non-empty lines or EOF */
        if (line.head || hit_eof) {
            /* Skip leading whitespace to find first real token */
            Token *first = line.head;
            while (first && first->kind == TOK_SPACE) first = first->next;

            if (first && first->kind == TOK_PUNCT &&
                strcmp(tok_text(first), "#") == 0) {
                /* ---- Directive line ---- */
                TokList dir_rest = {NULL, NULL};
                Token *t = first->next;
                /* skip one space after # */
                while (t && t->kind == TOK_SPACE) t = t->next;
                for (; t; t = t->next)
                    toklist_append(&dir_rest, tok_clone(&cpp->arena, t));
                SrcLoc loc = first->loc;
                process_directive(cpp, dir_rest, loc);
            } else if (is_active(cpp)) {
                /* ---- Ordinary code line: expand and emit ---- */
                if (first) {
                    /* Accumulate continuation lines for multi-line macro calls.
                       If open-paren depth > 0 at end of line, the macro call
                       continues on the next physical line. */
                    int paren_depth = count_paren_depth(line);
                    while (paren_depth > 0 && !hit_eof &&
                           cpp->includes.depth >= 0) {
                        f = &cpp->includes.frames[cpp->includes.depth];
                        bool eof2;
                        TokList cont = lex_one_line(cpp, f, &eof2);

                        /* skip any directives that appear mid-expression
                           (technically UB but be lenient) */
                        Token *cf = cont.head;
                        while (cf && cf->kind == TOK_SPACE) cf = cf->next;
                        if (cf && cf->kind == TOK_PUNCT &&
                            strcmp(tok_text(cf), "#") == 0) {
                            /* directive inside continuation: process it */
                            TokList dr = {NULL, NULL};
                            Token *dt = cf->next;
                            while (dt && dt->kind == TOK_SPACE)
                                dt = dt->next;
                            for (; dt; dt = dt->next)
                                toklist_append(&dr, tok_clone(&cpp->arena, dt));
                            process_directive(cpp, dr, cf->loc);
                        } else {
                            paren_depth += count_paren_depth(cont);
                            toklist_concat(&line, cont);
                        }
                        if (eof2) { hit_eof = true; break; }
                    }

                    TokList expanded = expand(cpp, line);
                    expanded = handle_pragma_operator(cpp, expanded);
                    emit_line_tokens(cpp, expanded,
                                     first->loc.line, first->loc.filename);
                } else {
                    /* blank line — keep structural whitespace */
                    fputc('\n', cpp->out);
                }
            }
        }

        if (hit_eof) {
            IncludeFrame *cf = &cpp->includes.frames[cpp->includes.depth];
            if (cpp->conds.depth > cf->cond_depth) {
                SrcLoc loc = {cf->logical_name, cf->line, 0};
                cpp_error(cpp, loc, "unterminated #if at end of file");
                cpp->conds.depth = cf->cond_depth;
            }
            pop_file(cpp);
        }
    }
}

/* =========================================================================
 * Public API
 * ====================================================================== */

static void define_builtin_str(CPP *cpp, const char *name, const char *val) {
    Macro *m = arena_alloc(&cpp->arena, sizeof(Macro));
    memset(m, 0, sizeof(Macro));
    m->name = intern_cstr(&cpp->strings, name);
    m->kind = MACRO_OBJECT;
    SrcLoc loc = {"<builtin>", 0, 0};
    Token *t = tok_new(&cpp->arena, TOK_PP_NUMBER, val, strlen(val), loc);
    toklist_append(&m->body, t);
    macro_insert(&cpp->macros, m);
}



void cpp_init(CPP *cpp, FILE *out) {
    memset(cpp, 0, sizeof(CPP));
    arena_init(&cpp->arena);
    intern_init(&cpp->strings);
    macro_table_init(&cpp->macros);
    guard_cache_init(&cpp->guards);
    cpp->includes.depth = -1;
    cpp->conds.depth    = -1;
    cpp->out            = out;
    cpp->last_file      = NULL;
    cpp->last_line      = 0;

    /* Freeze date/time */
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    static const char *months[] = {
        "Jan","Feb","Mar","Apr","May","Jun",
        "Jul","Aug","Sep","Oct","Nov","Dec"
    };
    snprintf(cpp->date_str, sizeof(cpp->date_str),
             "\"%s %2d %04d\"",
             months[tm->tm_mon], tm->tm_mday, tm->tm_year + 1900);
    snprintf(cpp->time_str, sizeof(cpp->time_str),
             "\"%02d:%02d:%02d\"",
             tm->tm_hour, tm->tm_min, tm->tm_sec);

    /* Standard built-in macros */
    define_builtin_str(cpp, "__STDC__",            "1");
    define_builtin_str(cpp, "__STDC_HOSTED__",     "1");
    define_builtin_str(cpp, "__STDC_VERSION__",    "201710L");
    /* GNU compatibility — required for glibc.
     * Use the version of the host compiler so that glibc version guards
     * (e.g. __GNUC_PREREQ) evaluate identically to a real cpp run. */
    define_builtin_str(cpp, "__GNUC__",            JCPP_STRINGIFY(__GNUC__));
    define_builtin_str(cpp, "__GNUC_MINOR__",      JCPP_STRINGIFY(__GNUC_MINOR__));
    define_builtin_str(cpp, "__GNUC_PATCHLEVEL__", JCPP_STRINGIFY(__GNUC_PATCHLEVEL__));
    define_builtin_str(cpp, "__GNUC_STDC_INLINE__","1");
    define_builtin_str(cpp, "__NO_INLINE__",       "1");
    /* Architecture — assume x86-64 Linux */
    define_builtin_str(cpp, "__LP64__",            "1");
    define_builtin_str(cpp, "__x86_64__",          "1");
    define_builtin_str(cpp, "__x86_64",            "1");
    define_builtin_str(cpp, "__amd64__",           "1");
    define_builtin_str(cpp, "__amd64",             "1");
    define_builtin_str(cpp, "__linux__",           "1");
    define_builtin_str(cpp, "__linux",             "1");
    define_builtin_str(cpp, "linux",               "1");
    define_builtin_str(cpp, "__unix__",            "1");
    define_builtin_str(cpp, "__unix",              "1");
    define_builtin_str(cpp, "__ELF__",             "1");
    /* Char/int/pointer sizes for x86-64 */
    define_builtin_str(cpp, "__CHAR_BIT__",        "8");
    define_builtin_str(cpp, "__SIZEOF_INT__",      "4");
    define_builtin_str(cpp, "__SIZEOF_LONG__",     "8");
    define_builtin_str(cpp, "__SIZEOF_LONG_LONG__","8");
    define_builtin_str(cpp, "__SIZEOF_SHORT__",    "2");
    define_builtin_str(cpp, "__SIZEOF_POINTER__",  "8");
    define_builtin_str(cpp, "__SIZEOF_FLOAT__",    "4");
    define_builtin_str(cpp, "__SIZEOF_DOUBLE__",   "8");
    define_builtin_str(cpp, "__SIZEOF_SIZE_T__",   "8");
    define_builtin_str(cpp, "__SIZEOF_PTRDIFF_T__","8");
    define_builtin_str(cpp, "__SIZEOF_WCHAR_T__",  "4");
    define_builtin_str(cpp, "__SIZEOF_WINT_T__",   "4");
    define_builtin_str(cpp, "__SIZE_TYPE__",       "long unsigned int");
    define_builtin_str(cpp, "__PTRDIFF_TYPE__",    "long int");
    define_builtin_str(cpp, "__WCHAR_TYPE__",      "int");
    define_builtin_str(cpp, "__WINT_TYPE__",       "unsigned int");
    define_builtin_str(cpp, "__INTMAX_TYPE__",     "long int");
    define_builtin_str(cpp, "__UINTMAX_TYPE__",    "long unsigned int");
    define_builtin_str(cpp, "__INT8_TYPE__",       "signed char");
    define_builtin_str(cpp, "__INT16_TYPE__",      "short int");
    define_builtin_str(cpp, "__INT32_TYPE__",      "int");
    define_builtin_str(cpp, "__INT64_TYPE__",      "long int");
    define_builtin_str(cpp, "__UINT8_TYPE__",      "unsigned char");
    define_builtin_str(cpp, "__UINT16_TYPE__",     "short unsigned int");
    define_builtin_str(cpp, "__UINT32_TYPE__",     "unsigned int");
    define_builtin_str(cpp, "__UINT64_TYPE__",     "long unsigned int");
    define_builtin_str(cpp, "__INT_LEAST8_TYPE__", "signed char");
    define_builtin_str(cpp, "__INT_LEAST16_TYPE__","short int");
    define_builtin_str(cpp, "__INT_LEAST32_TYPE__","int");
    define_builtin_str(cpp, "__INT_LEAST64_TYPE__","long int");
    define_builtin_str(cpp, "__UINT_LEAST8_TYPE__","unsigned char");
    define_builtin_str(cpp, "__UINT_LEAST16_TYPE__","short unsigned int");
    define_builtin_str(cpp, "__UINT_LEAST32_TYPE__","unsigned int");
    define_builtin_str(cpp, "__UINT_LEAST64_TYPE__","long unsigned int");
    define_builtin_str(cpp, "__INT_FAST8_TYPE__",  "signed char");
    define_builtin_str(cpp, "__INT_FAST16_TYPE__", "long int");
    define_builtin_str(cpp, "__INT_FAST32_TYPE__", "long int");
    define_builtin_str(cpp, "__INT_FAST64_TYPE__", "long int");
    define_builtin_str(cpp, "__UINT_FAST8_TYPE__", "unsigned char");
    define_builtin_str(cpp, "__UINT_FAST16_TYPE__","long unsigned int");
    define_builtin_str(cpp, "__UINT_FAST32_TYPE__","long unsigned int");
    define_builtin_str(cpp, "__UINT_FAST64_TYPE__","long unsigned int");
    define_builtin_str(cpp, "__INTPTR_TYPE__",     "long int");
    define_builtin_str(cpp, "__UINTPTR_TYPE__",    "long unsigned int");
    define_builtin_str(cpp, "__INT_MAX__",         "2147483647");
    define_builtin_str(cpp, "__LONG_MAX__",        "9223372036854775807L");
    define_builtin_str(cpp, "__LONG_LONG_MAX__",   "9223372036854775807LL");
    define_builtin_str(cpp, "__SHRT_MAX__",        "32767");
    define_builtin_str(cpp, "__SCHAR_MAX__",       "127");
    define_builtin_str(cpp, "__UCHAR_MAX__",       "255");
    define_builtin_str(cpp, "__SIZE_MAX__",        "18446744073709551615UL");
    define_builtin_str(cpp, "__WORDSIZE",          "64");

    /* Default system include paths */
    cpp_add_include_path(cpp, "/usr/lib/gcc/x86_64-pc-linux-gnu/15.2.1/include", true);
    cpp_add_include_path(cpp, "/usr/lib/gcc/x86_64-linux-gnu/12/include",  true);
    cpp_add_include_path(cpp, "/usr/local/include",  true);
    cpp_add_include_path(cpp, "/usr/include/x86_64-linux-gnu", true);
    cpp_add_include_path(cpp, "/usr/include",        true);
}

void cpp_free(CPP *cpp) {
    intern_free(&cpp->strings);
    for (unsigned i = 0; i < cpp->macros.cap; i++) {
        /* macros are in arena, only free hash table */
        (void)cpp->macros.buckets[i];
    }
    free(cpp->macros.buckets);
    for (unsigned i = 0; i < cpp->guards.cap; i++) {
        GuardEntry *e = cpp->guards.buckets[i];
        while (e) { GuardEntry *n = e->next; free(e); e = n; }
    }
    free(cpp->guards.buckets);
    /* include frames — free any remaining buffers */
    for (int i = 0; i <= cpp->includes.depth; i++)
        free(cpp->includes.frames[i].buf);
    arena_free_all(&cpp->arena);
}

void cpp_add_include_path(CPP *cpp, const char *path, bool is_system) {
    const char *p = intern_cstr(&cpp->strings, path);
    if (is_system) {
        if (cpp->sys_path_count < MAX_SEARCH_PATHS)
            cpp->sys_paths[cpp->sys_path_count++] = p;
    } else {
        if (cpp->quote_path_count < MAX_SEARCH_PATHS)
            cpp->quote_paths[cpp->quote_path_count++] = p;
    }
}

void cpp_define(CPP *cpp, const char *def) {
    /* def is "NAME" or "NAME=body" */
    const char *eq = strchr(def, '=');
    const char *name;
    const char *body_str;
    if (!eq) {
        name = def;
        body_str = "1";
    } else {
        name = def; /* will be truncated below */
        body_str = eq + 1;
    }
    size_t nlen = eq ? (size_t)(eq - def) : strlen(def);
    char *ndup = malloc(nlen + 1);
    memcpy(ndup, name, nlen); ndup[nlen] = '\0';

    /* synthesise "#define NAME body\n" and parse it */
    size_t blen = strlen(body_str);
    char *def_line = malloc(nlen + blen + 10);
    snprintf(def_line, nlen + blen + 10, "%s %s\n", ndup, body_str);

    TokList tl = lex_buffer(cpp, def_line, strlen(def_line), "<cmdline>");
    SrcLoc loc = {"<cmdline>", 0, 0};
    parse_define(cpp, tl, loc);

    free(def_line);
    free(ndup);
}

void cpp_undef(CPP *cpp, const char *name) {
    const char *iname = intern_cstr(&cpp->strings, name);
    macro_remove(&cpp->macros, iname);
}

int cpp_process_file(CPP *cpp, const char *path) {
    if (!file_exists(path)) {
        fprintf(stderr, "jcpp: error: %s: %s\n", path, strerror(errno));
        return 1;
    }
    const char *rp = intern_cstr(&cpp->strings, path);
    push_file(cpp, rp, path, false);
    run_cpp(cpp);
    if (cpp->conds.depth >= 0)
        fprintf(stderr, "%s: warning: unterminated #if\n", path);
    return cpp->error_count > 0 ? 1 : 0;
}
