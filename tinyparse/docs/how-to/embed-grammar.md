# Ship a grammar inside your program

This guide is for a host author whose program compiles a `.scm` grammar at run
time and who wants to ship the program without grammar text. It shows how to
load a packed grammar from a file and how to compile the grammar into the
program as a generated module.

## Prerequisites

- `tinyparse` on `PATH` and a C99 compiler. In a source tree, `mise run build`
  then `export PATH="$PWD/build/bin:$PATH"`; see [Install
  tinyparse](install.md). The commands run in the `tinyparse/` directory and
  link `build/lib/libtinyparse.a`; with an installed library, use its
  `include/` and `lib/` directories instead.
- A grammar that compiles with `tinyparse check`. The steps use
  `examples/sexpr/sexpr.scm`.
- A scratch directory:

  ```sh
  WORK="$(mktemp -d)"
  ```

## Choose a form

| Form | Compiled by | The program reads at run time | The program links | Grammar memory for std.javascript |
|------|---------|-------------------------------|-------------------|----------------------------------:|
| Grammar text | the program, at each start | the `.scm` file and every file it imports | the library | 12,811 KiB to compile |
| Packed grammar | `tinyparse pack` | the `.tpg` file | the library | 430 KiB to load |
| Generated module | `tinyparse c` | nothing | the library and the module | 430 KiB to load |
| Generated parser | `tinyparse c --parser-lib` | nothing | `libtinyparse.a` or the amalgamation, and the parser | 430 KiB to load |

The memory column is the smallest fixed grant that held the grammar, from
[Sizing a grant](../reference/c-api.md#sizing-a-grant). A packed grammar and a
generated module both hold the composed grammar: imports, the base grammar of
`extends`, the grammars `inject` names, and the parameters set when it was
made. Loading one calls no module resolver and reads no parameter.

A generated module runs in the library's VM, like grammar text. A generated
parser replaces the VM with one C function per rule and needs a source tree to
build; [Ship a generated C parser](generated-parser.md) covers it. Why the
forms differ is in [Design of tinyparse](../explanation/design.md).

## Load a packed grammar

1. Pack the grammar. Set its parameters here with `--param NAME[=VALUE]`;
   they cannot change after packing. A standard grammar is packed by its name,
   such as `std.jsonc`:

   ```sh
   tinyparse pack examples/sexpr/sexpr.scm --out "$WORK/sexpr.tpg"
   ```

2. Load the image with `tinyparse_load_binary_grammar` in place of
   `tinyparse_compile_scheme`. The grammar copies the image, so the program
   frees the file's bytes after the call. Save this program as
   `$WORK/tpg_host.c`; it takes the packed grammar and an input file:

   ```c
   #include <stdio.h>
   #include <stdlib.h>
   #include <string.h>

   #include "tinyparse.h"

   #define GRANT_SIZE (16u * 1024u * 1024u)

   /* Read a whole file into a malloc'd buffer. */
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
       tinyparse_configuration configuration;
       tinyparse_engine *engine = NULL;
       tinyparse_grammar *grammar = NULL;
       tinyparse_parser *parser = NULL;
       tinyparse_source image;
       tinyparse_string no_start = { NULL, 0 };
       tinyparse_string input;
       tinyparse_result result;
       unsigned char *packed = NULL, *text = NULL;
       size_t packed_length, text_length;
       void *grant = NULL;
       int status = 1;

       if (argc != 3) {
           fprintf(stderr, "usage: %s GRAMMAR.tpg INPUT\n", argv[0]);
           return 2;
       }
       packed = read_file(argv[1], &packed_length);
       text = read_file(argv[2], &text_length);
       grant = malloc(GRANT_SIZE);
       if (packed == NULL || text == NULL || grant == NULL) {
           fputs("cannot read the files or allocate the grant\n", stderr);
           goto release;
       }
       memset(&configuration, 0, sizeof configuration);
       configuration.memory.base = grant;
       configuration.memory.size = GRANT_SIZE;
       engine = tinyparse_open(configuration);
       if (engine == NULL) {
           fputs("tinyparse_open failed\n", stderr);
           goto release;
       }

       image.name.pointer = NULL;
       image.name.length = 0;
       image.bytes.pointer = packed;
       image.bytes.length = (uint32_t)packed_length;
       if (tinyparse_load_binary_grammar(engine, image, &grammar) != TINYPARSE_OK) {
           tinyparse_string fault = tinyparse_fault(engine);
           fprintf(stderr, "load: %.*s\n", (int)fault.length, (const char *)fault.pointer);
           goto close;
       }
       free(packed); /* the grammar holds its own copy of the image */
       packed = NULL;

       result = tinyparse_parser_new(engine, grammar, no_start, &parser);
       if (result != TINYPARSE_OK) {
           fprintf(stderr, "tinyparse_parser_new: result %d\n", (int)result);
           goto close;
       }
       input.pointer = text;
       input.length = (uint32_t)text_length;
       result = tinyparse_push(parser, input);
       if (result == TINYPARSE_OK)
           result = tinyparse_finish(parser);
       printf("result %d, %u diagnostics\n", (int)result,
              (unsigned)tinyparse_parser_diagnostic_count(parser));
       status = result == TINYPARSE_OK ? 0 : 1;

   close:
       if (parser != NULL)
           tinyparse_parser_free(parser);
       if (grammar != NULL)
           tinyparse_grammar_free(grammar);
       tinyparse_close(engine);
   release:
       free(grant);
       free(packed);
       free(text);
       return status;
   }
   ```

3. Build it and parse a file:

   ```sh
   cc -std=c99 -Wall -Wextra -I include "$WORK/tpg_host.c" build/lib/libtinyparse.a -o "$WORK/tpg_host"
   "$WORK/tpg_host" "$WORK/sexpr.tpg" examples/bufferfriendly/sample.sx
   ```

   ```text
   result 0, 0 diagnostics
   ```

Ship the `.tpg` with the program, and pack it again with each new tinyparse
version. A library that does not match the one that packed the image refuses
it with `TINYPARSE_INVALID` and a fault such as `packed grammar ABI mismatch`;
[Packed grammars](../reference/artifacts.md#packed-grammars) lists the checks.

## Compile the grammar into the program

1. Generate the module. It holds the packed grammar as a byte array and
   defines `tinyparse_string tinyparse_embedded_grammar(void)`, which returns
   it:

   ```sh
   tinyparse c examples/sexpr/sexpr.scm --out "$WORK/sexpr_grammar.c"
   ```

2. Pass what `tinyparse_embedded_grammar` returns to
   `tinyparse_load_binary_grammar`. The module declares no header, so the
   program declares the function. Save this program as
   `$WORK/module_host.c`:

   ```c
   #include <stdio.h>
   #include <stdlib.h>
   #include <string.h>

   #include "tinyparse.h"

   /* Defined in the file `tinyparse c` writes. */
   tinyparse_string tinyparse_embedded_grammar(void);

   #define GRANT_SIZE (1024u * 1024u)

   int main(void)
   {
       static const char text[] = "(define x 1)";
       tinyparse_configuration configuration;
       tinyparse_engine *engine;
       tinyparse_grammar *grammar = NULL;
       tinyparse_parser *parser = NULL;
       tinyparse_source image;
       tinyparse_string no_start = { NULL, 0 };
       tinyparse_string input;
       tinyparse_result result;
       void *grant = malloc(GRANT_SIZE);
       int status = 1;

       if (grant == NULL)
           return 1;
       memset(&configuration, 0, sizeof configuration);
       configuration.memory.base = grant;
       configuration.memory.size = GRANT_SIZE;
       engine = tinyparse_open(configuration);
       if (engine == NULL) {
           fputs("tinyparse_open failed\n", stderr);
           free(grant);
           return 1;
       }

       image.name.pointer = NULL;
       image.name.length = 0;
       image.bytes = tinyparse_embedded_grammar();
       if (tinyparse_load_binary_grammar(engine, image, &grammar) != TINYPARSE_OK) {
           tinyparse_string fault = tinyparse_fault(engine);
           fprintf(stderr, "load: %.*s\n", (int)fault.length, (const char *)fault.pointer);
           goto done;
       }
       result = tinyparse_parser_new(engine, grammar, no_start, &parser);
       if (result != TINYPARSE_OK) {
           fprintf(stderr, "tinyparse_parser_new failed: %d\n", (int)result);
           goto done;
       }
       input.pointer = (const uint8_t *)text;
       input.length = (uint32_t)strlen(text);
       result = tinyparse_push(parser, input);
       if (result == TINYPARSE_OK)
           result = tinyparse_finish(parser);
       if (result != TINYPARSE_OK) {
           fprintf(stderr, "parse failed: %d\n", (int)result);
           goto done;
       }
       printf("parsed %u bytes, %u diagnostics\n", (unsigned)input.length,
              (unsigned)tinyparse_parser_diagnostic_count(parser));
       status = 0;

   done:
       if (parser != NULL)
           tinyparse_parser_free(parser);
       if (grammar != NULL)
           tinyparse_grammar_free(grammar);
       tinyparse_close(engine);
       free(grant);
       return status;
   }
   ```

3. Build the program with the module and run it:

   ```sh
   cc -std=c99 -Wall -Wextra -I include "$WORK/module_host.c" "$WORK/sexpr_grammar.c" build/lib/libtinyparse.a -o "$WORK/module_host"
   "$WORK/module_host"
   ```

   ```text
   parsed 12 bytes, 0 diagnostics
   ```

The module includes only `tinyparse.h`, so it builds against an installed
library, the shared one included, and compiles as C99.

### Put two modules in one program

Every module defines the same name, `tinyparse_embedded_grammar`, and
`--name` does not change it, so the linker rejects two modules in one program
with a multiple definition of `tinyparse_embedded_grammar`. Rename the function
when you compile each module:

```sh
tinyparse c std.json --out "$WORK/json_grammar.c"
cc -std=c99 -I include -Dtinyparse_embedded_grammar=json_grammar -c "$WORK/json_grammar.c" -o "$WORK/json_grammar.o"
cc -std=c99 -I include -Dtinyparse_embedded_grammar=sexpr_grammar -c "$WORK/sexpr_grammar.c" -o "$WORK/sexpr_grammar.o"
```

The program then declares and calls `json_grammar()` and `sexpr_grammar()`.

## Check a grammar with a bundle

`tinyparse c --bundle` writes a module with a `main` that parses standard input
with the VM and prints one node kind per line, without fields or byte ranges.
It is a program to try a grammar with, and cannot be linked into a host that
has its own `main`:

```sh
tinyparse c examples/sexpr/sexpr.scm --bundle --out "$WORK/sexpr_bundle.c"
cc -std=c99 -I include "$WORK/sexpr_bundle.c" build/lib/libtinyparse.a -o "$WORK/sexpr_dump"
printf '(a b)' | "$WORK/sexpr_dump"
```

```text
Source
  Form
    List
      (
      Form
        Atom
      Space
      Form
        Atom
      )
```

On a failed parse it prints each diagnostic's message and exits with status 1.
