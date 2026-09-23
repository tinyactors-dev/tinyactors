'use strict';

// The compiled addon. Every other lib/ module reaches native code through
// this one, so the addon is loaded, and its error class registered, once.

module.exports = require('../build/tinyactors_napi.node');
