# `builder.h`

The builder is the source-language-neutral construction API for definitions.

It should eventually express the entire statechart model needed by the interpreter:

- atomic, compound, parallel, final, shallow-history, and deep-history states;
- initial transitions and their actions;
- entry/exit actions;
- transitions with zero or more event descriptors, optional conditions, type, targets, and actions;
- data declarations/binding metadata as required by the selected data model;
- sends, raises, conditionals, iteration, assignments/scripts, invokes, finalize blocks, and extension actions.

Compilation performs structural validation and emits compact immutable tables/layouts.

SCXML XML import, generated C, Ruby/Python builders, or other DSLs should all target this same builder/IR rather than adding alternate runtime semantics.
