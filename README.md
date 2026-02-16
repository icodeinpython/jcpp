# jcpp

A small C preprocessor with basic macro expansion, includes, and conditional compilation.

## Features

- Object-like and function-like macros
- `#include` with quoted and angle-bracket forms
- `#if`, `#ifdef`, `#ifndef`, `#elif`, `#else`, `#endif`
- `defined(...)` in `#if`
- `__FILE__` and `__LINE__`
- `#` stringize and `##` token concatenation

## Build

```sh
make
```

## Usage

```sh
./jcpp path/to/input.c > output.i
```

## Tests

```sh
make test
```

Test cases live in `tests/cases` with expected outputs in `tests/expected`.