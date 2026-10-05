Implementation modules. Headers here are private; the public API is
`../include/staticjs.h`.

- `internal.h`  what the modules share: values, built-in IDs, opcodes, programs
- `memory.c`    the allocator, growable buffers, the context, errors
- `text.c`      UTF-8, and JavaScript's UTF-16 view of it
- `number.c`    Number::toString, remainder and rounding, decimal literals
- `value.c`     strings, arrays, objects, sets, closures; release without recursion
- `syntax.h`    the lexer and its tokens
- `lexer.c`     bytes to tokens, one byte at a time
- `parser.c`    tokens to syntax events, by operator precedence over explicit stacks;
                statements by a frame per block and per if (an else-if chain is one frame)
- `compiler.c`  syntax events to bytecode, as each arrives; scopes and captures;
                block scopes inside function scopes, their slots reused; the last
                read of each slot moves its value (TAKE_LOCAL); each event goes
                through the matcher first, and a match tags its function
- `shapes.rules` the trees the matcher looks for (docs/matcher.md); `mise run shapes`
                compiles them to `shapes.h` (the shapes, for src/ and the tests) and
                `shapes_table.h` (the matcher's tables), both generated and checked in
- `matcher.h`   the matcher's interface, private to the compiler and its tests
- `matcher.c`   the matcher: a bottom-up tree automaton (iburg-style dynamic
                programming) over the events as they arrive; it keeps only brackets
                a rule can still match
- `vm.h`        the evaluator's state, shared with the built-ins
- `vm.c`        the evaluator: one instruction per unit of fuel
- `builtins.c`  built-in functions and methods; the ones that call back are frames,
                and `map` and `toSorted` run a callback the matcher tagged in C
- `writer.c`    a value to JavaScript source, into a buffer of any size
