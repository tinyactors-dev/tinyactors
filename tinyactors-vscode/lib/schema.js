'use strict';

// The part of JSON Schema that package.json's configurationAttributes use,
// checked against a debug configuration. The extension refuses a
// configuration that breaks its schema before it starts an adapter, with
// the first problem found, rather than leave the adapter to answer the
// `launch` or `attach` with an error.
//
// VS Code reads the same schema for completion and validation in
// launch.json. A keyword this file does not know is not used there: the
// contributes test checks every schema against KEYWORDS.

// Keywords that constrain a value.
const CONSTRAINTS = ['type', 'enum', 'properties', 'required', 'additionalProperties', 'items', 'uniqueItems',
  'minimum', 'maximum', 'minLength', 'pattern'];
// Keywords that only describe one; VS Code shows them in launch.json.
const ANNOTATIONS = ['description', 'markdownDescription', 'default', 'enumDescriptions', 'examples'];
const KEYWORDS = [...CONSTRAINTS, ...ANNOTATIONS];
const TYPES = ['string', 'number', 'integer', 'boolean', 'object', 'array', 'null'];

function typeOf(value) {
  if (value === null) return 'null';
  if (Array.isArray(value)) return 'array';
  if (Number.isInteger(value)) return 'integer';
  return typeof value;
}

function hasType(value, type) {
  const actual = typeOf(value);
  return actual === type || (type === 'number' && actual === 'integer');
}

function show(value) {
  return JSON.stringify(value);
}

// The first way `value` breaks `schema`, as a sentence about `path`, or
// undefined when it holds.
function check(value, schema, path) {
  if (schema.type !== undefined) {
    const types = [].concat(schema.type);
    if (!types.some((type) => hasType(value, type))) {
      return `"${path}" is ${types.join(' or ')}, not ${show(value)}`;
    }
  }
  if (schema.enum !== undefined && !schema.enum.includes(value)) {
    return `"${path}" is one of ${schema.enum.map(show).join(', ')}, not ${show(value)}`;
  }
  switch (typeOf(value)) {
    case 'string':
      if (schema.minLength !== undefined && value.length < schema.minLength) return `"${path}" is empty`;
      if (schema.pattern !== undefined && !new RegExp(schema.pattern, 'u').test(value)) {
        return `"${path}" does not match ${schema.pattern}: ${show(value)}`;
      }
      return undefined;
    case 'integer':
    case 'number':
      if (schema.minimum !== undefined && value < schema.minimum) return `"${path}" is at least ${schema.minimum}`;
      if (schema.maximum !== undefined && value > schema.maximum) return `"${path}" is at most ${schema.maximum}`;
      return undefined;
    case 'array':
      return checkArray(value, schema, path);
    case 'object':
      return checkObject(value, schema, path);
    default:
      return undefined;
  }
}

function checkArray(value, schema, path) {
  if (schema.uniqueItems === true && new Set(value.map(show)).size !== value.length) {
    return `"${path}" lists an entry twice`;
  }
  if (schema.items === undefined) return undefined;
  for (const [index, item] of value.entries()) {
    const problem = check(item, schema.items, `${path}[${index}]`);
    if (problem !== undefined) return problem;
  }
  return undefined;
}

function checkObject(value, schema, path) {
  const inside = (name) => (path === '' ? name : `${path}.${name}`);
  for (const name of schema.required ?? []) {
    if (value[name] === undefined) return `"${inside(name)}" is required`;
  }
  const properties = schema.properties ?? {};
  for (const [name, entry] of Object.entries(value)) {
    // As in JSON, a field whose value is undefined is not there.
    if (entry === undefined) continue;
    if (properties[name] !== undefined) {
      const problem = check(entry, properties[name], inside(name));
      if (problem !== undefined) return problem;
    } else if (schema.additionalProperties === false) {
      return `"${inside(name)}" is not a field of ${path === '' ? 'this configuration' : `"${path}"`}`;
    }
  }
  return undefined;
}

module.exports = { check, KEYWORDS, TYPES };
