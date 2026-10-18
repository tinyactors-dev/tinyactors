// @tinyactors/wasm: the package's names, its module loaded with top-level
// await (the bundled tinyactors.wasm, beside this file), so
// `import { Host } from '@tinyactors/wasm'` is the line it is in Node.
// The API's classes join these exports with p5-api.
import { init } from './lib/state.mjs';

await init();

export { TinyactorsError } from './lib/errors.mjs';
