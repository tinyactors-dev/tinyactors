# bufferfriendly: parse a growing buffer

`bufferfriendly.c` feeds a document to a generated s-expression parser a few
bytes at a time, calling the parser's `P_run_chunk` entry after each chunk.
Each call parses the top-level forms that are complete in the buffer and
returns `TINYPARSE_MORE`; the call on the last chunk finishes the tree,
which equals the tree `tinyparse run` builds from the whole document. The
example uses [`P_run_chunk`](../../docs/reference/c-api.md#p_run_chunk), the
other entries of the [generated parser
interface](../../docs/reference/c-api.md#generated-parser-interface) and the
[tree cursor](../../docs/reference/c-api.md#tree-cursor) functions.

## Prerequisites

Run the commands from the `tinyparse/` directory of a source tree, as
[Examples](../README.md) describes. A generated parser builds only from a
source tree, and this program also includes `src/internal.h`.

## Run it

Build the example programs, then run this one on the sample, a 190-byte file
of five top-level lists and a comment, in chunks of 4 bytes:

```sh
mise run examples
build/examples/bufferfriendly/bufferfriendly examples/bufferfriendly/sample.sx 4
```

Standard error gets the number of calls, the number that returned
`TINYPARSE_MORE`, and the final result code, where 0 is `TINYPARSE_OK`:

```text
bufferfriendly: 48 calls, 47 TINYPARSE_MORE suspensions, result=0
```

Standard output gets the tree in the format of `tinyparse run --dump-tree`.
It is 121 lines; all but the first 6 are cut:

```text
Source [0-190] [0,0]-[6,0]
  Form [0-27] [0,0]-[0,27]
    List [0-27] [0,0]-[0,27]
      open: ( [0-1] [0,0]-[0,1]
      Form [1-7] [0,1]-[0,7]
        Atom [1-7] [0,1]-[0,7]
...
```

The second argument is the chunk size in bytes, 4 when it is left out. A
chunk of 1 byte takes 190 calls, and a chunk larger than the file takes one
call that returns no `TINYPARSE_MORE`. The tree is the same for every chunk
size, and the same as this command prints:

```sh
tinyparse run examples/bufferfriendly/sexpr.scm --input examples/bufferfriendly/sample.sx --dump-tree
```

## How it works

`mise run examples` generates the parser from this directory's copy of the
s-expression grammar, under the prefix `sx`, compiles it with
`-std=c11 -O2 -I include -I src`, and links it with `bufferfriendly.c` and
`build/lib/libtinyparse.a`:

```sh
tinyparse c examples/bufferfriendly/sexpr.scm --parser-lib --name sx --out build/examples/bufferfriendly/sx.c
```

The grammar's start rule is `(rule Source (seq (repeat Form) EOF))`: a repeat
of top-level items, then the end of input. That shape is what makes
`tinyparse c` emit `sx_run_chunk` beside `sx_load` and `sx_run`. A generated
parser calls one C function per rule, so it can stop only between top-level
items, where none of those calls is in progress.

The program reads the whole file first, loads the grammar with `sx_load` and
creates a parser. Then, for each chunk, it:

1. pushes the chunk with `tinyparse_push`;
2. sets `parser->input_complete` to 1 for the last chunk and to 0 before;
3. calls `sx_run_chunk`.

`input_complete` is a field of the parser structure in `src/internal.h`, so
the program includes that header and is compiled with `-I src`. The parser
cannot tell on its own that no more bytes will come; the program sets the
field, and the call that sees it set returns what `sx_run` returns for the
whole input. An empty file takes one call with `input_complete` set.

## Limits

- A syntax error is reported only by the last call. Earlier calls return
  `TINYPARSE_MORE` however early the error is: for the input `) (a) (b) (c)`
  in 1-byte chunks, the first 12 of 13 calls return `TINYPARSE_MORE` and the
  13th returns `TINYPARSE_SYNTAX_ERROR`. The program then prints
  `error: unexpected input` and exits with status 1.
- `P_run_chunk` exists only for a grammar whose start rule is a top-level
  repetition ([P_run_chunk](../../docs/reference/c-api.md#p_run_chunk) gives
  the forms). For any other grammar, use `P_run` on the whole input, or the VM, which takes
  input in chunks through `tinyparse_push` and parses it in `tinyparse_finish`.
- The program's grant is a 512 MiB static array with no `grow` callback.

## Files

| File | Contents |
|------|----------|
| `bufferfriendly.c` | The program: `bufferfriendly FILE [CHUNK-SIZE]`, with `-` for standard input. |
| `sexpr.scm` | The s-expression grammar the parser is generated from. |
| `sample.sx` | The sample document. |
