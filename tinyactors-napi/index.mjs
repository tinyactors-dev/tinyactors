// @tinyactors/node: loads the addon and re-exports it, nothing else
// (AGENTS.md, "Layers and the resolver"). The addon is build/tinyactors.node,
// which `mise run build` makes; when packaging is built (`mise run pack`,
// ROADMAP.md), the platform's package comes first and build/ stays the
// development build.
import { createRequire } from 'node:module';

const require = createRequire(import.meta.url);
const addon = require('./build/tinyactors.node');

export const {
  encode,
  decode,
  TinyactorsError,
  CompileError,
  ResolveError,
  compileCharts,
  compileSystem,
  Host,
  SimulatedClock,
  Address,
} = addon;
