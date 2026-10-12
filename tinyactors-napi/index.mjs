// @tinyactors/node: loads the platform's addon and re-exports it, nothing
// else (AGENTS.md: there is no JavaScript above the addon).
import { createRequire } from 'node:module';

const require = createRequire(import.meta.url);
const addon = require('./build/tinyactors.node');

export const {
  encode,
  decode,
  compile,
  Host,
  SimulatedClock,
  TinyactorsError,
  OK,
  ERROR_EXECUTION,
  ERROR_COMMUNICATION,
  ERROR_INVALID,
  ERROR_NO_MEMORY,
  ERROR_STALE,
  ERROR_STATE,
  ERROR_UNSUPPORTED,
  ERROR_FATAL,
  ERROR_ABORTED,
} = addon;
