// Atlas's data: the core's types with their byte layouts, its functions
// with their calls, and what an actor, a message and a delayed send cost.
// Pure: no DOM and no file system, so main.mjs (Node.js) and page.mjs (the
// browser) compute the same rows from the same atlas.json.

/** The layers from the bottom up: a call goes down this list or stays in its layer. */
export const LAYER_ORDER = ['base', 'trace', 'image', 'core', 'system', 'host'];
/** Cache line size in bytes: the ruler's row. */
export const LINE = 64;
/** Which cost a type's record stands for. */
const COST_OF_TYPE = { ta_actor: 'actor', ta_event: 'message', ta_timer: 'delayed send' };

/** The rank of a layer in LAYER_ORDER, or null for one outside it (compile). */
export function rank(layer) {
  const index = LAYER_ORDER.indexOf(layer);
  return index < 0 ? null : index;
}

/** A type's name without `struct`, `const`, pointers and array bounds: what it embeds. */
export function baseTypeName(typeName) {
  if (!typeName || typeName.includes('*')) return null;
  return typeName.replace(/\[[^\]]*\]/g, '').replace(/\b(struct|union|const|volatile)\b/g, '').trim();
}

/** Indexes the atlas: names to records, callers from calls, containment. */
export function load(data) {
  const types = (data.types ?? []).map((type, index) => ({ ...type, index, kind: 'type', path: data.files?.[type.file] ?? null }));
  const functions = (data.functions ?? []).map((fn, index) => ({
    ...fn, index, kind: 'function', path: data.files?.[fn.file] ?? null,
    calls: fn.calls ?? [], inlines: fn.inlines ?? [], callers: [], inlinedBy: [],
  }));
  for (const fn of functions) {
    for (const callee of fn.calls) functions[callee]?.callers.push(fn.index);
    for (const callee of fn.inlines) functions[callee]?.inlinedBy.push(fn.index);
  }
  const typeByName = new Map(types.map((type) => [type.name, type]));
  const functionByName = new Map(functions.map((fn) => [fn.name, fn]));
  const containedIn = new Map();
  for (const type of types) {
    for (const [member, offset, , typeName] of type.members ?? []) {
      const inner = baseTypeName(typeName);
      if (!inner || inner === type.name || !typeByName.has(inner)) continue;
      if (!containedIn.has(inner)) containedIn.set(inner, []);
      containedIn.get(inner).push({ type: type.name, member, offset });
    }
  }
  const layers = data.layers?.length ? [...data.layers]
    : [...new Set([...types, ...functions].map((item) => item.layer))];
  return { meta: { commit: data.commit, date: data.date, build: data.build }, layers, types, functions, typeByName, functionByName, containedIn, costs: data.costs ?? {} };
}

/** Per layer: how many types and functions, in the atlas's layer order. */
export function layerCounts(atlas) {
  return atlas.layers.map((layer) => ({
    layer,
    types: atlas.types.filter((type) => type.layer === layer).length,
    functions: atlas.functions.filter((fn) => fn.layer === layer).length,
  }));
}

/** What a layer holds, each list sorted by name. */
export function layerContents(atlas, layer) {
  const byName = (a, b) => (a.name < b.name ? -1 : a.name > b.name ? 1 : 0);
  return {
    types: atlas.types.filter((type) => type.layer === layer).sort(byName),
    functions: atlas.functions.filter((fn) => fn.layer === layer).sort(byName),
  };
}

/**
 * Types, functions and fields (`type.field`) whose name starts with or
 * contains the query, case-insensitive: prefixes first, then shorter names.
 */
export function search(atlas, query, limit = 40) {
  const q = query.trim().toLowerCase();
  if (!q) return [];
  const hits = [];
  const consider = (item, name) => {
    const lower = name.toLowerCase();
    const at = lower.indexOf(q);
    if (at < 0) return;
    hits.push({ ...item, name, score: (at === 0 ? 0 : 1) * 1000 + name.length });
  };
  for (const type of atlas.types) {
    consider({ kind: 'type', target: type.name, layer: type.layer }, type.name);
    if (q.includes('.') || q.length >= 3) {
      for (const [member] of type.members ?? []) consider({ kind: 'field', target: type.name, member, layer: type.layer }, `${type.name}.${member}`);
    }
  }
  for (const fn of atlas.functions) consider({ kind: 'function', target: fn.name, layer: fn.layer }, fn.name);
  hits.sort((a, b) => a.score - b.score || (a.name < b.name ? -1 : 1));
  return hits.slice(0, limit);
}

/**
 * A type's layout as a list of spans in offset order: its members and the
 * holes between them (padding), the tail padding included.
 */
export function layoutSpans(atlas, type) {
  const members = [...(type.members ?? [])]
    .map(([name, offset, size, typeName, bits]) => ({ kind: 'member', name, offset, size, typeName, bits: bits ?? null }))
    .sort((a, b) => a.offset - b.offset || b.size - a.size);
  const spans = [];
  let at = 0;
  for (const member of members) {
    if (member.offset > at) spans.push({ kind: 'padding', name: 'padding', offset: at, size: member.offset - at });
    const inner = baseTypeName(member.typeName);
    spans.push({ ...member, described: inner && inner !== type.name && atlas.typeByName.has(inner) ? inner : null });
    at = Math.max(at, member.offset + member.size);
  }
  if (type.size > at) spans.push({ kind: 'padding', name: 'padding', offset: at, size: type.size - at });
  // A union's members and bit-fields share bytes: each overlapping span
  // takes the first lane free at its offset, so none hides another.
  const ends = [];
  for (const span of spans) {
    let lane = ends.findIndex((end) => end <= span.offset);
    if (lane < 0) lane = ends.length;
    ends[lane] = span.offset + Math.max(span.size, 1);
    span.lane = lane;
  }
  return spans;
}

/**
 * The layout on cache lines: one row per 64 bytes, each a list of pieces
 * {span, start, end} with start and end within the row (0..64). A span that
 * crosses a line is cut at it.
 */
export function layoutRows(atlas, type) {
  const spans = layoutSpans(atlas, type);
  const rows = [];
  const count = Math.max(1, Math.ceil(type.size / LINE));
  for (let line = 0; line < count; line++) rows.push({ line, offset: line * LINE, pieces: [], lanes: 1 });
  for (const span of spans) {
    for (let from = span.offset; from < span.offset + span.size;) {
      const line = Math.floor(from / LINE);
      const to = Math.min(span.offset + span.size, (line + 1) * LINE);
      const row = rows[line];
      if (row) {
        row.pieces.push({ span, start: from - line * LINE, end: to - line * LINE, first: from === span.offset });
        row.lanes = Math.max(row.lanes ?? 1, span.lane + 1);
      }
      from = to;
    }
  }
  return rows;
}

/** Padding bytes in a type: the holes and the tail. */
export function paddingBytes(atlas, type) {
  return layoutSpans(atlas, type).filter((span) => span.kind === 'padding').reduce((sum, span) => sum + span.size, 0);
}

/** The direction of a call from one layer to another: down, same, up, or across (a layer outside the order). */
export function direction(from, to) {
  if (from === to) return 'same';
  const a = rank(from);
  const b = rank(to);
  if (a === null || b === null) return 'across';
  return b < a ? 'down' : 'up';
}

/** A function's neighborhood: who calls it, what it calls and inlines, each with the call's direction. */
export function neighborhood(atlas, fn) {
  const byName = (a, b) => (a.name < b.name ? -1 : a.name > b.name ? 1 : 0);
  const inlined = new Set(fn.inlines);
  const calleeIndexes = [...new Set([...fn.calls, ...fn.inlines])];
  const callees = calleeIndexes.map((index) => atlas.functions[index]).filter(Boolean)
    .map((callee) => ({ fn: callee, inlined: inlined.has(callee.index), direction: direction(fn.layer, callee.layer) })).sort((a, b) => byName(a.fn, b.fn));
  const callerIndexes = [...new Set([...fn.callers, ...fn.inlinedBy])];
  const inlinedBy = new Set(fn.inlinedBy);
  const callers = callerIndexes.map((index) => atlas.functions[index]).filter(Boolean)
    .map((caller) => ({ fn: caller, inlined: inlinedBy.has(caller.index), direction: direction(caller.layer, fn.layer) })).sort((a, b) => byName(a.fn, b.fn));
  return { callers, callees };
}

/** Calls between layers: one entry per (from, to) pair of different layers, with its count and direction. */
export function crossLayerCalls(atlas) {
  const counts = new Map();
  for (const fn of atlas.functions) {
    for (const index of new Set([...fn.calls, ...fn.inlines])) {
      const callee = atlas.functions[index];
      if (!callee || callee.layer === fn.layer) continue;
      const key = `${fn.layer}\u0000${callee.layer}`;
      counts.set(key, (counts.get(key) ?? 0) + 1);
    }
  }
  return [...counts].map(([key, count]) => {
    const [from, to] = key.split('\u0000');
    return { from, to, count, direction: direction(from, to) };
  }).sort((a, b) => (a.from + a.to < b.from + b.to ? -1 : 1));
}

/** What one of something costs in the region: its parts and their sum. */
export function costBreakdown(atlas, name) {
  const parts = atlas.costs[name];
  if (!parts) return null;
  return { name, parts: parts.map(([part, bytes]) => ({ part, bytes })), total: parts.reduce((sum, [, bytes]) => sum + bytes, 0) };
}

/** The cost a type's record stands for (an actor for ta_actor), or one that names the type as a part. */
export function costOfType(atlas, type) {
  const named = COST_OF_TYPE[type.name];
  if (named && atlas.costs[named]) return costBreakdown(atlas, named);
  for (const name of Object.keys(atlas.costs)) {
    if (atlas.costs[name].some(([part]) => part === type.name)) return costBreakdown(atlas, name);
  }
  return null;
}

/** The source link of a type or a function, on GitHub. */
export function sourceURL(item) {
  if (!item.path) return null;
  return `https://github.com/tinyactors-dev/tinyactors/blob/main/tinyactors-core/${item.path}${item.line ? `#L${item.line}` : ''}`;
}
