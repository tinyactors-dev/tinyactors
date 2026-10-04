Implementation modules. Headers here are private; the public API is
`../include/staticjs.h`.

- `internal.h`  what the modules share: values, built-in IDs, opcodes, programs
- `memory.c`    the allocator, growable buffers, the context, errors
- `text.c`      UTF-8, and JavaScript's UTF-16 view of it
- `number.c`    Number::toString, remainder and rounding, decimal literals
- `value.c`     strings, arrays, objects, sets, closures; release without recursion
- `syntax.h`    the lexer and its tokens
- `lexer.c`     bytes to tokens, one byte at a time
- `parser.c`    tokens to syntax events, by operator precedence over explicit stacks
- `compiler.c`  syntax events to bytecode, as each arrives; scopes and captures
- `vm.h`        the evaluator's state, shared with the built-ins
- `vm.c`        the evaluator: one instruction per unit of fuel
- `builtins.c`  built-in functions and methods; the ones that call back are frames
- `writer.c`    a value to JavaScript source, into a buffer of any size
