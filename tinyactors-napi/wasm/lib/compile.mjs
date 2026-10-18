// The compiler, as the addon defines it (src/compile.c): compileCharts is
// ta_compile_charts, compileSystem is ta_compile_system (a <system>
// manifest), a compile result is the object both return, and a resolver,
// any callable (base, source) => string, is ta_compile_options.resolve:
// the only place a resolver is given, so it lives at compile time only.
//
// Who holds what:
//   - a compile result holds the ta_compile_result in linear memory:
//     close() frees it (ta_compile_free), a FinalizationRegistry frees it
//     when close() was not called;
//   - `image` is a Uint8Array copied from the result on its first read,
//     kept on the object, the application's from then on;
//   - the call's sources are views in the environment's arena (a mark per
//     call); a resolver's texts and messages are copied into the
//     compilation's scratch arena (request->scratch, ta_web_resolve_room),
//     which lives until the compile call returns.
//
// The ECMAScript syntax check the addon makes after a compilation (each
// expression parsed by the engine in the native datamodel's shapes) is the
// native datamodel's, which this package does not have yet
// (docs/plan.xml, p5-datamodel).
import { CompileError, ResolveError, TinyactorsError, refusal } from './errors.mjs';
import { instance } from './state.mjs';

const SEVERITIES = ['note', 'warning', 'error'];
const ERROR = 2;
const WERROR = 2; // TA_COMPILE_WERROR
const TA_OK = 0, TA_ERROR_INVALID = -3, TA_ERROR_NO_MEMORY = -4;
const encoder = new TextEncoder();

// The results not closed: freed by the collector as a fallback. The token
// is the module and the result's pointer, never the object.
const unclosed = new FinalizationRegistry(({ module, result }) => {
  try {
    if (!module.poisoned) module.call('ta_web_compile_free', result);
  } catch {
    // a poisoned module frees nothing more
  }
});

// The token the constructor checks: only the compile calls make a result.
const MAKING = Symbol('making');

export class Compiled {
  #module;
  #result; // 0 once closed
  #image;

  constructor(making, module, result, diagnostics) {
    if (making !== MAKING) throw new TypeError('a compile result is made by compileCharts and compileSystem');
    this.#module = module;
    this.#result = result;
    Object.defineProperty(this, 'diagnostics', { value: diagnostics, enumerable: true });
    unclosed.register(this, { module, result }, this);
  }

  static #of(self) {
    if (self === null || typeof self !== 'object' || !(#result in self)) throw new TypeError('not a compile result');
    return self;
  }

  // The image, copied on the first read and the same after; a first read
  // after close() throws TA_ERROR_STALE.
  get image() {
    const self = Compiled.#of(this);
    if (self.#image !== undefined) return self.#image;
    if (self.#result === 0) throw refusal('TA_ERROR_STALE', 'the compile result was closed before its image was read');
    const module = self.#module;
    const at = module.call('ta_web_compile_image', module.env, self.#result) >>> 0;
    self.#image = module.copyOut(at, module.word(0));
    return self.#image;
  }

  // Frees the result; again, nothing.
  close() {
    const self = Compiled.#of(this);
    if (self.#result === 0) return;
    const result = self.#result;
    self.#result = 0;
    unclosed.unregister(self);
    self.#module.call('ta_web_compile_free', result);
  }

  [Symbol.dispose]() {
    this.close();
  }
}
Object.defineProperty(Compiled.prototype, 'image', { enumerable: true });

// One diagnostic as a frozen object; its record's words (compile.c).
function diagnosticAt(module, result, i) {
  module.call('ta_web_compile_diagnostic', module.env, result, i);
  const w = module.words().slice(module.record, module.record + 8);
  const severity = w[0];
  return Object.freeze({
    severity: SEVERITIES[severity < 3 ? severity : 2],
    code: w[1],
    file: module.string(w[4], w[5]),
    line: w[2],
    column: w[3],
    message: module.string(w[6], w[7]),
  });
}

// A compile call's resolver: the import's owner while the call runs.
class Resolving {
  constructor(module, resolver) {
    this.module = module;
    this.resolver = resolver;
    this.error = undefined; // the first exception that is not a refusal, thrown after the call
    this.thrown = false;
  }

  // Text into the compilation's scratch arena: [pointer, length].
  room(text) {
    const module = this.module;
    if (!text.isWellFormed()) throw refusal('TA_ERROR_INVALID', 'a string with a lone surrogate is not text');
    const bytes = encoder.encode(text);
    const at = module.call('ta_web_resolve_room', module.env, bytes.length) >>> 0;
    if (at === 0) throw refusal('TA_ERROR_NO_MEMORY', 'no memory for a string');
    module.bytes().set(bytes, at);
    return [at, bytes.length];
  }

  // ta_web.resolve: base and source in the record's words 0-3; the answer
  // in it (compile.c).
  resolve() {
    const module = this.module;
    const w = module.words().slice(module.record, module.record + 4);
    const base = module.string(w[0], w[1]), source = module.string(w[2], w[3]);
    let text;
    try {
      text = this.resolver(base, source);
      if (typeof text !== 'string') throw new TypeError('a resolver (base, source) => string returns a string');
      const [at, length] = this.room(text);
      module.words().set([TA_OK, w[2], w[3], at, length], module.record);
      return TA_OK;
    } catch (error) {
      let message = '', line = 0, column = 0;
      const position = (value) => (typeof value === 'number' && value >= 1 && value <= 4294967295 ? Math.trunc(value) : 0);
      if (error instanceof ResolveError) {
        message = this.text(error.message);
        line = position(error.line);
        column = position(error.column);
      } else {
        message = this.text(error);
        this.raised(error);
      }
      module.words().set([TA_ERROR_INVALID, w[2], w[3], 0, 0, line, column, message[0], message[1]], module.record);
      return TA_OK;
    }
  }

  // `value` as text in the scratch arena; empty when it cannot be had (the
  // compiler then gives its generic message).
  text(value) {
    try {
      return this.room(String(value));
    } catch {
      return [0, 0];
    }
  }

  // The first exception that is not a refusal (and the import guard's,
  // for one of resolve() itself): kept, thrown after the call.
  raised(error) {
    if (this.thrown) return;
    this.thrown = true;
    this.error = error;
  }
}

function compileWith(manifest, [given, resolver, options], usage) {
  let flags = 0;
  if (options !== undefined) {
    if (options === null || typeof options !== 'object') throw new TypeError('a compile call\'s options are an object: {warningsAsErrors}');
    const werror = options.warningsAsErrors;
    if (werror !== undefined && typeof werror !== 'boolean') throw new TypeError('warningsAsErrors is a boolean');
    if (werror) flags |= WERROR;
  }
  if (resolver !== undefined && resolver !== null && typeof resolver !== 'function') {
    throw new TypeError('a resolver is a function (base, source) => string');
  }
  const module = instance();
  const scratch = module.scratch;
  const sources = manifest ? [given] : given;
  if (!manifest && !Array.isArray(given)) throw new TypeError(usage);
  const resolving = typeof resolver === 'function' ? new Resolving(module, resolver) : null;
  const id = resolving ? module.register(resolving) : 0;
  try {
    return module.scoped(scratch, () => {
      const count = sources.length;
      const pairs = module.room(scratch, 16 * (count || 1)) >>> 2;
      for (let i = 0; i < count; i++) {
        const source = sources[i];
        if (source === null || typeof source !== 'object') throw new TypeError(usage);
        if (typeof source.name !== 'string') throw new TypeError(usage);
        const [n, nl] = module.text(scratch, source.name);
        if (typeof source.text !== 'string') throw new TypeError(usage);
        const [t, tl] = module.text(scratch, source.text);
        module.words().set([n, nl, t, tl], pairs + 4 * i);
      }
      module.words().set([pairs << 2, count, flags, resolving ? 1 : 0, id], module.record);
      const result = module.call('ta_web_compile', module.env, manifest ? 1 : 0) >>> 0;
      const status = module.word(0) | 0;
      let diagnostics, first;
      try {
        const list = [];
        const n = result ? module.call('ta_web_compile_diagnostic_count', result) : 0;
        for (let i = 0; i < n; i++) {
          const diagnostic = diagnosticAt(module, result, i);
          list.push(diagnostic);
          if (first === undefined && diagnostic.severity === 'error') {
            first = `${diagnostic.file}:${diagnostic.line}:${diagnostic.column}: error: ${diagnostic.message}`;
          }
        }
        diagnostics = Object.freeze(list);
      } catch (error) {
        if (result) module.call('ta_web_compile_free', result);
        throw error;
      }
      if (resolving?.thrown || status !== TA_OK || first !== undefined) {
        if (result) module.call('ta_web_compile_free', result); // freed before it throws
        if (resolving?.thrown) throw resolving.error;
        if (status === TA_ERROR_NO_MEMORY) throw refusal('TA_ERROR_NO_MEMORY', 'the compiler\'s allocator refused memory');
        const name = module.statusName(status === TA_OK ? TA_ERROR_INVALID : status);
        throw new CompileError(first ?? name, { diagnostics, status: name });
      }
      return new Compiled(MAKING, module, result, diagnostics);
    });
  } finally {
    if (id) module.unregister(id);
  }
}

export function compileCharts(...args) {
  return compileWith(false, args, 'compileCharts(sources, resolver?, options?): sources an array of {name, text} of strings');
}

export function compileSystem(...args) {
  return compileWith(true, args, 'compileSystem(manifest, resolver?, options?): the manifest a {name, text} of strings');
}

export { TinyactorsError };
