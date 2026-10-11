# temporal-polyfill 1.0.5 (vendored)

The oracle for staticjs's Temporal values. Node 22 has no `Temporal`, and
its `--harmony-temporal` draft gives wrong values, so `tests/diff-node.mjs`
evaluates the Temporal spec cases against this polyfill instead.

- Package: [`temporal-polyfill`](https://www.npmjs.com/package/temporal-polyfill)
  1.0.5, by Adam Shaw (FullCalendar), MIT license (`LICENSE`, copied from the
  package).
- Tarball: `https://registry.npmjs.org/temporal-polyfill/-/temporal-polyfill-1.0.5.tgz`,
  integrity `sha512-+H/mmY74i6wXCMcjIhGuSnODcyZnc4eois2myhOyX0UqRAKNXQY5m9iB1en5jQ/n4SXtjLXo1ElBI5bmn8tZwQ==`.
- File: `global.js`, the package's single-file script build, unchanged.
  sha256 `7af6124d7d580545620d9fdd421d49967b6b7e307e06a4d496d160bcd5f3b1f3`
  (`LICENSE`: `c29814b29591d08896b63870ffdd652785f5b2e3f902860645f00871b596068d`).

`global.js` defines `globalThis.Temporal` when the global it runs in has
none (it also extends `Intl.DateTimeFormat` and `Date.prototype`).
`diff-node.mjs` runs it in a context of its own (`node:vm`) and makes that
context's `Temporal` the `Temporal` of every case it evaluates, so the
polyfill changes nothing else the cases see. Nothing is fetched when the
tests run.

Every expected value of a Temporal spec case comes from this polyfill, and
is checked against `@js-temporal/polyfill` 0.5.1 (the proposal's reference
polyfill) when the case is written; a case on which the two disagree is not
used (`docs/temporal.md`).

To update: `npm pack temporal-polyfill@<version>`, check the tarball's
integrity against the registry's, copy `package/global.js` and
`package/LICENSE` here, and update the version and hashes above.
