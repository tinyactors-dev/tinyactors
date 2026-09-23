'use strict';

const native = require('./lib/native');
const { TinyactorsError } = require('./lib/errors');
const { createSystem } = require('./lib/system');
const { statechart, literal } = require('./lib/statechart');

module.exports = {
  version: native.version,
  createSystem,
  statechart,
  literal,
  TinyactorsError,
};
