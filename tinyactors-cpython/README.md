# tinyactors for Python

SCXML statecharts as actors, from Python: the `tinyactors` extension
module drives the tinyactors host, and chart logic is written in Python.

**Status: not usable yet.** This directory holds the project's skeleton:
its toolchain, tasks, lint rules and the empty mapping table. There is no
module to import. What is being built, in what order, and how far it
is: [ROADMAP.md](ROADMAP.md).

## Limits, to be documented here

Chart code is trusted: CPython cannot be preempted in the middle of an
expression or limited in memory, and Python values lie outside the
host's allocator. Values nest at most 64 deep, which stops cycles but
not a value that shares subtrees.

## License

MIT, as the repository ([LICENSE](../LICENSE)).
