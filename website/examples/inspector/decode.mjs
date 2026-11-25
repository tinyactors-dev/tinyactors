// The inspector's decoder: a snapshot pack (format 4) into plain objects.
// The pack describes its own region layout (a CBOR part written by the
// build that made it), so everything below reads records through that
// description: member names are the only thing known in advance. Pure: no
// DOM and no file system, so main.mjs and page.mjs decode the same bytes.

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
  const imageParts = images.map((length, i) => ({ index: i, length, offset: part(length, `image ${i}`).offset }));
  const layout = indexLayout(decodeCBOR(layoutPart.bytes));
  const host = decodeHost(layout, hostPart.bytes);
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
  };
}

/** The host snapshot: its header and each segment's region, decoded. */
export function decodeHost(layout, bytes) {
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
    segments.push({ index, live, imageHash, regionBytes, config, ...decodeRegion(layout, region) });
  }
  return { header, segments };
}

/**
 * A segment's region: the system's header record, its pools and arrays
 * (where each lies and how full it is), its actors, and the memory map.
 */
export function decodeRegion(layout, region) {
  const view = new DataView(region.buffer, region.byteOffset, region.byteLength);
  const systemType = layout.system ?? 'ta_system';
  const systemSize = layout.typeMap.get(systemType)?.size ?? 0;
  const system = readStruct(layout, view, 0, systemType);
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

  const actorsPool = pool('actors');
  const actors = [];
  const slots = [];
  if (actorsPool && actorsPool.offset !== null) {
    const element = actorsPool.element;
    const kindOf = (name) => memberOf(layout, element, name)?.kind ?? '';
    const poolOfKind = (kind) => (/^(index|handle):(.+)$/.exec(kind) ?? [])[2] ?? null;
    for (let index = 0; index < Math.min(actorsPool.highWater, actorsPool.capacity); index++) {
      if (!live(actorsPool, index)) { slots.push(null); continue; }
      const record = readStruct(layout, view, at(actorsPool, index), element) ?? {};
      const mailboxPool = poolOfKind(kindOf('mailbox_head'));
      const timersPool = poolOfKind(kindOf('timers'));
      const actor = {
        slot: index, session: record.serial ?? null, state: record.state ?? null,
        mailboxLength: record.mailbox_length ?? null,
        mailbox: mailboxPool ? chain(mailboxPool, record.mailbox_head, 4096) : [],
        timers: timersPool ? chain(timersPool, record.timers, 4096).length : null,
        parent: handleSlot(record.parent), name: record.name ?? null,
      };
      actors.push(actor);
      slots.push(actor.state);
    }
    // the slots never handed out yet, so the grid shows the whole pool
    for (let index = slots.length; index < Math.min(actorsPool.capacity, 1 << 22); index++) slots.push(null);
  }
  return { region, system, systemSize, pools, arrays, actors, slots, memoryMap: memoryMap(region.length, systemType, systemSize, pools, arrays) };
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
    for (const actor of actors) {
      lines.push(`  actor ${actor.session} (slot ${actor.slot}): ${stateName(actor.state)}, ${actor.mailboxLength ?? '?'} queued${actor.timers ? `, ${actor.timers} timers` : ''}`);
    }
  }
  return lines;
}
