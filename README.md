# jcpp

A C preprocessor written in C, compatible with ISO C17 and GNU/glibc extensions.

## Features

- `#include` with `"..."` and `<...>` forms, plus `#include_next`
- Object-like and function-like macro expansion (`#define` / `#undef`)
- Variadic macros (`__VA_ARGS__`, named variadic parameters, GNU `##__VA_ARGS__` comma-swallowing)
- Token stringification (`#`) and token pasting (`##`)
- Conditional compilation (`#if`, `#ifdef`, `#ifndef`, `#elif`, `#else`, `#endif`) with full constant-expression evaluation
- Built-in macros: `__FILE__`, `__LINE__`, `__DATE__`, `__TIME__`, `__COUNTER__`, `__STDC__`, `__STDC_VERSION__`, etc.
- GNU version macros (`__GNUC__`, `__GNUC_MINOR__`, `__GNUC_PATCHLEVEL__`) set to the host compiler's version at build time
- `#pragma once` and traditional include-guard detection
- `#pragma GCC system_header`
- GCC-compatible linemarker output (`# <line> "<file>" <flags>`)
- `-I`, `-isystem`, `-D`, `-U` command-line options
- Arena allocator and string interning for fast processing

## Building

```sh
make
```

The binary is output to `./jcpp`.

## Usage

```
jcpp [options] <file.c>

Options:
  -I<dir>           Add directory to #include "..." search path
  -isystem<dir>     Add directory to #include <...> search path
  -D<name>[=val]    Define macro
  -U<name>          Undefine macro
  -o <file>         Write output to file (default: stdout)
  --help            Show this message
```

### Example

```sh
# Preprocess a file and write to file.i
./jcpp -I./include -DDEBUG=1 src/main.c -o main.i

# Then compile the preprocessed output with GCC
gcc main.i -S -o main.s
```

## Project Structure

```
jcpp/
├── src/
│   ├── jcpp.c      # Preprocessor core (lexer, macro engine, directives, emitter)
│   └── main.c      # CLI entry point
├── include/
│   └── cpp.h       # Public API and data-structure definitions
├── Makefile
└── README.md
```

## Licence

GPL
