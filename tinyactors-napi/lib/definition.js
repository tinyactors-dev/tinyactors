'use strict';

// Definition facade (ADRs 0001 and 0013). A Definition is a system and a
// definition ID, and nothing else: its identity, inspection and retired flag
// are read from core on every access, so the addon keeps no copy of a fact
// core owns.

const { core } = require('./native');
const { TinyactorsError } = require('./errors');

const utf8 = new TextDecoder('utf-8', { fatal: true });

// Core stores a revision as bytes. It reads back as a string when the bytes
// are valid UTF-8, and as a Uint8Array otherwise.
function revisionOf(bytes) {
  try {
    return utf8.decode(bytes);
  } catch {
    return new Uint8Array(bytes);
  }
}

function identityOf(identity) {
  return { family: identity.family.name, revision: revisionOf(identity.revision) };
}

// Core's "none" for a state, transition, expression or content ID.
const NONE = 4294967295;
const idOrNull = (id) => (id === NONE ? null : id);

// Core's lookups by ID (core ADRs 0042 and 0071), in the public shapes
// (ChartState, ChartTransition, ...). Every reader in the addon goes
// through these.
//
// Core answers `not-found` both for an ID out of range and for a freed
// definition. An ID is a question, so out of range answers undefined; a
// freed definition is an error, so it still throws.
function lookup(native, definition, id, read) {
  if (!Number.isInteger(id) || id < 0 || id >= NONE) {
    return undefined;
  }
  try {
    return read(id);
  } catch (error) {
    if (error instanceof TinyactorsError && error.code === 'not-found') {
      core.definition_inspect(native, definition); // throws `not-found` when freed
      return undefined;
    }
    throw error;
  }
}

function readState(native, definition, id) {
  return lookup(native, definition, id, () => {
    const info = core.definition_state(native, definition, id);
    return { id, name: info.id, parent: idOrNull(info.parent), kind: info.kind };
  });
}

function findState(native, definition, name) {
  if (typeof name !== 'string') {
    return undefined;
  }
  let id;
  try {
    id = core.definition_find_state(native, definition, name);
  } catch (error) {
    if (error instanceof TinyactorsError && error.code === 'not-found') {
      core.definition_inspect(native, definition); // throws `not-found` when freed
      return undefined;
    }
    throw error;
  }
  return readState(native, definition, id);
}

function readTransition(native, definition, id) {
  return lookup(native, definition, id, () => {
    const info = core.definition_transition(native, definition, id);
    const transition = {
      id,
      origin: info.origin,
      source: idOrNull(info.source),
      type: info.type,
      targets: [...info.targets],
      defaultTargets: info.default_targets,
      condition: idOrNull(info.condition),
      actionCount: info.action_count,
      documentOrder: info.document_order,
    };
    if (info.event !== undefined && info.event !== '') {
      transition.event = info.event;
    }
    return transition;
  });
}

function readAction(native, definition, id) {
  return lookup(native, definition, id, () => {
    const info = core.definition_action(native, definition, id);
    return {
      id,
      kind: info.kind,
      block: info.block,
      // The document's <script> has no owner (TA_STATE_NONE).
      owner: info.block === 'script' ? null : idOrNull(info.owner),
      blockIndex: info.block_index,
      index: info.index,
    };
  });
}

// An expression in the spec's own form: source text, or the host value
// the chart was built with (a function, a literal(), a { get, set }).
function readExpression(native, definition, id) {
  return lookup(native, definition, id, () => core.definition_expression(native, definition, id));
}

function readContent(native, definition, id) {
  return lookup(native, definition, id, () => {
    const info = core.definition_content(native, definition, id);
    const content = { bytes: new Uint8Array(info.bytes) };
    if (info.media_type !== undefined && info.media_type !== '') {
      content.mediaType = info.media_type;
    }
    return content;
  });
}

function readInvoke(native, definition, index) {
  return lookup(native, definition, index, () => {
    const info = core.definition_invoke(native, definition, index);
    const nested = info.nested === undefined || BigInt(info.nested) === 0n ? null : info.nested;
    return {
      index,
      type: info.has_type_expr ? null : info.type,
      content: idOrNull(info.content),
      src: idOrNull(info.src),
      nested,
    };
  });
}

// The spec a definition was compiled from (core ADR 0042), as core's
// export returns it: the shape define() takes, host values as the chart
// was built with them. Throws `not-found` once the definition is freed.
function exportSpec(native, definition) {
  return core.definition_export(native, definition);
}

// System's method behind Definition.encode(): the system holds the codecs.
const ENCODE = Symbol('tinyactors.encode');

/**
 * What Definition.encode() wrote (napi ADR 0027): the bytes, the explicit
 * identity to pass back to define(), and where each element landed.
 */
class EncodedChart {
  #spans;

  constructor(bytes, identity, spans) {
    this.bytes = bytes;
    if (identity !== undefined) {
      this.identity = identity;
    }
    this.#spans = spans;
  }

  /** Where the codec wrote element `id` of `kind` in `bytes`, as sourceOf() answers, or undefined. */
  placeOf(kind, id) {
    const span = this.#spans[kind]?.[id];
    if (span === undefined || span === null) {
      return undefined;
    }
    const place = { source: '', line: span.line };
    if (span.column !== 0) place.column = span.column;
    if (span.endLine !== 0) {
      place.endLine = span.endLine;
      if (span.endColumn !== 0) place.endColumn = span.endColumn;
    }
    if (span.length !== 0) {
      place.offset = span.offset;
      place.length = span.length;
    }
    return place;
  }
}

class Definition {
  #native;

  constructor(system, native, id) {
    this.#native = native;
    Object.defineProperty(this, 'system', { value: system, enumerable: true });
    Object.defineProperty(this, 'id', { value: id, enumerable: true });
  }

  /** The family and revision, read from core. */
  get identity() {
    return identityOf(core.definition_inspect(this.#native, this.id).identity);
  }

  /**
   * True once retire() has run. Core frees a retired definition as soon as
   * no resident actor uses it, and a freed definition's ID is stale, so
   * `not-found` also means retired: nothing else frees a registered
   * definition while its system is open.
   */
  get retired() {
    try {
      return core.definition_inspect(this.#native, this.id).retired;
    } catch (error) {
      if (error instanceof TinyactorsError && error.code === 'not-found') {
        return true;
      }
      throw error;
    }
  }

  /** A snapshot of core's view of this definition. */
  inspect() {
    const view = core.definition_inspect(this.#native, this.id);
    const inspection = {
      id: view.id,
      identity: identityOf(view.identity),
      datamodel: view.datamodel,
      binding: view.binding,
      retired: view.retired,
      actorCount: view.actor_count,
      stateCount: view.state_count,
      actorFixedBytes: view.actor_fixed_bytes,
      definitionBytes: view.definition_bytes,
      holds: view.holds,
      nestedBy: view.holders,
      transitionCount: view.transition_count,
      actionCount: view.action_count,
      expressionCount: view.expression_count,
      contentCount: view.content_count,
      invokeCount: view.invoke_count,
    };
    // Nesting is reported from parent to child (core ADR 0078): a child can
    // be nested by several parents, so it names none.
    const nested = [];
    for (let invoke = 0; invoke < view.invoke_count; invoke += 1) {
      const child = readInvoke(this.#native, this.id, invoke)?.nested;
      if (child !== undefined && child !== null) {
        nested.push(child);
      }
    }
    if (nested.length > 0) {
      inspection.nested = nested;
    }
    if (view.document_name !== undefined && view.document_name !== '') {
      inspection.name = view.document_name;
    }
    return inspection;
  }

  /**
   * Where element `id` of `kind` ('state', 'transition', 'action' or
   * 'expression') was written (core ADR 0085): the file, and the line and
   * column of its builder call, or undefined when the definition keeps no
   * span for it. A chart built with statechart() records every state,
   * transition, initial transition and action; IDs are core's, as in
   * inspect() and trace records.
   */
  sourceOf(kind, id) {
    let found;
    try {
      found = core.definition_span(this.#native, this.id, kind, id);
    } catch (error) {
      if (error instanceof TinyactorsError && error.code === 'not-found') {
        return undefined;
      }
      throw error;
    }
    const { span, source } = found;
    // Core keeps zero for a column the front end did not know (a spec's
    // span may give only a line); the place leaves it out.
    const place = { source: source ?? '', line: span.line };
    if (span.column !== undefined && span.column !== 0) {
      place.column = span.column;
    }
    if (span.end_line !== undefined && span.end_line !== 0) {
      place.endLine = span.end_line;
      if (span.end_column !== undefined && span.end_column !== 0) {
        place.endColumn = span.end_column;
      }
    }
    if (span.length !== undefined && span.length !== 0) {
      place.offset = Number(span.offset ?? 0n);
      place.length = span.length;
    }
    return place;
  }

  /**
   * The spec this definition was compiled from (core ADR 0042, napi ADR
   * 0027), in the shape define() takes: its identity when explicit, spans
   * and sources, expressions as authored (source text, the chart's own
   * functions and literal() values). Defining it again gives a definition
   * with an equal export; for a chart without functions, this one.
   */
  export() {
    return exportSpec(this.#native, this.id);
  }

  /**
   * The chart in a registered codec's syntax, by its name or an extension
   * (napi ADR 0027): { bytes, identity?, placeOf(kind, id) }. Defining the
   * bytes with { format, identity } answers a definition with this one's
   * revision. `unsupported` when the codec only reads; `invalid` with the
   * codec's code as `detail.code` when it cannot write the chart. Host
   * expressions are printed by the data model the chart is written for
   * (napi ADR 0031): its own, or `options.datamodel`, which the text then
   * names; the built-in one refuses a function (`unrepresentable`).
   */
  encode(format, options) {
    return this.system[ENCODE](this.id, format, this, options);
  }

  /** State `id` as { id, name, parent, kind }, or undefined out of range. */
  state(id) {
    return readState(this.#native, this.id, id);
  }

  /** The state whose author ID is `name`, or undefined. */
  findState(name) {
    return findState(this.#native, this.id, name);
  }

  /** Transition `id`, initial transitions after the <transition> elements. */
  transition(id) {
    return readTransition(this.#native, this.id, id);
  }

  /** Action `id`, and the block of executable content it sits in. */
  action(id) {
    return readAction(this.#native, this.id, id);
  }

  /** Expression `id` as the spec holds it: text, a function, a literal(). */
  expression(id) {
    return readExpression(this.#native, this.id, id);
  }

  /** Literal content `id`: { bytes, mediaType? }. */
  content(id) {
    return readContent(this.#native, this.id, id);
  }

  /** The <invoke> at declaration `index`. */
  invoke(index) {
    return readInvoke(this.#native, this.id, index);
  }

  /**
   * Prevent new actors from using this definition. Actors that use it keep
   * running; core frees it when the last one is gone. Idempotent.
   */
  retire() {
    core.system_retire_definition(this.#native, this.id);
  }
}

module.exports = {
  Definition, EncodedChart, ENCODE, identityOf, readState, findState, readContent,
};
