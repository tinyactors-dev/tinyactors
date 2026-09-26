'use strict';

// Where a chart's elements were written (core ADR 0086): statechart()
// records the file, line and column of the builder call that wrote each
// state, transition, initial transition and action, and
// Definition.sourceOf() reads one back by core's ID. This is how the
// debugger gives stack frames a source, maps a line breakpoint to the
// element on that line, answers breakpointLocations, and puts goto targets
// on lines. A spec built without spans has none of it, and the adapter
// then works as it did before: frames on line 0, lines unverified.

const { core } = require('../native');
const { Definition } = require('../definition');

const KINDS = ['state', 'transition', 'action'];

// A span's source as a path a client can open: `file://` URLs lose the scheme.
function pathOf(source) {
  if (typeof source !== 'string' || source === '') return undefined;
  return source.startsWith('file://') ? decodeURIComponent(source.slice('file://'.length)) : source;
}

const basename = (path) => path.slice(Math.max(path.lastIndexOf('/'), path.lastIndexOf('\\')) + 1);

// Whether a client's path names the same file as a span's.
function samePath(left, right) {
  if (left === undefined || right === undefined) return false;
  const a = left.replaceAll('\\', '/');
  const b = right.replaceAll('\\', '/');
  return a === b || a.endsWith(`/${b}`) || b.endsWith(`/${a}`);
}

class Sources {
  #system;
  #native;
  #elements = new Map();

  constructor(system, native) {
    this.#system = system;
    this.#native = native;
  }

  /** Where one element was written, as { path, line, column }, or undefined. */
  place(definition, kind, id) {
    if (id === null || id === undefined) return undefined;
    const found = this.#definition(definition).sourceOf(kind, id);
    const path = pathOf(found?.source);
    return path === undefined || !(found.line > 0) ? undefined : { path, line: found.line, column: found.column };
  }

  /** The DAP Source object for a place. */
  static source(place) {
    return { name: basename(place.path), path: place.path };
  }

  /** Every located state, transition and action of a definition, as { kind, id, path, line, column }. */
  elements(definition) {
    const key = String(definition);
    let list = this.#elements.get(key);
    if (list !== undefined) return list;
    list = [];
    const counts = core.definition_inspect(this.#native, definition);
    const limits = { state: counts.state_count, transition: counts.transition_count, action: counts.action_count };
    for (const kind of KINDS) {
      for (let id = 0; id < limits[kind]; id += 1) {
        const place = this.place(definition, kind, id);
        if (place !== undefined) list.push({ kind, id, ...place });
      }
    }
    list.sort((left, right) => left.line - right.line || left.column - right.column);
    this.#elements.set(key, list);
    return list;
  }

  /** The files a definition's spans name. */
  files(definition) {
    return [...new Set(this.elements(definition).map((element) => element.path))];
  }

  /**
   * The element a line breakpoint in `path` at `line` stands for: the first
   * element on that line, else the nearest one after it; an action before
   * a transition before a state on the same line, as the innermost
   * element there. Undefined when the file names none of the definition's.
   */
  locate(definition, path, line) {
    const inFile = this.elements(definition).filter((element) => samePath(path, element.path));
    const rank = { action: 0, transition: 1, state: 2 };
    const byRank = (list) => [...list].sort((left, right) => rank[left.kind] - rank[right.kind]);
    const on = inFile.filter((element) => element.line === line);
    if (on.length > 0) return byRank(on)[0];
    const after = inFile.filter((element) => element.line > line);
    if (after.length === 0) return undefined;
    const next = after[0].line;
    return byRank(after.filter((element) => element.line === next))[0];
  }

  /** The lines of `path` from `line` to `endLine` that hold an element. */
  lines(definition, path, line, endLine = line) {
    return this.elements(definition).filter((element) => samePath(path, element.path) &&
      element.line >= line && element.line <= endLine);
  }

  /**
   * Forgets what was read of a definition. A chart defined again after it
   * moved is answered by the same definition (core ADR 0077), which then
   * says where the chart is written now (core ADR 0086).
   */
  forget(definition) {
    this.#elements.delete(String(definition));
  }

  #definition(id) {
    return new Definition(this.#system, this.#native, id);
  }
}

module.exports = { Sources, samePath };
