#include "cpp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s [options] <file.c>\n"
        "Options:\n"
        "  -I<dir>       Add directory to include search path\n"
        "  -isystem<dir> Add directory to system include search path\n"
        "  -D<name>[=val]  Define macro\n"
        "  -U<name>      Undefine macro\n"
        "  -o <file>     Write output to file (default: stdout)\n"
        "  --help        Show this message\n",
        prog);
}

int main(int argc, char **argv) {
    const char *input_file  = NULL;
    const char *output_file = NULL;

    /* We collect -D/-U/-I in order so they are applied in order */
    typedef struct Opt { char kind; const char *val; } Opt;
    Opt *opts = malloc(sizeof(Opt) * (size_t)(argc + 1));
    int  nopt = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            usage(argv[0]); free(opts); return 0;
        }
        if (strncmp(a, "-I", 2) == 0) {
            const char *path = a + 2;
            if (!path[0] && i + 1 < argc) path = argv[++i];
            opts[nopt++] = (Opt){ 'I', path };
        } else if (strncmp(a, "-isystem", 8) == 0) {
            const char *path = a + 8;
            if (!path[0] && i + 1 < argc) path = argv[++i];
            opts[nopt++] = (Opt){ 'S', path };
        } else if (strncmp(a, "-D", 2) == 0) {
            const char *def = a + 2;
            if (!def[0] && i + 1 < argc) def = argv[++i];
            opts[nopt++] = (Opt){ 'D', def };
        } else if (strncmp(a, "-U", 2) == 0) {
            const char *name = a + 2;
            if (!name[0] && i + 1 < argc) name = argv[++i];
            opts[nopt++] = (Opt){ 'U', name };
        } else if (strcmp(a, "-o") == 0) {
            if (i + 1 >= argc) { fprintf(stderr, "-o requires an argument\n"); return 1; }
            output_file = argv[++i];
        } else if (a[0] == '-') {
            /* silently ignore unknown flags (e.g. -std=, -w, -M, etc.) */
        } else {
            if (input_file) {
                fprintf(stderr, "jcpp: multiple input files not supported\n");
                free(opts);
                return 1;
            }
            input_file = a;
        }
    }

    if (!input_file) {
        usage(argv[0]);
        free(opts);
        return 1;
    }

    FILE *out = stdout;
    if (output_file) {
        out = fopen(output_file, "w");
        if (!out) {
            perror(output_file);
            free(opts);
            return 1;
        }
    }

    CPP cpp;
    cpp_init(&cpp, out);

    /* Apply options in order */
    for (int i = 0; i < nopt; i++) {
        switch (opts[i].kind) {
            case 'I': cpp_add_include_path(&cpp, opts[i].val, false); break;
            case 'S': cpp_add_include_path(&cpp, opts[i].val, true);  break;
            case 'D': cpp_define(&cpp, opts[i].val);                  break;
            case 'U': cpp_undef(&cpp, opts[i].val);                   break;
        }
    }

    int rc = cpp_process_file(&cpp, input_file);

    cpp_free(&cpp);

    if (output_file) fclose(out);
    free(opts);
    return rc;
}