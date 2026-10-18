// @tinyactors/wasm/init: the package's names and `await init(url?)`, for a
// Service Worker (which refuses a module graph with top-level await) and
// for bundlers that do not handle it. A call before init() settles throws
// TA_ERROR_STATE. The API's classes join these exports with p5-api.
export { init } from './lib/state.mjs';
export { TinyactorsError } from './lib/errors.mjs';
