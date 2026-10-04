# Host class bindings — `(class NAME)`

This example shows the `(class NAME)` token primitive and the host *CLASS
bindings* it resolves against.

A `(class NAME)` form matches exactly **one** input byte, and only if the host
has installed a CLASS binding named `NAME` that accepts that byte. The binding is
an ordinary host C function — `tinyparse_class_fn`, `int fn(void *user,
uint32_t byte)`, returning nonzero when the byte is in the class — registered on
the engine with `tinyparse_bind`. The freestanding core never hard-codes a
character class beyond the small regex subset; richer or host-specific classes
(Unicode categories, identifier policy) come in this way.

## The built-in classes

The `tinyparse` CLI installs four ASCII classes before it compiles or parses a
grammar, for both `check` and `run`:

| name    | accepts                          |
|---------|----------------------------------|
| `alpha` | `A`–`Z`, `a`–`z`                 |
| `digit` | `0`–`9`                          |
| `alnum` | `alpha` or `digit`               |
| `space` | space, tab, newline, return      |

## The grammar

[`ident.scm`](ident.scm) defines an identifier as an alphabetic byte followed by
zero or more alphanumerics, with whitespace as trivia:

```scheme
(token Ident
  (seq
    (class alpha)
    (repeat (class alnum))))
```

## Running it

```sh
printf 'foo bar123 x' | tinyparse run examples/bindings/ident.scm \
    --input - --dump-tree
```

dumps a `Source` of three `Ident` nodes and reports no error:

```
Source
  Item
    Ident
  Space
  Item
    Ident
  Space
  Item
    Ident
```

## Binding your own class

Any host can register its own CLASS binding the same way the CLI does:

```c
static int is_xid_start(void *user, uint32_t byte) { /* ... */ }

tinyparse_binding binding;
binding.name     = /* "xid-start" as a tinyparse_string */;
binding.kind     = TINYPARSE_BIND_CLASS;
binding.function = (void *)is_xid_start;
binding.user     = NULL;
tinyparse_bind(engine, &binding);
```

A grammar can then write `(class xid-start)` and the matcher will call the host
function for each candidate byte.
