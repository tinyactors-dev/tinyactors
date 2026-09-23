'use strict';

const native = require('./build/tinyactors_napi.node');

module.exports = {
  version: native.version,
};
