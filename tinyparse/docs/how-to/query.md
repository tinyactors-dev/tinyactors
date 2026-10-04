# Find nodes with a query

This guide is for a user who has a grammar and wants parts of a parsed file
back: every key of a JSON object, the value of one key, every string in an
array. It writes the query against the tree, runs it with `tinyparse query`,
and then runs the same query from a C program with `tinyparse_query`. The
syntax of every pattern is in the [Query language](../reference/query-language.md)
reference.

## Prerequisites

- A source checkout with the shell in its `tinyparse/` directory, built and on
  your `PATH`:

  ```sh
  mise run build
  export PATH="$PWD/build/bin:$PATH"
  ```

  For a release, see [Install tinyparse](install.md).
- A C99 compiler, for the C section.
- A scratch directory and an input file. The examples query this one-line JSON
  document with the standard grammar std.json:

  ```sh
  WORK="$(mktemp -d)"
  printf '{"name": "tinyparse", "tags": ["c", "parser"], "stars": 12}\n' > "$WORK/package.json"
  ```

## Find the node kinds and fields

A query names node kinds and fields, so start from the tree:

```sh
tinyparse run std.json --input "$WORK/package.json" --dump-tree
```

Each line is a node: its field, if it has one, its kind, its byte range and its
start and end points:

```text
Document [0-60] [0,0]-[1,0]
  Value [0-59] [0,0]-[0,59]
    Object [0-59] [0,0]-[0,59]
      open: { [0-1] [0,0]-[0,1]
      Member [1-20] [0,1]-[0,20]
        key: String [1-7] [0,1]-[0,7]
        : [7-8] [0,7]-[0,8]
        Space [8-9] [0,8]-[0,9]
        value: Value [9-20] [0,9]-[0,20]
          String [9-20] [0,9]-[0,20]
      , [20-21] [0,20]-[0,21]
      Space [21-22] [0,21]-[0,22]
      Member [22-45] [0,22]-[0,45]
        key: String [22-28] [0,22]-[0,28]
        : [28-29] [0,28]-[0,29]
        Space [29-30] [0,29]-[0,30]
        value: Value [30-45] [0,30]-[0,45]
          Array [30-45] [0,30]-[0,45]
            open: [ [30-31] [0,30]-[0,31]
            Value [31-34] [0,31]-[0,34]
              String [31-34] [0,31]-[0,34]
            , [34-35] [0,34]-[0,35]
            Space [35-36] [0,35]-[0,36]
            Value [36-44] [0,36]-[0,44]
              String [36-44] [0,36]-[0,44]
            close: ] [44-45] [0,44]-[0,45]
      , [45-46] [0,45]-[0,46]
      Space [46-47] [0,46]-[0,47]
      Member [47-58] [0,47]-[0,58]
        key: String [47-54] [0,47]-[0,54]
        : [54-55] [0,54]-[0,55]
        Space [55-56] [0,55]-[0,56]
        value: Value [56-58] [0,56]-[0,58]
          Number [56-58] [0,56]-[0,58]
      close: } [58-59] [0,58]-[0,59]
  Space [59-60] [0,59]-[1,0]
```

A key is a `String` in the field `key` of a `Member`. `Space` nodes are
siblings like any other node, which matters for the
[anchor](../reference/query-language.md#anchor). `tinyparse node-types std.json`
lists every kind and field the grammar can produce.

## Capture a node inside its parent

A capture `@NAME` follows a child pattern, so the pattern starts at the parent.
Save the query in a file:

```sh
printf '%s\n' '(Member key: (String) @key)' > "$WORK/keys.scm"
tinyparse query std.json "$WORK/keys.scm" --input "$WORK/package.json"
```

Each line is a capture name, a byte range and the captured bytes:

```text
@key 1-7 "name"
@key 22-28 "tags"
@key 47-54 "stars"
```

`(String) @key` on its own is rejected with `tinyparse: error: invalid query`
and exit status 1, because the capture has no parent pattern. To capture a node
of any kind, use `_`: `(Member key: (_) @key)`.

## Keep only some matches

A predicate tests the captured bytes. Wrap the pattern and the predicate in one
more list. `#eq?` compares the whole capture, including the quotes of a JSON
string:

```sh
printf '%s\n' '((Member key: (String) @key value: (_) @value) (#eq? @key "\"tags\""))' > "$WORK/tags.scm"
tinyparse query std.json "$WORK/tags.scm" --input "$WORK/package.json"
```

Only the member whose key is `"tags"` matches, and both its captures are
reported:

```text
@key 22-28 "tags"
@value 30-45 ["c", "parser"]
```

`#match?` holds when a [regex](../reference/grammar.md#regex-dialect) matches
anywhere in the capture. The dialect has no anchors, so `(#match? @key "s\"")`
selects the keys that contain `s"`, here `"tags"` and `"stars"`.

## Report each of several children

A pattern matches at most once at each node, and each child pattern takes the
first sibling it matches, so `(Array (Value) @item)` reports `"c"` and not
`"parser"`. Start the pattern at the repeated node instead:

```sh
printf '%s\n' '(Value (String) @text)' > "$WORK/strings.scm"
tinyparse query std.json "$WORK/strings.scm" --input "$WORK/package.json"
```

Every `String` inside a `Value` is reported: the value of `"name"` and both
items of the array. A key is not inside a `Value`, so no key is reported:

```text
@text 9-20 "tinyparse"
@text 31-34 "c"
@text 36-44 "parser"
```

No pattern reports every item of an `Array` and nothing else: started at the
`Array` it matches once, and started at the item it cannot name the item's
parent. To keep only the array items, keep the captures that lie inside the
array's byte range, 30 to 45 here, which
`(Member value: (Value (Array) @array))` captures.

## Run the query from C

`tinyparse_query` takes a parsed tree, the query text and the input bytes, and
calls a function once per capture. This program compiles a grammar, parses a
file, runs a query from a file and prints each capture. Save it as
`$WORK/query.c`:

```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tinyparse.h"

#define GRANT_SIZE (4u * 1024u * 1024u)

static tinyparse_string bytes(const char *pointer, size_t length)
{
    tinyparse_string out;
    out.pointer = (const uint8_t *)pointer;
    out.length = (uint32_t)length;
    return out;
}

static char *read_file(const char *path, size_t *length)
{
    FILE *file = fopen(path, "rb");
    char *data = NULL;
    long size;

    if (file == NULL)
        return NULL;
    if (fseek(file, 0, SEEK_END) == 0 && (size = ftell(file)) >= 0 &&
        fseek(file, 0, SEEK_SET) == 0) {
        data = malloc((size_t)size + 1);
        if (data != NULL && fread(data, 1, (size_t)size, file) != (size_t)size) {
            free(data);
            data = NULL;
        }
        *length = (size_t)size;
    }
    fclose(file);
    return data;
}

static void print_fault(tinyparse_engine *engine, const char *step)
{
    tinyparse_string fault = tinyparse_fault(engine);
    fprintf(stderr, "%s: %.*s\n", step, (int)fault.length,
            (const char *)fault.pointer);
}

static void print_capture(void *user, const tinyparse_query_capture *capture)
{
    (void)user;
    printf("%.*s [%u-%u] %.*s\n", (int)capture->name.length,
           (const char *)capture->name.pointer, (unsigned)capture->start_byte,
           (unsigned)capture->end_byte, (int)capture->text.length,
           (const char *)capture->text.pointer);
}

int main(int argc, char **argv)
{
    tinyparse_configuration configuration;
    tinyparse_engine *engine;
    tinyparse_grammar *grammar = NULL;
    tinyparse_parser *parser = NULL;
    tinyparse_source source;
    tinyparse_result result;
    char *grammar_text = NULL, *input = NULL, *query = NULL;
    size_t grammar_length = 0, input_length = 0, query_length = 0;
    void *grant;
    int status = 1;

    if (argc != 4) {
        fprintf(stderr, "usage: %s GRAMMAR INPUT QUERY\n", argv[0]);
        return 2;
    }
    grammar_text = read_file(argv[1], &grammar_length);
    input = read_file(argv[2], &input_length);
    query = read_file(argv[3], &query_length);
    if (grammar_text == NULL || input == NULL || query == NULL) {
        fprintf(stderr, "cannot read an argument file\n");
        goto free_files;
    }

    grant = malloc(GRANT_SIZE);
    if (grant == NULL)
        goto free_files;
    memset(&configuration, 0, sizeof configuration);
    configuration.memory.base = grant;
    configuration.memory.size = GRANT_SIZE;
    configuration.memory.grow = NULL;
    engine = tinyparse_open(configuration);
    if (engine == NULL) {
        fprintf(stderr, "tinyparse_open: the grant is too small\n");
        free(grant);
        goto free_files;
    }

    source.name = bytes(argv[1], strlen(argv[1]));
    source.bytes = bytes(grammar_text, grammar_length);
    result = tinyparse_compile_scheme(engine, source, &grammar);
    if (result != TINYPARSE_OK) {
        print_fault(engine, "compile");
        goto close_engine;
    }
    result = tinyparse_parser_new(engine, grammar, bytes(NULL, 0), &parser);
    if (result != TINYPARSE_OK) {
        print_fault(engine, "parser");
        goto free_grammar;
    }
    result = tinyparse_push(parser, bytes(input, input_length));
    if (result == TINYPARSE_OK)
        result = tinyparse_finish(parser);
    if (result != TINYPARSE_OK) {
        fprintf(stderr, "the input does not parse\n");
        goto free_parser;
    }

    result = tinyparse_query(engine, tinyparse_parser_tree(parser),
                             bytes(query, query_length),
                             bytes(input, input_length), print_capture, NULL);
    if (result == TINYPARSE_INVALID)
        fprintf(stderr, "invalid query\n");
    else if (result != TINYPARSE_OK)
        print_fault(engine, "query");
    else
        status = 0;

free_parser:
    tinyparse_parser_free(parser);
free_grammar:
    tinyparse_grammar_free(grammar);
close_engine:
    tinyparse_close(engine);
    free(grant);
free_files:
    free(grammar_text);
    free(input);
    free(query);
    return status;
}
```

- `capture->text` points into the `input` argument, so the input buffer must
  outlive the call.
- A rejected query returns `TINYPARSE_INVALID` and sets no fault, so the
  program prints its own message.
- The query allocates from the engine's memory grant.
  [Sizing a grant](../reference/c-api.md#sizing-a-grant) covers how large to
  make it.

Compile it against the header and the static library:

```sh
cc -std=c99 -Wall -Wextra -pedantic -Werror -I include "$WORK/query.c" build/lib/libtinyparse.a -o "$WORK/query"
```

The library has no copy of the standard grammars, so pass std.json's source
from the source tree. It extends and injects nothing, so the program needs no
module resolver:

```sh
"$WORK/query" grammars/std/json/grammar.scm "$WORK/package.json" "$WORK/keys.scm"
```

The captures are the ones `tinyparse query` printed, in this program's format:

```text
key [1-7] "name"
key [22-28] "tags"
key [47-54] "stars"
```

For a grammar that uses `import`, `extends` or `inject`, load a packed grammar
or install a resolver, as
[Split, extend and combine grammars](compose-grammars.md) describes.

## Fix `invalid query`

Neither `tinyparse query` nor `tinyparse_query` says which form was rejected or
why. A rejected form stops the query, after the captures of the forms before it
have been printed. Check the form against these causes:

| Query | Cause | Write instead |
|-------|-------|---------------|
| `(String) @key` | A capture at the top level | `(Member key: (String) @key)` |
| `(_ @any)`, `(Member @m)` | A capture after the head of a pattern | `(Member (_) @any)` |
| `(Member key: String @key)` | A child that is a bare kind name | `(Member key: (String) @key)` |
| `(Pair key: (String) @key)` | A kind or field the grammar does not define | a kind from `--dump-tree` |
| `((Member (String) @k) (#not-eq? @k "x"))` | A predicate other than `#eq?` and `#match?` | `#eq?` or `#match?` |

The [Errors](../reference/query-language.md#errors) section of the reference
lists every rejected shape.
