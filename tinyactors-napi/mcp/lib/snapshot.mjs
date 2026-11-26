// Copied from website/examples/inspector/decode.mjs, which the Inspector
// page uses, until core reads snapshots itself (ROADMAP.md, "Requests to
// core"): keep the two the same. test/mcp.test.mjs decodes a snapshot of
// this build with it, so a layout change that breaks it fails the tests.

// The inspector's decoder: a snapshot pack (format 4) into plain objects.
// The pack describes its own region layout (a CBOR part written by the
// build that made it), so everything below reads records through that
// description: member names are the only thing known in advance. Its
// "strings" part says how a string member (an actor's name, an event's
// name and sendid, ...) reads as text from the region and the segment's
// image, which the pack holds too. Its "image" part says how the image's
// sections and records read, so every chart decodes (states, transitions,
// data); its "instance" part where an actor's active configuration lies in
// its instance; its "trace" part how the trace records the region's journal
// keeps read, which become spans. Pure: no DOM and no file system, so
// main.mjs and page.mjs decode the same bytes.

// ---- the pack's fixed header (tinyactors-core/src/host/host.c,
// ta_host_snapshot_pack). Every offset is here, so a change of format is
// one edit.
export const PACK = {
  magic: 'tinyactors-snapshot ',
  magicBytes: 24,
  format: 4,
  systemIDLength: 24, // u32
  datamodelLength: 28, // u32
  time: 32, // u64, host ticks
  hostLength: 40, // u64
  stateLength: 48, // u64
  imageCount: 56, // u32, then 4 bytes of zero
  layoutLength: 64, // u64
  layoutID: 72, // u64
  fixed: 80, // the image lengths follow, a u64 each
};

// The host snapshot inside the pack (host.c: snapshot_header,
// snapshot_segment): little-endian, "TAHS".
const HOST = {
  magic: 0x53484154,
  header: 72, // ten u32 and four u64
  segmentFixed: 24, // u32 live, u32 pad, u64 hash, u64 region_length; then the config
};

/** Actor states (tinyactors-core/include/tinyactors/system.h, TA_ACTOR_*). */
export const ACTOR_STATES = ['free', 'idle', 'runnable', 'waiting for a workspace', 'waiting for an ack', 'done', 'hibernated'];
export const stateName = (state) => ACTOR_STATES[state] ?? `state ${state}`;

const SCALARS = { u8: 1, u16: 2, u32: 4, u64: 8, i8: 1, i16: 2, i32: 4, i64: 8 };
const align8 = (n) => (n + 7) & ~7;
const text = (bytes) => new TextDecoder().decode(bytes);
/** A u64 as a number when it fits exactly, else a BigInt. */
const narrow = (big) => (big <= BigInt(Number.MAX_SAFE_INTEGER) && big >= -BigInt(Number.MAX_SAFE_INTEGER) ? Number(big) : big);

export class SnapshotError extends Error {
  constructor(kind, message) {
    super(message);
    this.kind = kind;
  }
}

// ---- CBOR (RFC 8949), the subset the layout uses and a little more
export function decodeCBOR(bytes) {
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  let at = 0;
  const need = (n) => { if (at + n > bytes.length) throw new SnapshotError('cbor', 'The layout description ends early.'); };
  const argument = (info) => {
    if (info < 24) return info;
    if (info === 24) { need(1); return view.getUint8(at++); }
    if (info === 25) { need(2); const v = view.getUint16(at); at += 2; return v; }
    if (info === 26) { need(4); const v = view.getUint32(at); at += 4; return v; }
    if (info === 27) { need(8); const v = view.getBigUint64(at); at += 8; return narrow(v); }
    throw new SnapshotError('cbor', `The layout description uses CBOR's additional information ${info}, which the inspector does not read.`);
  };
  const item = (depth) => {
    if (depth > 64) throw new SnapshotError('cbor', 'The layout description nests too deep.');
    need(1);
    const initial = view.getUint8(at++);
    const major = initial >> 5;
    const info = initial & 31;
    if (major === 7) {
      if (info === 20) return false;
      if (info === 21) return true;
      if (info === 22) return null;
      if (info === 23) return undefined;
      if (info === 25) { need(2); const half = view.getUint16(at); at += 2; return halfFloat(half); }
      if (info === 26) { need(4); const v = view.getFloat32(at); at += 4; return v; }
      if (info === 27) { need(8); const v = view.getFloat64(at); at += 8; return v; }
      throw new SnapshotError('cbor', `Unknown CBOR simple value ${info}.`);
    }
    if (info === 31) throw new SnapshotError('cbor', 'The layout description uses indefinite lengths.');
    const n = argument(info);
    switch (major) {
      case 0: return n;
      case 1: return typeof n === 'bigint' ? -1n - n : -1 - n;
      case 2: need(n); at += n; return bytes.slice(at - n, at);
      case 3: need(n); at += n; return text(bytes.subarray(at - n, at));
      case 4: { const out = []; for (let i = 0; i < n; i++) out.push(item(depth + 1)); return out; }
      case 5: { const out = {}; for (let i = 0; i < n; i++) { const key = item(depth + 1); out[String(key)] = item(depth + 1); } return out; }
      case 6: return item(depth + 1); // a tag: its content
      default: throw new SnapshotError('cbor', `Unknown CBOR major type ${major}.`);
    }
  };
  const value = item(0);
  return value;
}
function halfFloat(half) {
  const exponent = (half >> 10) & 31;
  const fraction = half & 1023;
  const sign = half & 0x8000 ? -1 : 1;
  if (exponent === 0) return sign * 2 ** -14 * (fraction / 1024);
  if (exponent === 31) return fraction ? NaN : sign * Infinity;
  return sign * 2 ** (exponent - 15) * (1 + fraction / 1024);
}

// ---- the layout: types by name, and a generic record reader
export function indexLayout(layout) {
  if (!layout || !Array.isArray(layout.types)) throw new SnapshotError('layout', 'The layout description has no types.');
  const types = new Map(layout.types.map((type) => [type.name, type]));
  const little = layout.byteOrder !== 'big';
  const nil = layout.nil ?? 0xffffffff;
  return { ...layout, typeMap: types, little, nil, pointerBytes: layout.pointerBytes ?? 8 };
}

/** The bytes one element of `type` takes: a scalar's, a pointer's, or a described type's size. */
export function elementSize(layout, type) {
  if (SCALARS[type]) return SCALARS[type];
  if (type === 'pointer') return layout.pointerBytes;
  return layout.typeMap.get(type)?.size ?? null;
}

function readScalar(layout, view, offset, type) {
  const little = layout.little;
  switch (type) {
    case 'u8': return view.getUint8(offset);
    case 'i8': return view.getInt8(offset);
    case 'u16': return view.getUint16(offset, little);
    case 'i16': return view.getInt16(offset, little);
    case 'u32': return view.getUint32(offset, little);
    case 'i32': return view.getInt32(offset, little);
    case 'u64': return narrow(view.getBigUint64(offset, little));
    case 'i64': return narrow(view.getBigInt64(offset, little));
    default: return undefined;
  }
}

/** One value of `type` at `offset`: a number, a nested object for a described type, or null where it cannot be read. */
function readValue(layout, view, offset, type, size, depth) {
  if (offset < 0 || offset + (size ?? 0) > view.byteLength) return null;
  if (SCALARS[type]) return readScalar(layout, view, offset, type);
  if (type === 'pointer') return layout.pointerBytes === 8 ? narrow(view.getBigUint64(offset, layout.little)) : view.getUint32(offset, layout.little);
  if (layout.typeMap.has(type) && depth < 8) return readStruct(layout, view, offset, type, depth + 1);
  return { bytes: size };
}

/** A described struct at `offset` as an object of its members (arrays as arrays). */
export function readStruct(layout, view, offset, typeName, depth = 0) {
  const type = layout.typeMap.get(typeName);
  if (!type) return null;
  const out = {};
  for (const member of type.members ?? []) {
    const count = member.count ?? 1;
    const each = count > 1 ? member.size / count : member.size;
    if (count > 1) {
      const values = [];
      for (let i = 0; i < count && i < 4096; i++) values.push(readValue(layout, view, offset + member.offset + i * each, member.type, each, depth));
      out[member.name] = values;
    } else {
      out[member.name] = readValue(layout, view, offset + member.offset, member.type, member.size, depth);
    }
  }
  return out;
}

/**
 * A record's fields as rows for a table: name (dotted inside a nested
 * struct), offset from the record's start, size, type, kind and value.
 * Arrays of scalars are one row with every value.
 */
export function fieldRows(layout, view, offset, typeName, prefix = '', base = 0, depth = 0) {
  const type = layout.typeMap.get(typeName);
  if (!type) return [];
  const rows = [];
  for (const member of type.members ?? []) {
    const name = prefix ? `${prefix}.${member.name}` : member.name;
    const at = base + member.offset;
    const count = member.count ?? 1;
    if (layout.typeMap.has(member.type) && count === 1 && depth < 3) {
      rows.push(...fieldRows(layout, view, offset + member.offset, member.type, name, at, depth + 1));
      continue;
    }
    const each = count > 1 ? member.size / count : member.size;
    let value;
    if (count > 1) {
      const values = [];
      for (let i = 0; i < count && i < 16; i++) values.push(readValue(layout, view, offset + member.offset + i * each, member.type, each, 8));
      value = SCALARS[member.type] ? values : `${count} × ${member.type}`;
    } else {
      value = readValue(layout, view, offset + member.offset, member.type, member.size, 8);
    }
    rows.push({ name, offset: at, size: member.size, type: member.type, kind: member.kind ?? null, count, value });
  }
  return rows;
}

const memberOf = (layout, typeName, name) => layout.typeMap.get(typeName)?.members?.find((member) => member.name === name) ?? null;
/** A dotted path's member: its offset from the type's start and the member. */
function pathOf(layout, typeName, path) {
  let offset = 0;
  let type = typeName;
  let member = null;
  for (const part of path.split('.')) {
    member = memberOf(layout, type, part);
    if (!member) return null;
    offset += member.offset;
    type = member.type;
  }
  return { offset, member };
}
const valueAt = (object, path) => path.split('.').reduce((value, part) => (value == null ? undefined : value[part]), object);

// ---- the pack
/** What a file is before it is decoded: a pack of this format, an older pack, or something else. */
export function identify(bytes) {
  const head = text(bytes.subarray(0, Math.min(bytes.length, PACK.magicBytes)));
  if (!head.startsWith(PACK.magic)) return { kind: 'other' };
  const format = Number.parseInt(head.slice(PACK.magic.length), 10);
  return { kind: format === PACK.format ? 'pack' : 'older', format };
}

/** Decodes a whole pack: its header, its layout and each segment's region. */
export function decodePack(input) {
  const bytes = input instanceof Uint8Array ? input : new Uint8Array(input);
  const found = identify(bytes);
  if (found.kind === 'other') throw new SnapshotError('not-a-pack', 'This file is not a tinyactors snapshot: it does not start with “tinyactors-snapshot”.');
  if (found.kind === 'older') throw new SnapshotError('older', `This is a format ${found.format} snapshot, which does not describe its memory layout.`);
  if (bytes.length < PACK.fixed) throw new SnapshotError('short', 'The snapshot ends inside its header.');
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const u32 = (at) => view.getUint32(at, true);
  const u64 = (at) => narrow(view.getBigUint64(at, true));
  const count = u32(PACK.imageCount);
  if (PACK.fixed + 8 * count > bytes.length) throw new SnapshotError('short', 'The snapshot ends inside its list of images.');
  const lengths = { systemID: u32(PACK.systemIDLength), datamodel: u32(PACK.datamodelLength), layout: u64(PACK.layoutLength), host: u64(PACK.hostLength), state: u64(PACK.stateLength) };
  const images = [];
  for (let i = 0; i < count; i++) images.push(u64(PACK.fixed + 8 * i));
  let at = align8(PACK.fixed + 8 * count);
  const part = (length, what) => {
    if (typeof length !== 'number' || at + length > bytes.length) throw new SnapshotError('short', `The snapshot ends inside its ${what}.`);
    const out = bytes.subarray(at, at + length);
    const offset = at;
    at += align8(length);
    return { bytes: out, offset };
  };
  const systemID = text(part(lengths.systemID, 'system id').bytes);
  const datamodel = text(part(lengths.datamodel, 'datamodel name').bytes);
  const layoutPart = part(lengths.layout, 'layout');
  const hostPart = part(lengths.host, 'host snapshot');
  const statePart = part(lengths.state, 'datamodel state');
  const imageParts = images.map((length, i) => ({ index: i, length, ...part(length, `image ${i}`) }));
  const layout = indexLayout(decodeCBOR(layoutPart.bytes));
  // segment i's image is the pack's image i
  const host = decodeHost(layout, hostPart.bytes, imageParts.map((image) => image.bytes));
  return {
    format: PACK.format, bytes: bytes.length, systemID, datamodel, time: u64(PACK.time), layoutID: view.getBigUint64(PACK.layoutID, true),
    library: layout.library ?? null, layout,
    parts: [
      { name: 'header', offset: 0, bytes: align8(PACK.fixed + 8 * count) },
      { name: 'layout', offset: layoutPart.offset, bytes: lengths.layout },
      { name: 'host snapshot', offset: hostPart.offset, bytes: lengths.host },
      { name: 'datamodel state', offset: statePart.offset, bytes: lengths.state },
      ...imageParts.map((image) => ({ name: `image ${image.index}`, offset: image.offset, bytes: image.length })),
    ],
    host: host.header, segments: host.segments,
    traces: packTraces(host.segments),
  };
}

/** Every live segment's spans as one list, null when no segment traces. */
function packTraces(segments) {
  const traced = segments.filter((segment) => segment.live && segment.traces);
  if (!traced.length) return null;
  const spans = traced.flatMap((segment) => segment.traces.spans);
  const sum = (key) => traced.reduce((total, segment) => total + segment.traces.retained[key], 0);
  return {
    spans, traces: new Set(spans.map((span) => span.trace)).size,
    retained: {
      records: sum('records'), acknowledged: sum('acknowledged'), pending: sum('pending'), bytes: sum('bytes'), lost: sum('lost'),
      from: Math.min(...traced.map((segment) => segment.traces.retained.from ?? Infinity)),
      to: Math.max(...traced.map((segment) => segment.traces.retained.to ?? -Infinity)),
    },
  };
}

/** The host snapshot: its header and each segment's region, decoded; `images[i]` is segment i's image, for its strings. */
export function decodeHost(layout, bytes, images = []) {
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  if (bytes.length < HOST.header || view.getUint32(0, true) !== HOST.magic) throw new SnapshotError('host', 'The host snapshot inside the pack does not start with “TAHS”.');
  const u32 = (i) => view.getUint32(i * 4, true);
  const u64 = (i) => narrow(view.getBigUint64(40 + i * 8, true));
  const header = {
    version: u32(1), segmentCount: u32(2), movedCount: u32(3), roundRobin: u32(4), migrationSequence: u32(5),
    configSize: u32(6), retiredCharts: u32(7), identity: u32(8), now: u64(0), routed: u64(1), migrated: u64(2), retired: u64(3),
  };
  const configType = layout.typeMap.get('ta_system_config');
  if (configType && configType.size !== header.configSize) {
    throw new SnapshotError('host', `The host snapshot's segment config is ${header.configSize} bytes; the layout says ${configType.size}.`);
  }
  const segments = [];
  let at = HOST.header;
  for (let index = 0; index < header.segmentCount; index++) {
    if (at + HOST.segmentFixed + header.configSize > bytes.length) throw new SnapshotError('host', `The host snapshot ends inside segment ${index}.`);
    const live = view.getUint32(at, true) !== 0;
    const imageHash = view.getBigUint64(at + 8, true);
    const regionBytes = narrow(view.getBigUint64(at + 16, true));
    const config = configType ? readStruct(layout, view, at + HOST.segmentFixed, 'ta_system_config') : null;
    at += align8(HOST.segmentFixed + header.configSize);
    if (!live) { segments.push({ index, live, imageHash, regionBytes: 0, config }); continue; }
    if (at + regionBytes > bytes.length) throw new SnapshotError('host', `The host snapshot ends inside segment ${index}'s region.`);
    const region = bytes.subarray(at, at + regionBytes);
    at += align8(regionBytes);
    segments.push({ index, live, imageHash, regionBytes, config, ...decodeRegion(layout, region, images[index] ?? null) });
  }
  return { header, segments };
}

/**
 * A segment's region: the system's header record, its pools and arrays
 * (where each lies and how full it is), its actors with their queued
 * events and delayed sends, and the memory map. `image` (the segment's
 * image, from the pack) lets string members read as text.
 */
export function decodeRegion(layout, region, image = null) {
  const view = new DataView(region.buffer, region.byteOffset, region.byteLength);
  const systemType = layout.system ?? 'ta_system';
  const systemSize = layout.typeMap.get(systemType)?.size ?? 0;
  const system = readStruct(layout, view, 0, systemType);
  const resolve = stringResolver(layout, view, image);
  const decodedImage = image ? decodeImage(layout, image, resolve) : null;
  const charts = decodedImage?.charts ?? [];
  const poolType = 'ta_pool';
  const pools = [];
  for (const description of layout.pools ?? []) {
    const located = pathOf(layout, systemType, description.member ?? description.name);
    if (!located) continue;
    const record = readStruct(layout, view, located.offset, located.member.type ?? poolType) ?? {};
    const slots = typeof record.slots_relative === 'number' ? located.offset + record.slots_relative : null;
    const generations = typeof record.generations_relative === 'number' ? located.offset + record.generations_relative : null;
    pools.push({
      name: description.name, element: description.element, recordOffset: located.offset,
      capacity: record.capacity ?? 0, used: record.used ?? 0, highWater: record.high_water_mark ?? 0, stride: record.stride ?? 0, freeHead: record.free_head ?? null,
      offset: slots, bytes: (record.stride ?? 0) * (record.capacity ?? 0), generationsOffset: generations, generationsBytes: record.capacity ?? 0,
    });
  }
  const arrays = [];
  for (const description of layout.arrays ?? []) {
    const located = pathOf(layout, systemType, description.offset);
    if (!located) continue;
    const relative = valueAt(system, description.offset);
    const count = description.count ? valueAt(system, description.count) : 1;
    const size = elementSize(layout, description.element);
    if (typeof relative !== 'number' || typeof count !== 'number' || !count || size === null) continue;
    arrays.push({ name: description.name, element: description.element, offset: relative, count, stride: size, bytes: count * size });
  }
  const pool = (name) => pools.find((p) => p.name === name) ?? null;
  const live = (p, index) => p && p.generationsOffset !== null && index < p.highWater && index < p.capacity
    && p.generationsOffset + index < region.length && (view.getUint8(p.generationsOffset + index) & 1) === 1;
  const at = (p, index) => p.offset + index * p.stride;

  // Following an index field: the element type's first member that indexes
  // the same pool is its link (an event's `next`, a timer's `owner_next`).
  const chain = (poolName, start, limit) => {
    const p = pool(poolName);
    const link = p && layout.typeMap.get(p.element)?.members?.find((member) => member.kind === `index:${poolName}`);
    const out = [];
    const seen = new Set();
    for (let index = start; index !== layout.nil && index != null && out.length < limit;) {
      if (!p || index >= p.capacity || seen.has(index)) break;
      seen.add(index);
      out.push(index);
      if (!link) break;
      index = readScalar(layout, view, at(p, index) + link.offset, link.type);
    }
    return out;
  };

  // A pool's record with its strings: its raw fields, and `strings` for each string member that reads as text.
  const recordOf = (poolName, index) => {
    const p = pool(poolName);
    if (!p || p.offset === null || index >= p.capacity) return null;
    const record = readStruct(layout, view, at(p, index), p.element) ?? {};
    return { slot: index, ...record, strings: stringsOf(layout, p.element, record, resolve) };
  };
  // An event's name as text: its `name`, else the image's event name its `name_id` indexes.
  const withEventName = (event) => (event ? { ...event, eventName: event.strings.name ?? event.strings.name_id ?? null } : event);

  const actorsPool = pool('actors');
  const actors = [];
  const slots = [];
  if (actorsPool && actorsPool.offset !== null) {
    const element = actorsPool.element;
    const kindOf = (name) => memberOf(layout, element, name)?.kind ?? '';
    const poolOfKind = (kind) => (/^(index|handle):(.+)$/.exec(kind) ?? [])[2] ?? null;
    const mailboxPool = poolOfKind(kindOf('mailbox_head'));
    const timersPool = poolOfKind(kindOf('timers'));
    const timerEvent = timersPool && layout.typeMap.get(pool(timersPool)?.element)?.members?.find((member) => member.kind === `index:${mailboxPool}`);
    for (let index = 0; index < Math.min(actorsPool.highWater, actorsPool.capacity); index++) {
      if (!live(actorsPool, index)) { slots.push(null); continue; }
      const record = readStruct(layout, view, at(actorsPool, index), element) ?? {};
      const strings = stringsOf(layout, element, record, resolve);
      const mailbox = mailboxPool ? chain(mailboxPool, record.mailbox_head, 4096) : [];
      const timerSlots = timersPool ? chain(timersPool, record.timers, 4096) : [];
      const instance = instanceOf(layout, view, element, record, charts, decodedImage);
      const actor = {
        slot: index, session: record.serial ?? null, state: record.state ?? null,
        chart: instance.chart, chartName: charts[instance.chart]?.name ?? null,
        configuration: instance.configuration, history: instance.history,
        mailboxLength: record.mailbox_length ?? null,
        mailbox,
        timers: timersPool ? timerSlots.length : null,
        parent: handleSlot(record.parent), name: strings.name ?? null,
        record, strings,
        queued: mailbox.map((slot) => withEventName(recordOf(mailboxPool, slot))),
        delayed: timerSlots.map((slot) => {
          const timer = recordOf(timersPool, slot);
          const event = timer && timerEvent ? withEventName(recordOf(mailboxPool, timer[timerEvent.name])) : null;
          return { ...timer, event };
        }),
      };
      actors.push(actor);
      slots.push(actor.state);
    }
    // the slots never handed out yet, so the grid shows the whole pool
    for (let index = slots.length; index < Math.min(actorsPool.capacity, 1 << 22); index++) slots.push(null);
  }
  const traces = decodeTraces(layout, view, { system, systemType, resolve, charts, actors, pool });
  return { region, system, systemSize, pools, arrays, actors, slots, resolve, charts, imageSections: decodedImage?.sections ?? [], traces, memoryMap: memoryMap(region.length, systemType, systemSize, pools, arrays) };
}

// ---- the image: every chart, read through the description's "image"

/**
 * An image's sections and charts. Each chart: its index, name, datamodel,
 * source (the document it was compiled from: the image holds no SCXML
 * text), states (id, kind, parent, children, initial states, depth,
 * history), transitions (source, targets, events, condition, type) and
 * data ids, every index local to the chart. `resolve` reads its strings.
 */
export function decodeImage(layout, image, resolve) {
  const format = layout.image;
  if (!format || !image) return null;
  const imageLayout = { ...layout, little: format.byteOrder !== 'big' };
  const view = new DataView(image.buffer, image.byteOffset, image.byteLength);
  const head = layout.typeMap.get(format.header);
  const sectionType = layout.typeMap.get(format.section);
  if (!head || !sectionType || head.size > image.length) return null;
  const header = readStruct(imageLayout, view, 0, format.header);
  const kinds = new Map((format.sections ?? []).map((section) => [section.kind, section]));
  const sections = [];
  for (let i = 0; i < (header[format.sectionCount] ?? 0); i++) {
    const at = head.size + i * sectionType.size;
    if (at + sectionType.size > image.length) break;
    const record = readStruct(imageLayout, view, at, format.section);
    const known = kinds.get(record[format.kind]);
    sections.push({ kind: record[format.kind], name: known?.name ?? `section ${record[format.kind]}`, element: known?.element ?? 'bytes',
      offset: record[format.offset], bytes: record[format.size], count: record[format.count] });
  }
  const section = (name) => sections.find((s) => s.name === name) ?? null;
  const size = (s) => elementSize(layout, s.element) ?? 0;
  /** Record `index` of a section, or null where it does not lie. */
  const record = (name, index) => {
    const s = section(name);
    if (!s || typeof index !== 'number' || index < 0 || index >= s.count) return null;
    const at = s.offset + index * size(s);
    if (at + size(s) > image.length) return null;
    return SCALARS[s.element] ? readScalar(imageLayout, view, at, s.element) : readStruct(imageLayout, view, at, s.element);
  };
  const word = (index) => record('u32', index);
  const nil = layout.nil;
  const ranges = format.ranges ?? [];
  const valueNames = (type, member) => (format.names ?? []).find((n) => n.type === type && n.member === member)?.values ?? [];
  const flagBits = (type, member) => Object.fromEntries((format.flags ?? []).filter((f) => f.type === type && f.member === member).map((f) => [f.name, f.bit]));
  const text = (id) => (id === nil ? null : resolve.string(id));
  const eventName = (index) => (index === nil ? null : resolve.eventName(index));

  /** The u32s a "u32-pool" member names: an array of entries, each an array of its elements. */
  const list = (typeName, recordValue, memberName) => {
    const member = layout.typeMap.get(typeName)?.members?.find((m) => m.name === memberName && m.list);
    if (!member) return [];
    const count = recordValue[member.list.count] ?? 0;
    const width = member.list.elements.length;
    const out = [];
    for (let i = 0; i < count && i < 65536; i++) {
      const entry = [];
      for (let k = 0; k < width; k++) entry.push(word(recordValue[memberName] + i * width + k));
      out.push(entry);
    }
    return out;
  };

  const chartSection = section('charts');
  const stateKinds = valueNames('ta_state_record', 'kind');
  const expressionType = 'ta_expression_record';
  const transitionFlags = flagBits('ta_transition_record', 'flags');
  const charts = [];
  for (let index = 0; index < (chartSection?.count ?? 0); index++) {
    const chart = record('charts', index);
    const first = Object.fromEntries(ranges.map((r) => [r.section, chart[r.first]]));
    const count = Object.fromEntries(ranges.map((r) => [r.section, chart[r.count]]));
    const local = (name, i) => (i === nil || i >= (count[name] ?? 0) ? null : record(name, first[name] + i));
    const expression = (i) => {
      const e = local('expressions', i);
      return e ? text(e.source) : null;
    };
    const transitionRecords = [];
    for (let i = 0; i < (count.transitions ?? 0); i++) transitionRecords.push(local('transitions', i));
    const targetsOf = (t) => list('ta_transition_record', t, 'targets_offset').map(([state]) => state);
    const states = [];
    for (let i = 0; i < (count.states ?? 0); i++) {
      const s = local('states', i);
      const initial = s.initial !== nil && transitionRecords[s.initial] ? targetsOf(transitionRecords[s.initial]) : null;
      states.push({
        index: i, id: text(s.id), kind: stateKinds[s.kind] ?? `kind ${s.kind}`, parent: s.parent === nil ? null : s.parent,
        children: [], end: s.end, depth: s.depth, initial, historyOffset: s.history_offset === nil ? null : s.history_offset,
      });
    }
    for (const state of states) if (state.parent !== null && states[state.parent]) states[state.parent].children.push(state.index);
    const transitions = transitionRecords.map((t, i) => {
      const flags = t.flags ?? 0;
      const type = flags & (transitionFlags.initial ?? 0) ? 'initial' : flags & (transitionFlags.internal ?? 0) ? 'internal' : 'external';
      return {
        index: i, source: t.source, targets: targetsOf(t),
        events: list('ta_transition_record', t, 'events_offset').map(([lo]) => eventName(lo) || '*'),
        cond: t.condition === nil ? null : expression(t.condition), type,
      };
    });
    const data = [];
    for (let i = 0; i < (count.data ?? 0); i++) {
      const d = local('data', i);
      data.push({ index: i, id: text(d.id), state: d.state === nil ? null : d.state, expr: d.expression === nil ? null : expression(d.expression) });
    }
    const expressions = [];
    for (let i = 0; i < (count.expressions ?? 0); i++) {
      const e = local('expressions', i);
      expressions.push({ index: i, kind: valueNames(expressionType, 'kind')[e.kind] ?? e.kind, source: text(e.source), line: e.line, column: e.column });
    }
    charts.push({
      index, name: text(chart.name), datamodel: text(chart.datamodel), source: text(chart.source),
      binding: valueNames('ta_chart_record', 'binding')[chart.binding] ?? chart.binding,
      states, transitions, data, expressions, record: chart,
    });
  }
  return { header, sections, charts };
}

/** The words of a bit set from `at` (u32 words of the region) as the indices set, of at most `count`. */
function bitsAt(layout, view, at, count) {
  const out = [];
  for (let i = 0; i < count; i++) {
    const offset = at + (i >>> 5) * 4;
    if (offset + 4 > view.byteLength) break;
    if ((view.getUint32(offset, layout.little) >>> (i & 31)) & 1) out.push(i);
  }
  return out;
}

/**
 * An actor's chart and active configuration, through the description's
 * "instance": a resident actor's instance (a slab offset) holds its chart
 * and, after its header, the areas the chart's record sizes; a hibernated
 * actor's holds its chart, its configuration being in its blob (null here).
 * The configuration is the active states' indices in document order, the
 * root left out; `history` each recorded history state's states.
 */
export function instanceOf(layout, view, element, record, charts, image) {
  const description = layout.instance;
  const none = { chart: null, configuration: null, history: null };
  if (!description) return none;
  const member = layout.typeMap.get(element)?.members?.find((m) => m.name === description.member && kindHolds(m, record));
  if (!member) return none;
  const value = record[member.name];
  if (member.kind?.startsWith('global:')) return { chart: value, configuration: null, history: null };
  if (member.kind !== 'slab') return none;
  const systemType = layout.system ?? 'ta_system';
  const slab = pathOf(layout, systemType, layout.strings?.slab ?? 'slab');
  const slabRecord = slab ? readStruct(layout, view, slab.offset, slab.member.type) : null;
  const pagesMember = layout.strings?.pages ?? 'pages_relative';
  if (!slabRecord || typeof slabRecord[pagesMember] !== 'number') return none;
  const at = slab.offset + slabRecord[pagesMember] + value;
  const headerType = layout.typeMap.get(description.header);
  if (!headerType || at < 0 || at + headerType.size > view.byteLength) return none;
  const header = readStruct(layout, view, at, description.header);
  const chartMember = headerType.members.find((m) => m.kind === 'global:charts');
  const chartIndex = chartMember ? header[chartMember.name] : null;
  const chart = charts[chartIndex];
  if (!chart) return { chart: chartIndex, configuration: null, history: null };
  const wordBytes = SCALARS[description.word] ?? 4;
  const stateCount = chart.record[description.stateCount] ?? chart.states.length;
  const stateWords = Math.ceil(stateCount / (wordBytes * 8));
  const areas = {};
  let word = headerType.size / wordBytes;
  for (const area of description.areas ?? []) {
    const when = area.when;
    const present = !when || (when.nonzero ? (chart.record[when.member] ?? 0) !== 0 : (when.values ?? []).includes(chart.record[when.member]));
    const words = !present ? 0 : area.words === 'stateWords' ? stateWords : chart.record[area.words] ?? 0;
    areas[area.name] = { word, words };
    word += words;
  }
  const config = areas.config ? bitsAt(layout, view, at + areas.config.word * wordBytes, stateCount) : [];
  const configuration = config.filter((index) => chart.states[index]?.parent !== null);
  let history = null;
  if (areas.history_recorded?.words && areas.history?.words) {
    history = {};
    for (const h of bitsAt(layout, view, at + areas.history_recorded.word * wordBytes, stateCount)) {
      const state = chart.states[h];
      if (!state || state.historyOffset === null || state.parent === null) continue;
      const parent = chart.states[state.parent];
      const span = parent.end - state.parent - 1;
      history[h] = bitsAt(layout, view, at + (areas.history.word + state.historyOffset) * wordBytes, span).map((i) => state.parent + 1 + i);
    }
  }
  return { chart: chartIndex, configuration, history, instance: { offset: at, header, areas } };
}

// ---- traces: the records the region's journal keeps, as spans

const hex = (value, digits) => BigInt.asUintN(digits * 4, BigInt(value)).toString(16).padStart(digits, '0');

/**
 * The trace records the journal keeps (the description's "trace"), oldest
 * first: the acknowledged ones a host already exported, then the pending
 * ones; and the spans they make, in the shape <ta-trace-view> takes, named
 * and attributed as core's OTLP encoder does. Times are the system's clock
 * (ticks, as nanoseconds), each record placed after the ones before it in
 * the same tick by its fuel stamp. A dynamic string of an acknowledged
 * record may have been freed, so it is not read: an actor's name comes
 * from its record when it is still resident. Null when tracing is off.
 */
export function decodeTraces(layout, view, { system, systemType = 'ta_system', resolve, charts = [], actors = [] } = {}) {
  const description = layout.trace;
  if (!description || !system) return null;
  if (valueAt(system, description.tracing) === description.disabled) return null;
  const located = pathOf(layout, systemType, description.journal);
  if (!located) return null;
  const journal = readStruct(layout, view, located.offset, located.member.type);
  const storage = located.offset + journal[description.storage];
  const field = (name) => journal[description[name]];
  const recordType = layout.typeMap.get(description.record);
  const kinds = new Map((description.kinds ?? []).map((kind) => [kind.kind, kind]));
  const nil = layout.nil;
  const total = field('retained') + field('used');
  const records = [];
  let at = field('head');
  let walked = 0;
  while (recordType && walked < total) {
    if (field('wrapped') && at === field('end')) at = 0;
    if (at + recordType.size > field('capacity') || storage + at + recordType.size > view.byteLength) break;
    const header = readStruct(layout, view, storage + at, description.record);
    const size = header[description.size];
    if (!size || size % 8 || walked + size > total) break;
    const kind = kinds.get(header[description.kind]);
    const bodyAt = storage + at + recordType.size;
    const body = kind ? readStruct(layout, view, bodyAt, kind.body) ?? {} : {};
    const lists = {};
    let listAt = bodyAt + (kind ? layout.typeMap.get(kind.body)?.size ?? 0 : 0);
    for (const l of kind?.lists ?? []) {
      const values = [];
      for (let i = 0; i < (body[l.count] ?? 0) && listAt + 4 <= storage + at + size; i++, listAt += 4) values.push(view.getUint32(listAt, layout.little));
      lists[l.count] = values;
    }
    records.push({ offset: at, kind: kind?.name ?? `kind ${header[description.kind]}`, acknowledged: walked < field('retained'), header, body, lists });
    at += size;
    walked += size;
  }

  // text: an image string always; a dynamic one only while its record is pending
  const dynamic = layout.strings?.slabBit >>> 0;
  const textOf = (record, reference) => {
    if (typeof reference !== 'number' || reference === nil) return null;
    if (record.acknowledged && (reference & dynamic) >>> 0) return null;
    return resolve?.string(reference) ?? null;
  };
  const residentName = new Map(actors.map((actor) => [actor.slot, actor]));
  const actorName = (record, reference) => {
    const text = textOf(record, reference);
    if (text !== null) return text;
    const handle = record.header.actor;
    const actor = residentName.get(handleSlot(handle));
    return actor && actor.session === record.header.key?.session ? actor.name : null;
  };
  const stateID = (chart, index) => charts[chart]?.states[index]?.id ?? String(index);
  const transitionText = (chart, index) => {
    const t = charts[chart]?.transitions[index];
    if (!t) return String(index);
    return `${charts[chart].states[t.source]?.parent === null ? '' : stateID(chart, t.source)} → ${t.targets.map((s) => stateID(chart, s)).join(' ')}${t.events.length ? ` on ${t.events.join(' ')}` : ''}`;
  };

  // time: ticks as nanoseconds, records of one tick ordered by fuel
  const ticksPerSecond = system.config?.ticks_per_second || 1e9;
  const fuelBase = new Map();
  for (const r of records) {
    const now = r.header.now;
    const fuel = r.kind === 'microstep' ? Math.min(r.header.fuel, r.body.begin_fuel ?? r.header.fuel) : r.header.fuel;
    if (!fuelBase.has(now) || fuel < fuelBase.get(now)) fuelBase.set(now, fuel);
  }
  const time = (now, fuel) => Number(now) * (1e9 / ticksPerSecond) + Math.max(0, Number(fuel) - Number(fuelBase.get(now) ?? fuel));

  const spans = [];
  const byID = new Map();
  const events = new Map(); // span id -> events, attached once every span exists
  const failed = new Set();
  let lost = 0;
  const common = (record, nameReference) => {
    const h = record.header;
    const attributes = {
      'scxml.session_id': h.key.session, 'scxml.actor': `${(h.actor & 0xffffff) - 1}:${h.actor >>> 24}`,
      'scxml.definition': h.chart, 'scxml.macrostep': h.key.macrostep, 'scxml.time': h.now,
    };
    const name = actorName(record, nameReference);
    if (name !== null) attributes['tinyactors.actor.name'] = name;
    attributes['tinyactors.chart.name'] = charts[h.chart]?.name ?? null;
    return attributes;
  };
  const open = (record, name, attributes, kind, start = time(record.header.now, record.header.fuel)) => {
    const h = record.header;
    const span = {
      trace: hex(h.trace.high, 16) + hex(h.trace.low, 16), span: hex(h.span, 16), parent: h.parent ? hex(h.parent, 16) : null,
      name, kind: 1, start, end: null, status: 0, statusMessage: '', attributes: { ...attributes, 'tinyactors.span.kind': kind },
      events: [], acknowledged: record.acknowledged, sampled: (h.flags & (description.sampled ?? 1)) !== 0,
    };
    spans.push(span);
    byID.set(span.span, span);
    return span;
  };
  const states = (chart, list) => list.map((s) => stateID(chart, s));
  for (const record of records) {
    const { header: h, body: b, lists } = record;
    const t = time(h.now, h.fuel);
    switch (record.kind) {
      case 'dropped': lost += Number(b.count ?? 0); break;
      case 'spawned': case 'finished': case 'destroyed': case 'unloaded': case 'loaded': {
        const attributes = common(record, b.name);
        if (record.kind === 'finished' && b.final_state !== nil) attributes['scxml.final_state'] = stateID(h.chart, b.final_state);
        open(record, `scxml.${record.kind}`, attributes, record.kind).end = t + 1;
        break;
      }
      case 'macrostepBegin': {
        const attributes = common(record, b.actor_name);
        const event = b.name === nil ? null : textOf(record, b.name) ?? resolve?.eventName(b.name_id) ?? null;
        if (b.name !== nil) {
          attributes['scxml.event.name'] = event;
          attributes['scxml.event.type'] = description.eventTypes?.[b.type] ?? b.type;
          if (b.sendid !== nil) attributes['scxml.event.sendid'] = textOf(record, b.sendid);
          if (b.origin !== nil) attributes['scxml.event.origin'] = textOf(record, b.origin);
          if (b.origin_session) attributes['scxml.event.origin_session_id'] = b.origin_session;
        }
        if (b.cause?.session || b.cause?.macrostep) {
          attributes['scxml.cause.session_id'] = b.cause.session;
          attributes['scxml.cause.macrostep'] = b.cause.macrostep;
          attributes['scxml.cause.microstep'] = b.cause.microstep;
        }
        open(record, `scxml.macrostep ${b.name === nil ? 'bootstrap' : event ?? '(unread)'}`, attributes, 'macrostep');
        break;
      }
      case 'macrostepEnd': {
        const span = byID.get(hex(h.span, 16));
        if (!span) break; // its begin was dropped from the history
        span.end = Math.max(t, span.start + 1);
        span.attributes['scxml.state'] = states(h.chart, lists.state_count ?? []);
        span.attributes['scxml.state.id'] = lists.state_count ?? [];
        if (!record.acknowledged) span.acknowledged = false;
        break;
      }
      case 'microstep': {
        const macro = byID.get(hex(h.parent, 16));
        const attributes = macro ? { ...macro.attributes } : common(record, nil);
        for (const key of Object.keys(attributes)) if (key.startsWith('scxml.event') || key.startsWith('scxml.cause') || key.startsWith('scxml.state') || key === 'tinyactors.span.kind') delete attributes[key];
        attributes['scxml.microstep'] = h.key.microstep;
        const event = textOf(record, b.name);
        if (b.name !== nil && event !== null) attributes['scxml.event.name'] = event;
        const transitions = lists.transition_count ?? [];
        attributes['scxml.transition'] = transitions.map((i) => transitionText(h.chart, i));
        attributes['scxml.transition.id'] = transitions;
        attributes['scxml.state.exited'] = states(h.chart, lists.exited_count ?? []);
        attributes['scxml.state.exited.id'] = lists.exited_count ?? [];
        attributes['scxml.state.entered'] = states(h.chart, lists.entered_count ?? []);
        attributes['scxml.state.entered.id'] = lists.entered_count ?? [];
        const name = b.name === nil ? '(eventless)' : event ?? macro?.attributes['scxml.event.name'] ?? '(unread)';
        const span = open(record, `scxml.microstep ${name}`, attributes, 'microstep', time(h.now, b.begin_fuel));
        span.end = Math.max(t, span.start + 1);
        break;
      }
      default: {
        if (!kinds.has(h.kind)) break;
        const attributes = {};
        const s = (key, reference) => { const value = textOf(record, reference); if (value !== null) attributes[key] = value; };
        if (record.kind === 'condition') {
          attributes['scxml.transition'] = transitionText(h.chart, b.transition);
          attributes['scxml.transition.id'] = b.transition;
          attributes['scxml.result'] = b.result === 1;
        } else if (record.kind === 'raised') s('scxml.event.name', b.name);
        else if (record.kind === 'sent') {
          s('scxml.event.name', b.name); s('scxml.send.target', b.target); s('scxml.event.sendid', b.sendid);
          if (b.delay) attributes['scxml.send.delay_ns'] = b.delay;
        } else if (record.kind === 'invoked') {
          s('scxml.invoke.id', b.invokeid); s('scxml.invoke.type', b.type); s('scxml.invoke.src', b.source);
          if (b.child_session) attributes['scxml.invoke.session_id'] = b.child_session;
        } else if (record.kind === 'cancelled') s('scxml.event.sendid', b.sendid);
        else if (record.kind === 'assign') {
          const location = charts[h.chart]?.expressions[b.location]?.source;
          if (location) attributes['scxml.location'] = location;
        } else if (record.kind === 'log') { s('scxml.log.label', b.label); s('scxml.log.text', b.text); }
        else if (record.kind === 'actionFailed') attributes['scxml.action.opcode'] = b.opcode;
        const name = record.kind === 'actionFailed' ? 'scxml.action_failed' : `scxml.${record.kind}`;
        const target = [hex(h.span, 16), hex(h.parent, 16)];
        if (!events.has(target.join())) events.set(target.join(), []);
        events.get(target.join()).push({ name, time: t, attributes });
        if (record.kind === 'actionFailed') failed.add(target.join());
      }
    }
  }
  // a step's events go on its own span (a microstep's), else on its parent's
  for (const [key, list] of events) {
    const [own, parent] = key.split(',');
    const span = byID.get(own) ?? byID.get(parent);
    if (!span) continue;
    span.events.push(...list);
    if (failed.has(key)) span.status = 2;
  }
  // a span still open at capture: ended at the last record, marked incomplete
  const last = records.length ? Math.max(...records.map((r) => time(r.header.now, r.header.fuel))) : 0;
  for (const span of spans) {
    if (span.end !== null) continue;
    span.end = Math.max(last, span.start + 1);
    span.attributes['scxml.incomplete'] = true;
  }
  const nows = records.map((r) => r.header.now);
  return {
    spans, records,
    retained: {
      records: records.length, acknowledged: records.filter((r) => r.acknowledged).length, pending: records.filter((r) => !r.acknowledged).length,
      bytes: total, capacity: field('capacity'), lost: field('lost_total'), droppedInHistory: lost, written: field('written_total'),
      from: nows.length ? Math.min(...nows) : null, to: nows.length ? Math.max(...nows) : null,
    },
  };
}

// ---- strings: the description's "strings" says how a string member reads as text

/** Whether a member's kind holds for this record: always, or when its `when` member has one of its values. */
export function kindHolds(member, record) {
  if (!member.when) return true;
  return (member.when.values ?? []).includes(record?.[member.when.member]);
}

/**
 * Reads text through the description: `string(reference)` (a slab string
 * of the region or a string of the image) and `eventName(index)` (the
 * image's event names), each null for none or for what does not lie where
 * it should. Without an image only slab strings read.
 */
export function stringResolver(layout, view, image) {
  const description = layout.strings;
  const none = { string: () => null, eventName: () => null, text: () => null };
  if (!description) return none;
  const systemType = layout.system ?? 'ta_system';
  const slab = pathOf(layout, systemType, description.slab);
  const slabRecord = slab ? readStruct(layout, view, slab.offset, slab.member.type) : null;
  const pages = slabRecord && typeof slabRecord[description.pages] === 'number' ? slab.offset + slabRecord[description.pages] : null;
  const header = layout.typeMap.get(description.header);
  const lengthMember = memberOf(layout, description.header, description.length);
  const decoder = new TextDecoder('utf-8', { fatal: true });
  const textAt = (bytes) => { try { return decoder.decode(bytes); } catch { return null; } };
  const slabBit = description.slabBit >>> 0;
  const scale = 2 ** description.slabShift;

  const slabString = (reference) => {
    if (pages === null || !header || !lengthMember) return null;
    const at = pages + ((reference & ~slabBit) >>> 0) * scale;
    if (at < 0 || at + header.size > view.byteLength) return null;
    const length = readScalar(layout, view, at + lengthMember.offset, lengthMember.type);
    const start = at + header.size;
    if (typeof length !== 'number' || start + length > view.byteLength) return null;
    return textAt(new Uint8Array(view.buffer, view.byteOffset + start, length));
  };

  // the image: its header, its sections, its strings and event names
  const format = description.image;
  let imageString = () => null;
  let eventName = () => null;
  if (image && format) {
    const imageLayout = { ...layout, little: format.byteOrder !== 'big' };
    const imageView = new DataView(image.buffer, image.byteOffset, image.byteLength);
    const head = layout.typeMap.get(format.header);
    const sectionType = layout.typeMap.get(format.section);
    const record = head && head.size <= image.length ? readStruct(imageLayout, imageView, 0, format.header) : null;
    const sections = [];
    for (let i = 0; record && sectionType && i < (record[format.sectionCount] ?? 0); i++) {
      const at = head.size + i * sectionType.size;
      if (at + sectionType.size > image.length) break;
      sections.push(readStruct(imageLayout, imageView, at, format.section));
    }
    const section = (kind) => sections.find((s) => s[format.kind] === kind) ?? null;
    const strings = section(format.strings);
    const names = section(format.eventNames);
    const lengthSize = SCALARS[format.length] ?? 4;
    imageString = (id) => {
      if (!strings || typeof id !== 'number' || id + lengthSize > strings[format.size]) return null;
      const at = strings[format.offset] + id;
      const length = readScalar(imageLayout, imageView, at, format.length);
      if (typeof length !== 'number' || id + lengthSize + length > strings[format.size]) return null;
      return textAt(image.subarray(at + lengthSize, at + lengthSize + length));
    };
    const nameType = layout.typeMap.get(format.eventName);
    const nameMember = memberOf(layout, format.eventName, format.name);
    eventName = (index) => {
      if (!names || !nameType || !nameMember || typeof index !== 'number' || index >= names[format.count]) return null;
      const at = names[format.offset] + index * nameType.size;
      if (at + nameType.size > image.length) return null;
      return imageString(readScalar(imageLayout, imageView, at + nameMember.offset, nameMember.type));
    };
  }
  const string = (reference) => {
    if (typeof reference !== 'number' || reference === description.none) return null;
    return (reference & slabBit) >>> 0 ? slabString(reference) : imageString(reference);
  };
  /** A member's value as text, by its kind: null when it is none or not a string. */
  const text = (kind, value) => (kind === 'string' ? string(value) : kind === 'event-name' ? eventName(value) : null);
  return { string, eventName, text };
}

/** A record's string members as text: { member: text } for each that reads. */
export function stringsOf(layout, typeName, record, resolve) {
  const out = {};
  for (const member of layout.typeMap.get(typeName)?.members ?? []) {
    if ((member.kind !== 'string' && member.kind !== 'event-name') || !kindHolds(member, record)) continue;
    const value = resolve.text(member.kind, record?.[member.name]);
    if (value !== null) out[member.name] = value;
  }
  return out;
}

/**
 * One record of a segment's region, field by field: each field's name
 * (dotted inside a nested struct), offset from the record's start, size,
 * type, kind (none where its `when` does not hold), raw value, and, where
 * they apply, its text (a string member) and its link (an index or handle:
 * the pool, the slot and its offset in the region; a relative offset: the
 * offset in the region).
 */
export function decodeRecord(snapshot, segment, typeName, offset) {
  const found = typeof segment === 'number' ? snapshot.segments[segment] : segment;
  if (!found?.region) return { fields: [] };
  const layout = snapshot.layout;
  const view = new DataView(found.region.buffer, found.region.byteOffset, found.region.byteLength);
  const resolve = found.resolve ?? stringResolver(layout, view, null);
  const pools = new Map((found.pools ?? []).map((p) => [p.name, p]));
  const fields = [];
  const walk = (type, at, prefix, base, depth) => {
    const description = layout.typeMap.get(type);
    if (!description) return;
    const record = readStruct(layout, view, at, type, 8);
    for (const member of description.members ?? []) {
      const name = prefix ? `${prefix}.${member.name}` : member.name;
      const count = member.count ?? 1;
      if (layout.typeMap.has(member.type) && count === 1 && depth < 3) {
        walk(member.type, at + member.offset, name, base + member.offset, depth + 1);
        continue;
      }
      // a member described once per kind (an actor's instance): the one whose kind holds
      const twins = description.members.filter((other) => other.name === member.name);
      if (twins.length > 1 && (!kindHolds(member, record) || twins.find((other) => kindHolds(other, record)) !== member)) continue;
      const kind = member.kind && kindHolds(member, record) ? member.kind : null;
      const value = record?.[member.name] ?? null;
      const field = { name, offset: base + member.offset, size: member.size, type: member.type, kind, value };
      const text = kind ? resolve.text(kind, value) : null;
      if (text !== null) field.text = text;
      const link = linkOf(layout, pools, kind, value, at);
      if (link) field.link = link;
      fields.push(field);
    }
  };
  walk(typeName, offset, '', 0, 0);
  return { fields };
}

function linkOf(layout, pools, kind, value, recordOffset) {
  if (!kind || typeof value !== 'number') return null;
  if (kind === 'relative') return { offset: recordOffset + value };
  const match = /^(index|handle):(.+)$/.exec(kind);
  if (!match) return null;
  const slot = match[1] === 'handle' ? handleSlot(value) : value === layout.nil ? null : value;
  const p = pools.get(match[2]);
  if (slot === null || !p || slot >= p.capacity) return null;
  return { pool: match[2], slot, offset: p.offset + slot * p.stride };
}

/** A handle (generation:8 | index+1:24) as its slot, null for 0. */
export function handleSlot(handle) {
  if (typeof handle !== 'number' || handle === 0) return null;
  return (handle & 0xffffff) - 1;
}

/** The region's bands in offset order, gaps "not described" (the slab, the heap, the journal: no pool or array names them). */
export function memoryMap(length, systemType, systemSize, pools, arrays) {
  const bands = [{ name: systemType, offset: 0, bytes: systemSize, kind: 'header' }];
  for (const p of pools) {
    if (p.offset !== null && p.bytes) bands.push({ name: p.name, offset: p.offset, bytes: p.bytes, kind: 'pool' });
    if (p.generationsOffset !== null && p.generationsBytes) bands.push({ name: `${p.name} generations`, offset: p.generationsOffset, bytes: p.generationsBytes, kind: 'generations' });
  }
  for (const a of arrays) bands.push({ name: a.name, offset: a.offset, bytes: a.bytes, kind: 'array' });
  const sorted = bands.filter((band) => band.offset >= 0 && band.offset < length).sort((a, b) => a.offset - b.offset || b.bytes - a.bytes);
  const out = [];
  let at = 0;
  for (const band of sorted) {
    if (band.offset > at) out.push({ name: 'not described', offset: at, bytes: band.offset - at, kind: 'gap' });
    if (band.offset + band.bytes <= at) continue; // inside one before it
    out.push(band);
    at = band.offset + band.bytes;
  }
  if (length > at) out.push({ name: 'not described', offset: at, bytes: length - at, kind: 'gap' });
  return out;
}

/** An actor's record, field by field, with its offset in the region. */
export function actorRecord(layout, region, pool, slot) {
  const view = new DataView(region.buffer, region.byteOffset, region.byteLength);
  const offset = pool.offset + slot * pool.stride;
  return { offset, rows: fieldRows(layout, view, offset, pool.element) };
}

/** A pack's summary as lines of text: what main.mjs prints, the same on every run. */
export function summarize(pack, name = 'snapshot') {
  const lines = [];
  lines.push(`${name}: format ${pack.format}, ${pack.bytes} bytes`);
  lines.push(`system ${pack.systemID}, datamodel ${pack.datamodel || 'none'}, library ${pack.library ?? 'unknown'}, layout ${pack.layoutID.toString(16).padStart(16, '0')}, time ${pack.time}`);
  for (const segment of pack.segments) {
    if (!segment.live) { lines.push(`segment ${segment.index}: retired`); continue; }
    lines.push(`segment ${segment.index}: ${segment.regionBytes} bytes, ${segment.actors.length} actors`);
    for (const p of segment.pools) lines.push(`  pool ${p.name}: ${p.used} used of ${p.capacity}, high water ${p.highWater}, ${p.stride} bytes a slot`);
    const actors = [...segment.actors].sort((a, b) => (a.session < b.session ? -1 : a.session > b.session ? 1 : 0));
    for (const chart of segment.charts ?? []) {
      lines.push(`  chart ${chart.index} ${chart.name} (${chart.datamodel}, from ${chart.source}): ${chart.states.length} states, ${chart.transitions.length} transitions, data ${chart.data.map((d) => d.id).join(', ') || 'none'}`);
      for (const state of chart.states) {
        if (state.parent === null) continue;
        const initial = state.initial ? ` initial ${state.initial.map((i) => chart.states[i].id).join(' ')}` : '';
        lines.push(`    state ${state.id}: ${state.kind}${state.parent ? ` in ${chart.states[state.parent].id}` : ''}${initial}`);
      }
      for (const transition of chart.transitions) {
        if (transition.type === 'initial') continue;
        lines.push(`    transition ${transition.index}: ${chart.states[transition.source]?.id ?? '?'} → ${transition.targets.map((i) => chart.states[i].id).join(' ') || '(targetless)'}${transition.events.length ? ` on ${transition.events.join(' ')}` : ''}${transition.cond ? ` if ${transition.cond}` : ''}${transition.type === 'internal' ? ' (internal)' : ''}`);
      }
    }
    for (const actor of actors) {
      const where = actor.configuration ? ` in ${actor.configuration.map((i) => segment.charts[actor.chart]?.states[i]?.id ?? i).join(' ')}` : '';
      lines.push(`  actor ${actor.session} ${actor.name ?? '(no name)'} (slot ${actor.slot}, ${actor.chartName ?? '?'}): ${stateName(actor.state)}${where}, ${actor.mailboxLength ?? '?'} queued${actor.timers ? `, ${actor.timers} timers` : ''}`);
      for (const event of actor.queued ?? []) lines.push(`    queued ${event?.eventName ?? '?'}${event?.strings?.sendid ? ` (sendid ${event.strings.sendid})` : ''}`);
      for (const timer of actor.delayed ?? []) lines.push(`    delayed ${timer.event?.eventName ?? '?'}${timer.strings?.target ? ` to ${timer.strings.target}` : ''}`);
    }
    const traces = segment.traces;
    if (!traces) { lines.push('  traces: tracing off'); continue; }
    const r = traces.retained;
    lines.push(`  traces: ${r.records} records (${r.acknowledged} acknowledged, ${r.pending} pending), ${r.bytes} bytes of ${r.capacity}, ${r.lost} lost, ticks ${r.from} to ${r.to}; ${traces.spans.length} spans`);
    const byName = new Map();
    for (const span of traces.spans) byName.set(span.name, (byName.get(span.name) ?? 0) + 1);
    for (const [name, count] of [...byName].sort(([a], [b]) => (a < b ? -1 : a > b ? 1 : 0))) lines.push(`    ${count} × ${name}`);
  }
  if (pack.traces) lines.push(`traces: ${pack.traces.spans.length} spans in ${pack.traces.traces} traces`);
  return lines;
}
