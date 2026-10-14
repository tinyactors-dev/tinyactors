---
title: Getting started
description: "From a download to a C program that evaluates JavaScript."
---

# Getting started

This page takes you from a download to a program that evaluates
JavaScript. It covers the three ways to build staticjs into a C program;
the browser is in [In the browser](./wasm.md).

## 1. Get a release

Releases are on the repository's GitHub releases page, tagged
`staticjs/v<version>`. Each one has these assets:

| Asset | What it holds | Use it to |
|---|---|---|
| `staticjs-<version>-src.tar.gz` | `staticjs.h`; `staticjs.c`, the whole library as one C file (the *amalgamation*); `staticjs_wasm.c`, the wasm module's allocator; `staticjs.api.json`, the API's types, constants and struct layouts | compile staticjs into your program with clang, on any target |
| `staticjs-<version>-<target>.tar.gz` for `x86_64-linux`, `aarch64-linux`, `x86_64-macos`, `aarch64-macos` | `include/staticjs.h`, `lib/libstaticjs.a`, and the shared library (`libstaticjs.so.<major>.<minor>` with a `libstaticjs.so` link, or `libstaticjs.<major>.<minor>.dylib`) | link a prebuilt library, with any C compiler |
| `staticjs-<version>-wasm32.tar.gz` | `staticjs.wasm` and `include/staticjs.h` | run staticjs in a browser or in Node ([In the browser](./wasm.md)) |
| `staticjs-<version>.json` | the manifest: the commit, the compiler, the flags, every file's hash | check what a release was built from |
| `SHA256SUMS` | each asset's SHA-256 | `sha256sum -c SHA256SUMS --ignore-missing` |

Every tarball carries `LICENSE` and the notices of the code staticjs ports
(`LICENSE.fdlibm`, `LICENSE.v8`); keep them with what you ship.

The libraries call no C library and export the `sjs_*` functions only. They
leave no symbol undefined, so one Linux library works with glibc and with
musl.

## 2. Write a program

`examples/hello.c` is the smallest whole program. It makes a context with an
allocator, compiles one expression, evaluates it on a budget of fuel and
prints the value:

```c
#include "staticjs.h"

#include <stdio.h>
#include <stdlib.h>

/* staticjs allocates only through the embedder. It asks for 16-byte
 * alignment, which malloc gives on 64-bit hosts; aligned_alloc covers any
 * larger alignment. */
static void *allocate(void *self, size_t size, size_t alignment) {
    (void)self;
    if (alignment <= _Alignof(max_align_t)) return malloc(size);
    return aligned_alloc(alignment, (size + alignment - 1) / alignment * alignment);
}

static void release(void *self, const sjs_allocator_free_params *params) {
    (void)self;
    free(params->ptr);
}

static const sjs_allocator_vtable heap = { allocate, release };

static const char SOURCE[] =
    "({ total: [1, 2, 3, 4, 5].map((n) => n * n).reduce((sum, n) => sum + n, 0),"
    "   names: [\"grace\", \"ada\"].toSorted() })";

int main(void) {
    sjs_context *context;
    sjs_context_config config = { .allocator = { NULL, &heap }, .limits = SJS_LIMITS_DEFAULT };
    if (sjs_context_init(&context, &config) != SJS_OK) return 1;

    /* Compile: a source held in memory, as one expression. */
    sjs_program *program;
    sjs_error error = { 0 };
    sjs_compile_params compile = {
        .compiler = { .context = context, .goal = SJS_GOAL_EXPRESSION },
        .source = SJS_STR(SOURCE),
        .error = &error,
    };
    if (sjs_compile(&compile, &program) != SJS_OK) {
        fprintf(stderr, "%u:%u: %.*s\n", error.at.line, error.at.column, (int)error.message.length,
                error.message.ptr);
        return 1;
    }

    /* Evaluate: fuel is pushed, and the evaluation stops where it runs out. */
    sjs_vm *vm;
    if (sjs_vm_init(&vm, &(sjs_vm_config){ .context = context }) != SJS_OK) return 1;
    if (sjs_vm_start(vm, &(sjs_run_params){ .program = program }) != SJS_OK) return 1;
    sjs_run status = sjs_vm_run(vm, 100000);
    if (status != SJS_RUN_DONE) {
        if (status == SJS_RUN_FAULT) sjs_vm_error(vm, &error);
        fprintf(stderr, "%s: %.*s\n", status == SJS_RUN_PAUSED ? "out of fuel" : "fault",
                (int)error.message.length, error.message.ptr);
        return 1;
    }
    sjs_value result;
    sjs_vm_take_result(vm, &result);

    /* Write the value as text, into a buffer of any size. */
    sjs_writer *writer;
    if (sjs_writer_init(&writer, &(sjs_writer_config){ .context = context, .value = result }) != SJS_OK) return 1;
    uint8_t bytes[64];
    sjs_result written;
    do {
        sjs_buffer buffer = { bytes, sizeof bytes, 0 };
        written = sjs_writer_write(writer, &buffer);
        fwrite(bytes, 1, buffer.length, stdout);
    } while (written == SJS_NEED_SPACE);
    putchar('\n');

    /* Everything made with the context goes before the context does. */
    sjs_writer_destroy(writer);
    sjs_release(context, result);
    sjs_vm_destroy(vm);
    sjs_program_release(program);
    sjs_context_destroy(context);
    return written == SJS_OK ? 0 : 1;
}
```

## 3. Build it

**With a prebuilt library** (any C11 compiler; here for x86_64 Linux):

```sh
tar xzf staticjs-0.9.0-x86_64-linux.tar.gz
cd staticjs-0.9.0-x86_64-linux
cc -std=c11 -Iinclude hello.c lib/libstaticjs.a -o hello                        # static
cc -std=c11 -Iinclude hello.c -Llib -lstaticjs -Wl,-rpath,"$PWD/lib" -o hello   # shared
./hello
```

**From the amalgamation** (clang only):

```sh
tar xzf staticjs-0.9.0-src.tar.gz
cd staticjs-0.9.0-src
clang -std=c11 -O2 -I. hello.c staticjs.c -o hello
./hello
```

It prints:

```
{"total":55,"names":["ada","grace"]}
```

The amalgamation needs clang: it computes square roots with clang's
`__builtin_elementwise_sqrt`, which GCC does not have, so GCC fails to link
it. It builds with clang 18 and clang 22; the release's libraries are built
with `zig cc` (LLVM 21). With GCC, link a prebuilt library. Compile the amalgamation with
`-O2`. It is freestanding: it includes nothing but the compiler's
`stdint.h`, `stddef.h` and `stdbool.h`.

**From this repository**, `mise run dist` in `staticjs/` writes the header,
the amalgamation, the API description and `libstaticjs.a` for this machine
to `staticjs/dist/`.

## 4. Try programs

`examples/eval.c` is a command-line evaluator. Build it as you built
`hello.c`, and use it to try what staticjs accepts:

```sh
$ ./eval '[3, 1, 2].toSorted((a, b) => a - b)'
[1,2,3]
$ ./eval --module 'export const total = [1, 2, 3].reduce((a, b) => a + b, 0);'
{"total":6}
$ ./eval '({ a: 1 }).b.c'
fault at 1:14: cannot read a property of undefined (c)
$ ./eval '[1, 2] === [1, 2]'
fault at 1:8: === on two objects compares identity, which staticjs does not have
$ ./eval '() => { while (true) {} }'
unsupported at 1:9: staticjs has no loops: use map, filter, reduce or some
```

Or try them here, without building anything:

```js live
[3, 1, 2].toSorted((a, b) => a - b)
```


A fault is staticjs declining to give a value where JavaScript would throw,
or would coerce, compare identities or look on a prototype. It never gives
a different value. [The language](./language.md) says what a program can say,
and [Faults and refusals](./faults.md) where staticjs stops.

## Next

- [Embedding](./embedding.md): the API. Contexts, limits and fuel; values
  in and out; globals, native functions and modules; the clock and the
  random seed; streaming.
- [In the browser](./wasm.md): staticjs in a browser or in Node.
- [../../examples/](./examples.md): whole programs for each of
  these.
