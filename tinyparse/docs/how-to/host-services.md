# Supply imports, classes and parameters from a C host

This guide is for a host author whose grammar works with the `tinyparse`
command and fails in their own program: a compile that stops at
`extends requires a module resolver`, a `(class NAME)` that never matches, or a
parameter that has no effect. The command provides these services to every
grammar it compiles; a host program has none of them until it installs them.

## Prerequisites

- A source tree built with `mise run build`, the shell in its `tinyparse/`
  directory, and `tinyparse` on your `PATH`
  (`export PATH="$PWD/build/bin:$PATH"`); for a release, see
  [Install tinyparse](install.md).
- A C99 compiler.
- A scratch directory for the files below:

  ```sh
  WORK="$(mktemp -d)"
  ```

- The program `host.c`, listed in full under The complete program below,
  saved as `$WORK/host.c` and built:

  ```sh
  cc -std=c99 -Wall -Wextra -I include "$WORK/host.c" build/lib/libtinyparse.a -o "$WORK/host"
  ```

`host` takes `[--param NAME[=VALUE]]... [--language NAME=GRAMMAR]... GRAMMAR
INPUT`. It compiles a `.scm` grammar or loads a `.tpg` packed grammar, parses
`INPUT`, prints each diagnostic and then the tree, and exits with status 0 when
the parse produced no diagnostic. The sections below show the part of it that
supplies each service.

## What the command provides

| Service | What `tinyparse` does | What a host gets without it | C API |
|---------|-----------------------|-----------------------------|-------|
| Module resolver | Maps `NAME` to the file `NAME`, with each `.` replaced by `/` and `.scm` appended, in the directory of the grammar on the command line; then to the standard grammar of that name built into the command. | The compile fails with `import requires a module resolver`, `extends requires a module resolver` or `inject requires a module resolver to find the embedded grammar`. | [`tinyparse_set_module_resolver`](../reference/c-api.md#tinyparse_set_module_resolver) |
| Character classes | Binds `alpha`, `digit`, `alnum` and `space` ([Classes](../reference/grammar.md#classes)). | `(class NAME)` matches no byte; the parse reports `parse.syntax` where the class was needed. | [`tinyparse_bind`](../reference/c-api.md#tinyparse_bind) |
| Parameters | Sets the `params:` of a grammar package's `package.scm`, then each `--param`. | Every parameter is unset, so `(when NAME: X)` leaves `X` out of the language. | [`tinyparse_set_param`](../reference/c-api.md#tinyparse_set_param) |
| Memory | A 16 MiB grant that grows with `malloc`. | A fixed grant too small for the grammar fails with an "out of memory" fault. | [`tinyparse_memory`](../reference/c-api.md#memory-grant) |

The project `tinyparse init DIR --grammar NAME` seeds has none of these
services and a fixed 8 MiB grant. Its program fails for std.jsonc, std.json5,
std.typescript and std.tsx with `extends requires a module resolver`, for
std.fence with `inject requires a module resolver to find the embedded grammar`,
and for std.javascript with `out of memory reading string`.

## Resolve import, extends and inject names

A grammar names other grammars in `(import NAME)`, `(extends NAME)` and
`(inject grammar: NAME ...)` ([Composition](../reference/grammar.md#composition)).
The library does no I/O, so it asks the engine's module resolver for the text
of each name while `tinyparse_compile_scheme` runs. This resolver follows the
command's rule, and serves `std.NAME` from `grammars/std/NAME/grammar.scm` in
place of the command's built-in copies (fragment):

```c
struct resolver {
    struct blocks *blocks;
    char directory[1024]; /* the root grammar's directory, ending in '/' */
};

static tinyparse_result resolve(void *user, tinyparse_string name,
                                tinyparse_source *out_source)
{
    struct resolver *resolver = user;
    char path[2048];
    size_t used, i, length = 0;
    char *bytes;

    used = (size_t)snprintf(path, sizeof path, "%s", resolver->directory);
    for (i = 0; i < name.length && used + 5 < sizeof path; i++)
        path[used++] = name.pointer[i] == '.' ? '/' : (char)name.pointer[i];
    memcpy(path + used, ".scm", 5);
    bytes = read_file(resolver->blocks, path, &length);

    if (bytes == NULL && name.length > 4 && memcmp(name.pointer, "std.", 4) == 0) {
        snprintf(path, sizeof path, STD_DIRECTORY "%.*s/grammar.scm",
                 (int)name.length - 4, (const char *)name.pointer + 4);
        bytes = read_file(resolver->blocks, path, &length);
    }
    if (bytes == NULL)
        return TINYPARSE_INVALID;
    out_source->bytes = text(bytes, length);
    return TINYPARSE_OK;
}
```

Install it before compiling (fragment):

```c
    result = tinyparse_set_module_resolver(engine, resolve, &resolver);
```

`examples/import/main.scm` imports `words`, which the resolver finds beside it:

```sh
"$WORK/host" examples/import/main.scm examples/import/fixtures/sample.words
```

The `Word` and `Space` nodes come from `words.scm`:

```text
Source [0-17]
  Word [0-5]
  Space [5-6]
  Word [6-11]
  Space [11-12]
  Word [12-17]
```

The compiled grammar keeps pointers into the bytes the resolver returns, so
`host` frees every file buffer after its last call on the engine. The result
contract and cycle handling are in the
[tinyparse_set_module_resolver](../reference/c-api.md#tinyparse_set_module_resolver)
entry.

## Bind character classes

A `(class NAME)` form matches one byte that the host's class function for
`NAME` accepts. This grammar reads names that start with a byte of the class
`xid-start`, which the command does not bind. Save it as `$WORK/names.scm`:

```scheme
(grammar names
  start: Source

  (token Space (regex "[ \t\r\n]+"))
  (token Name
    (seq (class xid-start) (repeat (choice (class xid-start) (class digit)))))

  (extras (trivia Space))

  (rule Source (seq (repeat Name) EOF)))
```

Write an input with non-ASCII names:

```sh
printf 'größe _tmp2 名前\n' > "$WORK/names.txt"
```

The command compiles the grammar and then fails on the first byte, because
`xid-start` has no binding:

```sh
tinyparse run "$WORK/names.scm" --input "$WORK/names.txt"
```

```text
error: unexpected input [parse.syntax]
```

`host` binds the command's four classes and `xid-start`, here a function that
accepts ASCII letters, `_` and every byte of a multi-byte UTF-8 sequence. Each
binding is one call (fragment):

```c
static tinyparse_result bind_class(tinyparse_engine *engine, const char *name,
                                   tinyparse_class_fn function)
{
    tinyparse_binding binding;
    binding.name = text(name, strlen(name));
    binding.kind = TINYPARSE_BIND_CLASS;
    binding.function = (void *)function;
    binding.user = NULL;
    return tinyparse_bind(engine, &binding);
}
```

With the binding, the input parses:

```sh
"$WORK/host" "$WORK/names.scm" "$WORK/names.txt"
```

```text
Source [0-21]
  Name [0-7]
  Space [7-8]
  Name [8-13]
  Space [13-14]
  Name [14-20]
  Space [20-21]
```

The class function returns nonzero for a byte in the class; its other rules
(purity, byte range, when to bind) are in the
[tinyparse_bind](../reference/c-api.md#tinyparse_bind) entry.

## Set parameters

`(when NAME: X)` makes `X` part of the language only when parameter `NAME` is
set. The command sets the parameters a grammar package's `package.scm` lists;
the library does not read `package.scm`. std.tsx is such a package: its
manifest sets `types` and `jsx`
([std.tsx](../reference/std-grammars.md#stdtsx)). Write a TSX file:

```sh
printf 'const a = <b>{x}</b>;\n' > "$WORK/app.tsx"
```

Compiled with no parameter, the grammar leaves out JSX:

```sh
"$WORK/host" grammars/std/tsx/grammar.scm "$WORK/app.tsx"
```

```text
error: unexpected input [parse.syntax]
```

`host` sets each `--param` with `tinyparse_set_param` before it compiles
(fragment):

```c
static tinyparse_result set_param(tinyparse_engine *engine, const char *spec)
{
    const char *equals = strchr(spec, '=');
    size_t length = equals != NULL ? (size_t)(equals - spec) : strlen(spec);
    int value = equals == NULL || strcmp(equals + 1, "0") != 0;
    return tinyparse_set_param(engine, text(spec, length), value);
}
```

With the manifest's parameters, the JSX element parses:

```sh
"$WORK/host" --param types --param jsx grammars/std/tsx/grammar.scm "$WORK/app.tsx"
```

The first 11 of its 32 lines follow; the rest of the element and of the
statement is left out:

```text
program [0-22]
  lexical_declaration [0-21]
    kind: const [0-5]
    Space [5-6]
    variable_declarator [6-20]
      name: binding_target [6-7]
        identifier [6-7]
      Space [7-8]
      = [8-9]
      Space [9-10]
      value: jsx_element [10-20]
...
```

Parameters are read while `tinyparse_compile_scheme` runs, for the grammar and
every grammar it imports or extends. A parameter set after the compile changes
nothing until the next compile. To compile a grammar package, set the
parameters its `params:` field sets; the
[package.scm manifest](../reference/grammar.md#packagescm-manifest) table says
which values set a parameter and which clear it.

## Parse injected regions

`(inject grammar: NAME content: REGION)` parses `REGION` with another grammar.
For a grammar compiled from `.scm`, the module resolver supplies that grammar
during the compile, and nothing else is needed. The std.fence grammar injects
std.json into `json` fences. Write a fence whose content is JSON5:

````sh
printf '```json\n{retries: 3,}\n```\n' > "$WORK/notes.md"
````

The std.json grammar does not parse it. The region's parse fails without a diagnostic and
leaves an empty `Document` under the `injection` node:

```sh
"$WORK/host" grammars/std/fence/grammar.scm "$WORK/notes.md"
```

````text
Document [0-26]
  Block [0-25]
    JsonFence [0-25]
      ```json [0-7]
      \n [7-8]
      injection [8-21]
        Document [8-21]
      \n [21-22]
      ``` [22-25]
  Space [25-26]
````

[`tinyparse_register_language`](../reference/c-api.md#tinyparse_register_language)
replaces the grammar an inject form names, for parses started afterward and
with no recompile. `host --language NAME=GRAMMAR` compiles `GRAMMAR` and
registers it under `NAME`, with `parse` set to `NULL` so the VM parses the
region (fragment):

```c
static tinyparse_result register_language(tinyparse_engine *engine,
                                          struct blocks *blocks,
                                          const char *spec)
{
    const char *equals = strchr(spec, '=');
    tinyparse_language_entry entry;
    tinyparse_result result;

    if (equals == NULL)
        return TINYPARSE_INVALID;
    result = load_grammar(engine, blocks, equals + 1, &entry.grammar);
    if (result != TINYPARSE_OK)
        return result;
    entry.parse = NULL; /* parse the region with the VM */
    return tinyparse_register_language(engine,
                                       text(spec, (size_t)(equals - spec)),
                                       &entry);
}
```

Register std.json5 under the name `std.json`:

```sh
"$WORK/host" --language std.json=grammars/std/json5/grammar.scm grammars/std/fence/grammar.scm "$WORK/notes.md"
```

With std.json5 registered, the region holds its object:

````text
Document [0-26]
  Block [0-25]
    JsonFence [0-25]
      ```json [0-7]
      \n [7-8]
      injection [8-21]
        Document [8-21]
          Value [8-21]
            Object [8-21]
              open: { [8-9]
              Member [9-19]
                key: Identifier [9-16]
                : [16-17]
                Space [17-18]
                value: Value [18-19]
                  Number [18-19]
              , [19-20]
              close: } [20-21]
      \n [21-22]
      ``` [22-25]
  Space [25-26]
````

To parse a region with a generated parser, set `entry.grammar` to the grammar
its `P_load` returned and `entry.parse` to its `P_run`;
`examples/nesting/nesting.c` registers two that way, and [Ship a generated C
parser](generated-parser.md) builds them.

## Use a standard grammar

The standard grammars are built into the `tinyparse` command; the library has
no copy of them. A host gets one in either of two ways.

Pack it with the command and load the packed file. The packed grammar already
holds its parameters, its base grammars and the grammars it injects, so loading
it calls no resolver and needs no parameter:

```sh
tinyparse pack std.jsonc --out "$WORK/jsonc.tpg"
printf '// retries\n{"retries": 3}\n' > "$WORK/config.jsonc"
"$WORK/host" "$WORK/jsonc.tpg" "$WORK/config.jsonc"
```

```text
Document [0-26]
  Comment [0-10]
  Space [10-11]
  Value [11-25]
    Object [11-25]
      open: { [11-12]
      Member [12-24]
        key: String [12-21]
        : [21-22]
        Space [22-23]
        value: Value [23-24]
          Number [23-24]
      close: } [24-25]
  Space [25-26]
```

Or compile its `grammar.scm` from the source tree, with a resolver that finds
the grammars it extends or injects and the parameters its `package.scm` sets.
`host` resolves `std.json` for std.jsonc's `(extends std.json)` from
`grammars/std/json/grammar.scm`:

```sh
"$WORK/host" grammars/std/jsonc/grammar.scm "$WORK/config.jsonc"
```

The tree is the same as from the packed file.

Loading packed std.javascript takes 430 KiB of the grant; compiling its
`grammar.scm` takes 12,811 KiB ([Sizing a grant](../reference/c-api.md#sizing-a-grant)).
A host that ships a standard grammar loads a `.tpg`, or compiles in a generated
module ([Ship a grammar inside your program](embed-grammar.md)).

## The complete program

`host.c`:

```c
/* host.c: parse a file with the services the tinyparse command provides. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tinyparse.h"

#define GRANT_SIZE (1024u * 1024u)
#define MAX_BLOCKS 64
#define STD_DIRECTORY "grammars/std/"

/* Every block the program allocates: grow blocks and file buffers. A compiled
** grammar keeps pointers into its source bytes, so they are freed last. */
struct blocks {
    void *pointer[MAX_BLOCKS];
    size_t count;
};

static void *keep(struct blocks *blocks, void *pointer)
{
    if (pointer == NULL || blocks->count == MAX_BLOCKS) {
        free(pointer);
        return NULL;
    }
    blocks->pointer[blocks->count++] = pointer;
    return pointer;
}

static void free_blocks(struct blocks *blocks)
{
    while (blocks->count > 0)
        free(blocks->pointer[--blocks->count]);
}

/* Hand out at least GRANT_SIZE bytes per block, so the engine asks rarely. */
static void *grow(void *user, size_t need, size_t *got)
{
    size_t size = need > GRANT_SIZE ? need : GRANT_SIZE;
    *got = size;
    return keep(user, malloc(size));
}

static tinyparse_string text(const char *pointer, size_t length)
{
    tinyparse_string out;
    out.pointer = (const uint8_t *)pointer;
    out.length = (uint32_t)length;
    return out;
}

static char *read_file(struct blocks *blocks, const char *path, size_t *length)
{
    FILE *file = fopen(path, "rb");
    char *buffer = NULL;
    long size;

    if (file == NULL)
        return NULL;
    if (fseek(file, 0, SEEK_END) == 0 && (size = ftell(file)) >= 0) {
        rewind(file);
        buffer = keep(blocks, malloc((size_t)size + 1));
        if (buffer != NULL)
            *length = fread(buffer, 1, (size_t)size, file);
    }
    fclose(file);
    return buffer;
}

static void print_fault(const char *what, tinyparse_engine *engine)
{
    tinyparse_string fault = tinyparse_fault(engine);
    fprintf(stderr, "%s: %.*s\n", what, (int)fault.length,
            (const char *)fault.pointer);
}

/* The module resolver: NAME a.b is <directory>a/b.scm, then
** grammars/std/b/grammar.scm when NAME is std.b. */
struct resolver {
    struct blocks *blocks;
    char directory[1024]; /* the root grammar's directory, ending in '/' */
};

static tinyparse_result resolve(void *user, tinyparse_string name,
                                tinyparse_source *out_source)
{
    struct resolver *resolver = user;
    char path[2048];
    size_t used, i, length = 0;
    char *bytes;

    used = (size_t)snprintf(path, sizeof path, "%s", resolver->directory);
    for (i = 0; i < name.length && used + 5 < sizeof path; i++)
        path[used++] = name.pointer[i] == '.' ? '/' : (char)name.pointer[i];
    memcpy(path + used, ".scm", 5);
    bytes = read_file(resolver->blocks, path, &length);

    if (bytes == NULL && name.length > 4 && memcmp(name.pointer, "std.", 4) == 0) {
        snprintf(path, sizeof path, STD_DIRECTORY "%.*s/grammar.scm",
                 (int)name.length - 4, (const char *)name.pointer + 4);
        bytes = read_file(resolver->blocks, path, &length);
    }
    if (bytes == NULL)
        return TINYPARSE_INVALID;
    out_source->bytes = text(bytes, length);
    return TINYPARSE_OK;
}

/* Character classes: the four the tinyparse command binds, and xid-start. */
static int class_alpha(void *user, uint32_t byte)
{
    (void)user;
    return (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z');
}

static int class_digit(void *user, uint32_t byte)
{
    (void)user;
    return byte >= '0' && byte <= '9';
}

static int class_alnum(void *user, uint32_t byte)
{
    return class_alpha(user, byte) || class_digit(user, byte);
}

static int class_space(void *user, uint32_t byte)
{
    (void)user;
    return byte == ' ' || byte == '\t' || byte == '\n' || byte == '\r';
}

/* ASCII letters, '_', and every byte of a multi-byte UTF-8 sequence. */
static int class_xid_start(void *user, uint32_t byte)
{
    return class_alpha(user, byte) || byte == '_' || byte >= 0x80;
}

static tinyparse_result bind_class(tinyparse_engine *engine, const char *name,
                                   tinyparse_class_fn function)
{
    tinyparse_binding binding;
    binding.name = text(name, strlen(name));
    binding.kind = TINYPARSE_BIND_CLASS;
    binding.function = (void *)function;
    binding.user = NULL;
    return tinyparse_bind(engine, &binding);
}

static tinyparse_result bind_classes(tinyparse_engine *engine)
{
    tinyparse_result result = bind_class(engine, "alpha", class_alpha);
    if (result == TINYPARSE_OK)
        result = bind_class(engine, "digit", class_digit);
    if (result == TINYPARSE_OK)
        result = bind_class(engine, "alnum", class_alnum);
    if (result == TINYPARSE_OK)
        result = bind_class(engine, "space", class_space);
    if (result == TINYPARSE_OK)
        result = bind_class(engine, "xid-start", class_xid_start);
    return result;
}

/* Set a parameter from NAME (set), NAME=0 (clear) or NAME=VALUE (set). */
static tinyparse_result set_param(tinyparse_engine *engine, const char *spec)
{
    const char *equals = strchr(spec, '=');
    size_t length = equals != NULL ? (size_t)(equals - spec) : strlen(spec);
    int value = equals == NULL || strcmp(equals + 1, "0") != 0;
    return tinyparse_set_param(engine, text(spec, length), value);
}

/* Compile a .scm grammar, or load a .tpg packed grammar. */
static tinyparse_result load_grammar(tinyparse_engine *engine,
                                     struct blocks *blocks, const char *path,
                                     tinyparse_grammar **out_grammar)
{
    tinyparse_source source;
    size_t length = 0, path_length = strlen(path);
    char *bytes = read_file(blocks, path, &length);

    if (bytes == NULL) {
        fprintf(stderr, "cannot read %s\n", path);
        return TINYPARSE_INVALID;
    }
    source.name = text(path, path_length);
    source.bytes = text(bytes, length);
    if (path_length > 4 && strcmp(path + path_length - 4, ".tpg") == 0)
        return tinyparse_load_binary_grammar(engine, source, out_grammar);
    return tinyparse_compile_scheme(engine, source, out_grammar);
}

/* Register GRAMMAR as the language NAME, from NAME=GRAMMAR. */
static tinyparse_result register_language(tinyparse_engine *engine,
                                          struct blocks *blocks,
                                          const char *spec)
{
    const char *equals = strchr(spec, '=');
    tinyparse_language_entry entry;
    tinyparse_result result;

    if (equals == NULL)
        return TINYPARSE_INVALID;
    result = load_grammar(engine, blocks, equals + 1, &entry.grammar);
    if (result != TINYPARSE_OK)
        return result;
    entry.parse = NULL; /* parse the region with the VM */
    return tinyparse_register_language(engine,
                                       text(spec, (size_t)(equals - spec)),
                                       &entry);
}

/* Print a node kind or field name, with a line feed or tab as \n or \t. */
static void print_name(tinyparse_string name)
{
    uint32_t i;
    for (i = 0; i < name.length; i++) {
        if (name.pointer[i] == '\n')
            fputs("\\n", stdout);
        else if (name.pointer[i] == '\t')
            fputs("\\t", stdout);
        else
            putchar(name.pointer[i]);
    }
}

static void print_node(const tinyparse_tree *tree, tinyparse_tree_cursor *cursor)
{
    tinyparse_string kind =
        tinyparse_tree_symbol_name(tree, tinyparse_tree_cursor_kind(cursor));
    tinyparse_symbol field = tinyparse_tree_cursor_field(cursor);

    printf("%*s", (int)(2 * tinyparse_tree_cursor_depth(cursor)), "");
    if (tinyparse_tree_cursor_flags(cursor) & TINYPARSE_NODE_MISSING)
        printf("MISSING ");
    if (field.id != TINYPARSE_NO_FIELD) {
        print_name(tinyparse_tree_symbol_name(tree, field));
        printf(": ");
    }
    print_name(kind);
    printf(" [%u-%u]\n", (unsigned)tinyparse_tree_cursor_start_byte(cursor),
           (unsigned)tinyparse_tree_cursor_end_byte(cursor));
    if (tinyparse_tree_cursor_goto_first_child(cursor)) {
        do
            print_node(tree, cursor);
        while (tinyparse_tree_cursor_goto_next_sibling(cursor));
        tinyparse_tree_cursor_goto_parent(cursor);
    }
}

int main(int argc, char **argv)
{
    struct blocks blocks;
    struct resolver resolver;
    tinyparse_configuration configuration;
    tinyparse_engine *engine;
    tinyparse_grammar *grammar = NULL;
    tinyparse_parser *parser = NULL;
    tinyparse_tree_cursor *cursor = NULL;
    const tinyparse_tree *tree;
    tinyparse_result result;
    const char *grammar_path, *input_path, *slash;
    char *input;
    size_t input_length = 0;
    int i, status = 1;

    blocks.count = 0;
    if (argc < 3) {
        fprintf(stderr, "usage: %s [--param NAME[=VALUE]]... "
                "[--language NAME=GRAMMAR]... GRAMMAR INPUT\n", argv[0]);
        return 2;
    }
    grammar_path = argv[argc - 2];
    input_path = argv[argc - 1];

    memset(&configuration, 0, sizeof configuration);
    configuration.memory.base = keep(&blocks, malloc(GRANT_SIZE));
    configuration.memory.size = GRANT_SIZE;
    configuration.memory.grow = grow;
    configuration.memory.user = &blocks;
    engine = tinyparse_open(configuration);
    if (engine == NULL) {
        fprintf(stderr, "tinyparse_open failed\n");
        free_blocks(&blocks);
        return 1;
    }

    /* Resolve names relative to the directory of the grammar file. */
    resolver.blocks = &blocks;
    slash = strrchr(grammar_path, '/');
    snprintf(resolver.directory, sizeof resolver.directory, "%.*s",
             slash != NULL ? (int)(slash - grammar_path + 1) : 0, grammar_path);
    result = tinyparse_set_module_resolver(engine, resolve, &resolver);

    /* Classes, parameters and languages, before compiling. */
    if (result == TINYPARSE_OK)
        result = bind_classes(engine);
    for (i = 1; result == TINYPARSE_OK && i < argc - 2; i += 2) {
        if (strcmp(argv[i], "--param") == 0)
            result = set_param(engine, argv[i + 1]);
        else if (strcmp(argv[i], "--language") == 0)
            result = register_language(engine, &blocks, argv[i + 1]);
        else
            result = TINYPARSE_INVALID;
    }
    if (result != TINYPARSE_OK) {
        print_fault("setup", engine);
        goto done;
    }

    result = load_grammar(engine, &blocks, grammar_path, &grammar);
    if (result != TINYPARSE_OK) {
        print_fault("compile", engine);
        goto done;
    }
    input = read_file(&blocks, input_path, &input_length);
    if (input == NULL) {
        fprintf(stderr, "cannot read %s\n", input_path);
        goto done;
    }
    result = tinyparse_parser_new(engine, grammar, text(NULL, 0), &parser);
    if (result != TINYPARSE_OK) {
        print_fault("parser", engine);
        goto done;
    }
    result = tinyparse_push(parser, text(input, input_length));
    if (result == TINYPARSE_OK)
        result = tinyparse_finish(parser);

    for (i = 0; i < (int)tinyparse_parser_diagnostic_count(parser); i++) {
        const tinyparse_diagnostic *diagnostic =
            tinyparse_parser_diagnostic(parser, (uint32_t)i);
        printf("%.*s [%.*s]\n", (int)diagnostic->message.length,
               (const char *)diagnostic->message.pointer,
               (int)diagnostic->code.length,
               (const char *)diagnostic->code.pointer);
    }
    tree = tinyparse_parser_tree(parser);
    if (result == TINYPARSE_OK && tree != NULL &&
        tinyparse_tree_cursor_new(tree, &cursor) == TINYPARSE_OK) {
        print_node(tree, cursor);
        tinyparse_tree_cursor_free(cursor);
        status = tinyparse_parser_diagnostic_count(parser) == 0 ? 0 : 1;
    }
    tinyparse_parser_free(parser);

done:
    tinyparse_grammar_free(grammar);
    tinyparse_close(engine);
    free_blocks(&blocks);
    return status;
}
```
