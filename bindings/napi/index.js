"use strict";
const lowlevel = require("./lowlevel");
const { defineMachine } = require("./lib/compiler");
const {
  createSystem,
  System,
  Actor,
  Lease,
  TinyActorsError,
} = require("./lib/system");
const helpers = require("./lib/helpers");
module.exports = {
  defineMachine,
  createSystem,
  System,
  Actor,
  Lease,
  TinyActorsError,
  ...helpers,
};
Object.defineProperty(module.exports, "lowlevel", {
  enumerable: true,
  value: lowlevel,
});
