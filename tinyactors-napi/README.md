# tinyactors-napi

`tinyactors-napi` is the Node-API binding for Tinyactors. It targets Node.js
22 or newer and Bun.

The binding is currently a bootstrap stub. It exports only `version`, which is
the package version:

```js
const tinyactors = require('tinyactors-napi');

console.log(tinyactors.version);
```

Build and test it through the repository task interface:

```text
mise run build
mise run test
```

The test loads the same native addon with Node.js and Bun. See
[`docs/design.md`](docs/design.md) for the planned binding architecture.
