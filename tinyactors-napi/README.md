# @tinyactors/node

SCXML statecharts as actors, from Node.js and Bun: a Node-API addon that
drives the tinyactors host, with chart logic in JavaScript.

**Status: not usable yet.** This directory holds the project's skeleton:
its toolchain, tasks, lint rules and the empty mapping table. There is no
addon to load. The last published version, 0.2.0, was built from an
earlier binding with another API, which this package replaces. What is
being built, in what order, and how far it is: [ROADMAP.md](ROADMAP.md).

## Limits, to be documented here

Chart code is trusted: it runs in the application's realm, and Node-API
cannot make another. Values nest at most 64 deep, which stops cycles but
not a value that shares subtrees.

## License

MIT, as the repository ([LICENSE](../LICENSE)).
