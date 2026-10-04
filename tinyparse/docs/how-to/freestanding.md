# Run tinyparse without libc

This guide is for a developer who targets bare metal, a kernel, a sandbox or
another environment without a hosted C library. It shows what the library
needs from the target, how to build it there, and how to give it memory you
control.

## Prerequisites

- A source tree with `tinyparse` built (`mise run build`), the shell in its
  `tinyparse/` directory; see [Install tinyparse](install.md).
- A C11 compiler for your target that provides the freestanding headers
  `<stddef.h>` and `<stdint.h>`. Clang and GCC provide both. The commands
  below use `clang`.
- `nm` from binutils or LLVM, to read the objects.
- A scratch directory with the amalgamation in it:

  ```sh
  mise run amalgamate
  WORK="$(mktemp -d)"
  cp dist/amalgamation/tinyparse.c dist/amalgamation/tinyparse.h "$WORK"
  ```

## Build the library for your target

Compile `tinyparse.c` as freestanding code, and keep the compiler from
replacing loops with calls to C library functions:

```sh
clang -std=c11 -O2 -ffreestanding -fno-builtin -c "$WORK/tinyparse.c" -o "$WORK/tinyparse.o"
```

For another target, add its triple, such as `--target=aarch64-none-elf`; the
library has no code that depends on the target. Clang 22 also compiles it for
`riscv32-none-elf` and `wasm32`, which no part of the tinyparse build tests.

## Provide the memory functions

The library calls no allocator, does no I/O and reads no clock. The only
symbols it leaves undefined are the memory functions the compiler emits calls
to for copying and zeroing structures:

```sh
nm -u "$WORK/tinyparse.o"
```

```text
                 U memcpy
                 U memset
```

The release build accepts `memmove` and `memcmp` as well, and on macOS clang
emits `bzero` in place of `memset`; [Library
symbols](../reference/artifacts.md#library-symbols) lists what each build
leaves undefined. If your target has no C library, define the functions the
object needs. This `$WORK/memory.c` is enough for the object above; compile it with
`-fno-builtin` too, so the compiler does not turn its loops back into calls to
themselves:

```c
#include <stddef.h>

void *memcpy(void *destination, const void *source, size_t count)
{
    unsigned char *to = destination;
    const unsigned char *from = source;

    while (count-- > 0)
        *to++ = *from++;
    return destination;
}

void *memset(void *destination, int value, size_t count)
{
    unsigned char *to = destination;

    while (count-- > 0)
        *to++ = (unsigned char)value;
    return destination;
}
```

```sh
clang -std=c11 -O2 -ffreestanding -fno-builtin -c "$WORK/memory.c" -o "$WORK/memory.o"
```

### Link a program with no C library

This program for x86-64 Linux uses no C library at all: its entry point is its
own, and it reports its result through the exit status, the number of
children of the root node. Save it as `$WORK/bare.c`:

```c
/* bare.c: parse a fixed input with no C library, on x86-64 Linux.
   The exit status is the number of children of the root node, or 255
   when a call fails. */
#include "tinyparse.h"

static unsigned char grant[256 * 1024] __attribute__((aligned(16)));

static const char grammar_text[] =
    "(grammar words start: Words"
    "  (token Space (regex \"[ ]+\"))"
    "  (token Word (regex \"[a-z]+\"))"
    "  (extras (trivia Space))"
    "  (rule Words (seq (repeat Word) EOF)))";

static const char input_text[] = "one two three";

int bare_main(void)
{
    tinyparse_configuration configuration = { { grant, sizeof grant, 0, 0, 0 }, 0 };
    tinyparse_source source = { { 0, 0 }, { (const uint8_t *)grammar_text, sizeof grammar_text - 1 } };
    tinyparse_string no_start = { 0, 0 };
    tinyparse_string input = { (const uint8_t *)input_text, sizeof input_text - 1 };
    tinyparse_engine *engine;
    tinyparse_grammar *grammar = 0;
    tinyparse_parser *parser = 0;
    tinyparse_tree_cursor_storage cursor;
    int status = 255;

    engine = tinyparse_open(configuration);
    if (engine == 0)
        return 255;
    if (tinyparse_compile_scheme(engine, source, &grammar) == TINYPARSE_OK &&
        tinyparse_parser_new(engine, grammar, no_start, &parser) == TINYPARSE_OK &&
        tinyparse_push(parser, input) == TINYPARSE_OK &&
        tinyparse_finish(parser) == TINYPARSE_OK) {
        tinyparse_tree_cursor_init(&cursor, tinyparse_parser_tree(parser));
        status = (int)tinyparse_tree_cursor_child_count((tinyparse_tree_cursor *)&cursor);
    }
    if (parser != 0)
        tinyparse_parser_free(parser);
    if (grammar != 0)
        tinyparse_grammar_free(grammar);
    tinyparse_close(engine);
    return status;
}

/* The entry point: align the stack, call bare_main, exit with its result. */
__asm__(".globl _start\n"
        "_start:\n"
        "  xor %ebp, %ebp\n"
        "  and $-16, %rsp\n"
        "  call bare_main\n"
        "  mov %eax, %edi\n"
        "  mov $60, %eax\n"
        "  syscall\n");
```

```sh
clang -std=c11 -O2 -ffreestanding -fno-builtin -Wall -Wextra -c "$WORK/bare.c" -o "$WORK/bare.o"
clang -static -nostdlib "$WORK/bare.o" "$WORK/tinyparse.o" "$WORK/memory.o" -o "$WORK/bare"
"$WORK/bare"
echo $?
```

The root `Words` has five children, three `Word` nodes and two `Space` nodes:

```text
5
```

## Choose a memory grant

The engine allocates everything from the grant the host passes to
`tinyparse_open`. The [Memory grant](../reference/c-api.md#memory-grant)
reference specifies each field; the choice is between two shapes:

| Grant | Set | Use it when |
|-------|-----|-------------|
| Fixed | `base` and `size`; `grow` is `NULL` | The memory is a static array or a region you reserve once. A call that needs more fails with `TINYPARSE_OUT_OF_MEMORY`, or with `TINYPARSE_INVALID` and an "out of memory" fault while compiling or loading a grammar. |
| Growable | `base`, `size`, and a `grow` callback | The work varies, as when the input size is not known. The engine calls `grow` when the current block cannot hold its next allocation. |

To use a growable grant:

1. Write `grow` so that it returns a block of at least `need` bytes, aligned
   to 16 bytes, or `NULL` to refuse.
2. Record every block `grow` returns. The engine never resizes, moves or
   frees one, and `tinyparse_close` gives none back.
3. After `tinyparse_close`, free `base` and every recorded block.

[Memory grant](../reference/c-api.md#memory-grant) states the full contract,
including how a larger block reports its size through `*got`.

`chunk_minimum` is the smallest piece the engine takes from a block, 4,096
bytes when it is 0. The first block holds 128 bytes of engine bookkeeping and
one piece of that size, so a smaller `chunk_minimum` lets `tinyparse_open`
accept a smaller first block. On x86-64 Linux the smallest first block it
accepts is 4,224 bytes with the default and 640 bytes with a `chunk_minimum`
of 512.

## Size the grant

[Sizing a grant](../reference/c-api.md#sizing-a-grant) gives the smallest
fixed grant for each standard grammar, compiled and loaded, and the memory a
parse needs per input byte. To measure your own grammar and input, give the
engine a small first block and a `grow` callback that counts. This hosted
program, `$WORK/measure.c`, compiles a grammar, parses a file, prints the totals and
frees every block:

```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tinyparse.h"

#define FIRST_BLOCK (8u * 1024u)
#define MAX_BLOCKS 4096

/* Every block the engine received, so the host can free them after
   tinyparse_close, and the bytes handed out so far. */
typedef struct grant_record {
    void *blocks[MAX_BLOCKS];
    size_t block_count;
    size_t total;
} grant_record;

static void *grow(void *user, size_t need, size_t *got)
{
    grant_record *record = user;
    void *block;

    if (record->block_count == MAX_BLOCKS)
        return NULL;
    block = malloc(need);
    if (block == NULL)
        return NULL;
    record->blocks[record->block_count++] = block;
    record->total += need;
    *got = need;
    return block;
}

static unsigned char *read_file(const char *path, size_t *out_length)
{
    FILE *file = fopen(path, "rb");
    unsigned char *bytes = NULL;
    long length;

    if (file == NULL)
        return NULL;
    if (fseek(file, 0, SEEK_END) == 0 && (length = ftell(file)) > 0 &&
        fseek(file, 0, SEEK_SET) == 0 && (bytes = malloc((size_t)length)) != NULL &&
        fread(bytes, 1, (size_t)length, file) != (size_t)length) {
        free(bytes);
        bytes = NULL;
    }
    fclose(file);
    *out_length = bytes != NULL ? (size_t)length : 0;
    return bytes;
}

int main(int argc, char **argv)
{
    static grant_record record;
    tinyparse_configuration configuration;
    tinyparse_engine *engine;
    tinyparse_grammar *grammar = NULL;
    tinyparse_parser *parser = NULL;
    tinyparse_source source;
    tinyparse_string no_start = { NULL, 0 };
    tinyparse_string input;
    unsigned char *grammar_text = NULL, *input_text = NULL;
    size_t grammar_length, input_length, i;
    size_t after_compile, compile_blocks;
    int status = 1;

    if (argc != 3) {
        fprintf(stderr, "usage: %s GRAMMAR.scm INPUT\n", argv[0]);
        return 2;
    }
    grammar_text = read_file(argv[1], &grammar_length);
    input_text = read_file(argv[2], &input_length);
    if (grammar_text == NULL || input_text == NULL) {
        fputs("cannot read the grammar or the input\n", stderr);
        goto release;
    }

    memset(&configuration, 0, sizeof configuration);
    configuration.memory.base = grow(&record, FIRST_BLOCK, &i);
    configuration.memory.size = FIRST_BLOCK;
    configuration.memory.grow = grow;
    configuration.memory.user = &record;
    if (configuration.memory.base == NULL)
        goto release;
    engine = tinyparse_open(configuration);
    if (engine == NULL) {
        fputs("tinyparse_open failed\n", stderr);
        goto release;
    }

    source.name.pointer = (const uint8_t *)argv[1];
    source.name.length = (uint32_t)strlen(argv[1]);
    source.bytes.pointer = grammar_text;
    source.bytes.length = (uint32_t)grammar_length;
    if (tinyparse_compile_scheme(engine, source, &grammar) != TINYPARSE_OK) {
        tinyparse_string fault = tinyparse_fault(engine);
        fprintf(stderr, "compile: %.*s\n", (int)fault.length, (const char *)fault.pointer);
        goto close;
    }
    after_compile = record.total;
    compile_blocks = record.block_count;

    input.pointer = input_text;
    input.length = (uint32_t)input_length;
    if (tinyparse_parser_new(engine, grammar, no_start, &parser) != TINYPARSE_OK ||
        tinyparse_push(parser, input) != TINYPARSE_OK ||
        tinyparse_finish(parser) != TINYPARSE_OK) {
        fputs("parse failed\n", stderr);
        goto close;
    }
    printf("compile: %zu bytes in %zu blocks\n", after_compile, compile_blocks);
    printf("compile + parse of %zu input bytes: %zu bytes in %zu blocks\n",
           input_length, record.total, record.block_count);
    status = 0;

close:
    if (parser != NULL)
        tinyparse_parser_free(parser);
    if (grammar != NULL)
        tinyparse_grammar_free(grammar);
    tinyparse_close(engine);
release:
    /* The engine never frees a block: the host frees them all. */
    for (i = 0; i < record.block_count; i++)
        free(record.blocks[i]);
    free(grammar_text);
    free(input_text);
    return status;
}
```

```sh
clang -std=c99 -Wall -Wextra -I include "$WORK/measure.c" build/lib/libtinyparse.a -o "$WORK/measure"
"$WORK/measure" examples/sexpr/sexpr.scm examples/bufferfriendly/sample.sx
```

The grammar took 20,576 bytes and the 190-byte input 40,704 more:

```text
compile: 20576 bytes in 4 blocks
compile + parse of 190 input bytes: 61280 bytes in 12 blocks
```

The totals count whole blocks, so they include the unused end of each block.
A fixed grant of the measured total, 61,280 bytes, held the same work; one of
49,152 bytes did not.

## Run it on several threads

Separate engines run on separate threads; nothing made from one engine may be
used from two threads at once. [Threading](../reference/c-api.md#threading)
states the rules.
