'use strict';

// System parameters (core ADR 0092, napi ADR 0035): set once when a system
// is created, each with a default and bounds. Core's come from its own
// table (ta_param_info_of), the host kernel's from its table
// (tah_param_table), napi's from the addon's; nothing is copied here.

const native = require('./native');
const { TinyactorsError } = require('./errors');

let table;

// The defaults a `tinyactors.params` attribute is read against (napi ADR
// 0036): the attribute names only the parameters that differ from them, so a
// reader must know which defaults applied. Change the version whenever a
// default changes; test/tracing.js holds each version's defaults and fails
// until it is changed.
const DEFAULTS_VERSION = 'v2';

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

/** Every parameter: name → { default, min, max, unit, owner: "core" | "kernel" | "napi" }. */
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

/**
 * The `tinyactors.params` resource attribute for `values` (napi ADR 0036):
 * the defaults' version, then `name=value` for each parameter that differs
 * from its default, in name order, separated by `;`. `v1` alone for a
 * system with every default.
 */
function parameterAttribute(values) {
  const differing = [];
  for (const [name, info] of tableOf()) {
    if (values[name] !== info.default) {
      differing.push(name);
    }
  }
  differing.sort();
  return [DEFAULTS_VERSION, ...differing.map((name) => `${name}=${values[name]}`)].join(';');
}

module.exports = { DEFAULTS_VERSION, parameterAttribute, parameterInfo, resolveParameters };
