'use strict';

// System parameters (core ADR 0092, napi ADR 0035): set once when a system
// is created, each with a default and bounds. Core's come from its own
// table (ta_param_info_of), napi's from the addon's; nothing is copied here.

const native = require('./native');
const { TinyactorsError } = require('./errors');

let table;

function tableOf() {
  if (table === undefined) {
    table = new Map();
    for (const entry of native.parameterTable()) {
      table.set(entry.name, Object.freeze({
        default: entry.default, min: entry.min, max: entry.max, unit: entry.unit, owner: entry.owner,
      }));
    }
  }
  return table;
}

/** Every parameter: name → { default, min, max, unit, owner: "core" | "napi" }. */
function parameterInfo() {
  return Object.freeze(Object.fromEntries(tableOf()));
}

/**
 * `given` checked against the table, with every parameter it leaves out at
 * its default: a frozen object naming every parameter. Throws `invalid`
 * naming the parameter and its bounds.
 */
function resolveParameters(given) {
  if (given !== undefined && (given === null || typeof given !== 'object' || Array.isArray(given))) {
    throw new TinyactorsError('createSystem(): parameters must be an object', 'invalid');
  }
  const entries = tableOf();
  for (const name of Object.keys(given ?? {})) {
    if (!entries.has(name)) {
      throw new TinyactorsError(`createSystem(): unknown parameter "${name}"`, 'invalid');
    }
  }
  const resolved = {};
  for (const [name, info] of entries) {
    const value = given?.[name];
    if (value === undefined) {
      resolved[name] = info.default;
      continue;
    }
    if (!Number.isSafeInteger(value) || value < info.min || value > info.max) {
      throw new TinyactorsError(
        `createSystem(): parameters.${name} must be an integer from ${info.min} to ${info.max}`, 'invalid');
    }
    resolved[name] = value;
  }
  return Object.freeze(resolved);
}

module.exports = { parameterInfo, resolveParameters };
