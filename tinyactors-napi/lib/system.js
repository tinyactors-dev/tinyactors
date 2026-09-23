'use strict';

// System facade over the native System (ADR 0012). Later steps add the
// generated core module and the plumbing behind it.

const native = require('./native');
const { TinyactorsError } = require('./errors');
const { Definition } = require('./definition');
const { DefinitionBuilder } = require('./statechart');

const { core } = native;

// Compiles a spec in the generated shape. A refused chart throws with core's
// compile error formatted into the message (ADR 0058), and the error itself,
// with a readable path, as `detail`.
function compile(system, spec) {
  try {
    return core.definition_compile_spec(system, spec);
  } catch (error) {
    const refusal = error instanceof TinyactorsError ? error.detail?.error : undefined;
    if (refusal === undefined) {
      throw error;
    }
    let text;
    try {
      text = core.compile_error_format(spec, refusal);
    } catch {
      text = `${refusal.code}: ${refusal.message}`;
    }
    throw new TinyactorsError(`chart refused: ${text}`, error.code, {
      error: refusal,
      code: refusal.code,
      path: pathText(refusal.path),
    });
  }
}

// `states[0].transitions[1].condition`, from core's path elements.
function pathText(path) {
  const noIndex = 4294967295;
  return (path ?? [])
    .map(({ member, index }, at) => `${at === 0 ? '' : '.'}${member}${index === noIndex ? '' : `[${index}]`}`)
    .join('');
}

// The native System behind a public one, for other lib/ modules and tests.
// index.js does not export it.
let nativeOf;

class System {
  #native;

  static {
    nativeOf = (system) => system.#native;
  }

  constructor(options) {
    if (options !== undefined && (options === null || typeof options !== 'object')) {
      throw new TinyactorsError('createSystem options must be an object', 'invalid');
    }
    this.#native = new native.System();
  }

  /** True once close() has run. */
  get closed() {
    return this.#native.closed;
  }

  /**
   * Compile and register a chart: a statechart() builder, or a spec in the
   * generated shape (the shape core's definition export returns). Until the
   * JavaScript data model lands (ROADMAP step 11), charts compile against
   * core's null data model.
   */
  define(source) {
    let spec = source;
    if (source instanceof DefinitionBuilder) {
      spec = source.build();
    } else if (source === null || typeof source !== 'object') {
      throw new TinyactorsError('define(): expected a statechart() builder or a spec object', 'invalid');
    }
    const compiled = compile(this.#native, spec);
    const id = core.system_register_definition(this.#native, compiled.out);
    return new Definition(this, this.#native, id);
  }

  /**
   * The newest registered, unretired definition with this identity, or
   * undefined.
   */
  findDefinition(identity) {
    if (identity === null || typeof identity !== 'object') {
      throw new TinyactorsError('findDefinition(): expected { family, revision }', 'invalid');
    }
    try {
      const id = core.system_find_definition(this.#native, {
        family: { name: identity.family },
        revision: identity.revision,
      });
      return new Definition(this, this.#native, id);
    } catch (error) {
      if (error instanceof TinyactorsError && error.code === 'not-found') {
        return undefined;
      }
      throw error;
    }
  }

  /**
   * Memory counters by category and the number of allocator calls
   * (ADRs 0009 and 0014). Throws `closed` after close().
   */
  stats() {
    return this.#native.stats();
  }

  /**
   * Destroy the native system and everything it holds. Idempotent and
   * authoritative: afterwards every method except close() throws `closed`.
   */
  close() {
    this.#native.close();
  }

  [Symbol.dispose]() {
    this.close();
  }
}

function createSystem(options) {
  return new System(options);
}

module.exports = { System, createSystem, nativeOf };
