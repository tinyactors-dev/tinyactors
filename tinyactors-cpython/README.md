# tinyactors for Python

**Status: bootstrap written from scratch.** The C extension exposes
`encode` and `decode` for [driver-frame values](docs/values.md). Host and
actor execution are still planned in [ROADMAP.md](ROADMAP.md).

Run `mise run build` to build the extension. `mise run test` checks lint
and the codec and adapter on both interpreters, then runs M1. M1 is
deliberately red: the adapter exchanges valid driver frames and answers
every method with -32601 until its real public API exists. Run it alone
with `mise run conform -- --milestone M1`.

## License

MIT, as the repository ([LICENSE](../LICENSE)).
