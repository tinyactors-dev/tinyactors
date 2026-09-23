'use strict';

const native = require('./lib/native');
const { TinyactorsError } = require('./lib/errors');
const { createSystem } = require('./lib/system');

module.exports = {
  version: native.version,
  createSystem,
  TinyactorsError,
};
