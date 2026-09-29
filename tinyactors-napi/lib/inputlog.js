'use strict';

// Reading an input log for an audit trail (napi ADR 0034 section 5): core's
// portable record layout (core ADRs 0090 and 0091) decoded into plain
// records, one at a time, from byte chunks cut anywhere. Replay never needs
// this: core reads its own records. Values are decoded with the built-in
// data model's reader (napi ADR 0031), so a class the log names must be in
// `classes`, beside the standard ones.

const { TinyactorsError } = require('./errors');
const { STANDARD_CLASSES } = require('./values');
const { durableReader } = require('./durable');

const HEADER = 32;
const ABSENT = 0xffffffff;
const VERSION = 1;
const EVENT_TYPES = ['external', 'internal', 'platform'];

// Each kind's name and fields, in core's order: `t` a text, `n` a number,
// `list` a count followed by that many texts (core's trailing lists).
const KINDS = {
  1: { kind: 'spawned', fields: ['t:family', 't:name', 't:revision'], values: ['binding'] },
  2: { kind: 'event', fields: ['n:type', 'n:hostSeq', 't:name', 't:sendID', 't:origin', 't:originType', 't:invokeID'], values: ['data'] },
  3: { kind: 'jump', fields: ['n:type', 'n:hostSeq', 't:name', 't:sendID', 't:origin', 't:originType', 't:invokeID', 't:source', 'list:targets'], values: ['data'] },
  4: { kind: 'report', fields: ['n:reportKind', 'n:ends', 't:invokeID', 't:reason'], values: ['data'] },
  5: { kind: 'fired', fields: ['t:sendID'], values: [] },
  6: { kind: 'withdrawn', fields: ['t:sendID'], values: [] },
  7: { kind: 'abandoned', fields: [], values: [] },
  8: { kind: 'released', fields: [], values: [] },
  9: { kind: 'finished', fields: ['t:finalState'], values: [] },
  10: { kind: 'destroyed', fields: [], values: [] },
  11: { kind: 'queued', fields: ['n:type', 'n:hostSeq', 't:name', 't:sendID', 't:origin', 't:originType', 't:invokeID'], values: ['data'] },
  12: { kind: 'dropped', fields: ['n:hostSeq'], values: [] },
  13: { kind: 'evicted', fields: [], values: [] },
  14: { kind: 'checkpoint', fields: ['t:family', 't:name', 't:revision', 'n:counter', 'n:macrostep', 'n:microstep',
                                     'n:lifecycle', 'n:parts', 'n:owed', 'n:activeCount', 'n:lateCount'], values: ['data'] },
  15: { kind: 'checkpoint-history', fields: ['t:history', 'list:states'], values: [] },
  16: { kind: 'checkpoint-send', fields: ['t:sendID', 't:name', 't:type', 't:target', 'n:due', 'n:flags',
                                          'n:targetContent', 'n:content'], values: ['data', 'targetValue'] },
  17: { kind: 'checkpoint-invoke', fields: ['t:invokeID', 't:type', 'n:index'], values: [] },
};

const corrupt = (why) => new TinyactorsError(`readInputLog(): ${why}`, 'invalid');

function le(bytes, at, count) {
  let value = 0n;
  for (let index = count - 1; index >= 0; index -= 1) {
    value = (value << 8n) | BigInt(bytes[at + index]);
  }
  return value;
}

const pad8 = (length) => (length + 7) & ~7;

// The span of one whole record at `at`, or 0 when it is not all there.
function spanOf(bytes, at, length) {
  if (length - at < HEADER) {
    return 0;
  }
  const fixed = Number(le(bytes, at + 4, 4));
  const text = Number(le(bytes, at + 24, 4));
  const values = Number(le(bytes, at + 28, 2));
  if (fixed < HEADER) {
    throw corrupt('a record\'s fixed part is shorter than its header');
  }
  let end = at + fixed + pad8(text);
  for (let value = 0; value < values; value += 1) {
    for (;;) {
      if (end + 4 > length) {
        return 0;
      }
      const chunk = Number(le(bytes, end, 4));
      end += chunk === 0 ? 8 : pad8(4 + chunk);
      if (chunk === 0) {
        break;
      }
    }
  }
  return end <= length ? end - at : 0;
}

// One record, whose bytes are all at `at`, as a plain object.
function decode(bytes, at, classes, utf8) {
  const version = Number(le(bytes, at, 2));
  const code = Number(le(bytes, at + 2, 2));
  const fixed = Number(le(bytes, at + 4, 4));
  const shape = KINDS[code];
  if (version !== VERSION || shape === undefined) {
    throw corrupt(`a record of version ${version} and kind ${code} is not one this build reads`);
  }
  const textAt = at + fixed;
  const textBytes = Number(le(bytes, at + 24, 4));
  const valueCount = Number(le(bytes, at + 28, 2));
  const record = { kind: shape.kind, seq: le(bytes, at + 8, 8).toString(), time: Number(le(bytes, at + 16, 8)) };
  const text = (field) => {
    const offset = Number(le(bytes, at + HEADER + 8 * field, 4));
    const length = Number(le(bytes, at + HEADER + 8 * field + 4, 4));
    if (offset === ABSENT) {
      return undefined;
    }
    if (offset + length > textBytes) {
      throw corrupt('a text runs past the record\'s text part');
    }
    return utf8.decode(bytes.subarray(textAt + offset, textAt + offset + length));
  };
  let field = 0;
  for (const spec of shape.fields) {
    const [type, name] = spec.split(':');
    if (type === 'n') {
      record[name] = le(bytes, at + HEADER + 8 * field, 8);
      field += 1;
    } else if (type === 't') {
      const value = text(field);
      if (value !== undefined) {
        record[name] = value;
      }
      field += 1;
    } else {
      const count = Number(le(bytes, at + HEADER + 8 * field, 8));
      field += 1;
      record[name] = [];
      for (let index = 0; index < count; index += 1) {
        record[name].push(text(field));
        field += 1;
      }
    }
  }
  // Texts past the named fields: a checkpoint's active and late-bound state lists.
  if (shape.kind === 'checkpoint') {
    record.active = [];
    record.late = [];
    for (let index = 0; index < Number(record.activeCount); index += 1) {
      record.active.push(text(field + index));
    }
    for (let index = 0; index < Number(record.lateCount); index += 1) {
      record.late.push(text(field + Number(record.activeCount) + index));
    }
    delete record.activeCount;
    delete record.lateCount;
  }
  let valueAt = textAt + pad8(textBytes);
  for (let index = 0; index < valueCount; index += 1) {
    const reader = durableReader(classes);
    for (;;) {
      const chunk = Number(le(bytes, valueAt, 4));
      if (chunk === 0) {
        valueAt += 8;
        break;
      }
      reader.push(utf8.decode(bytes.subarray(valueAt + 4, valueAt + 4 + chunk), { stream: true }));
      valueAt += pad8(4 + chunk);
    }
    reader.push(utf8.decode());
    record[shape.values[index] ?? `value${index}`] = reader.end();
  }
  // Friendlier forms of the numbers an audit reads.
  if (record.type !== undefined && typeof record.type === 'bigint') {
    record.type = EVENT_TYPES[Number(record.type)] ?? 'external';
  }
  // A host number (the mailbox's, core ADR 0090 section 6) is text: it is 64 bits.
  if (typeof record.hostSeq === 'bigint') {
    record.hostSeq = record.hostSeq === 0n ? undefined : record.hostSeq.toString();
    if (record.hostSeq === undefined) {
      delete record.hostSeq;
    }
  }
  if (record.seq === '0') {
    delete record.seq; // a host record written while no actor was resident
  }
  if (typeof record.ends === 'bigint') {
    record.ends = record.ends !== 0n;
  }
  if (record.family !== undefined && record.name !== undefined && shape.kind !== 'event' && shape.kind !== 'queued' &&
      shape.kind !== 'jump') {
    record.address = { family: record.family, name: record.name };
    delete record.family;
    delete record.name;
  }
  delete record.reportKind;
  for (const name of Object.keys(record)) {
    if (typeof record[name] === 'bigint') {
      record[name] = Number(record[name]);
    }
  }
  return Object.freeze(record);
}

/**
 * Decode an input log (napi ADR 0034 section 5): `chunks`, an iterable or
 * async iterable of Uint8Arrays cut anywhere, yields its records in order,
 * one at a time, as plain objects ({ kind, seq, time, ... }). `classes`
 * names the classes the log's values construct, beside the standard ones
 * (napi ADR 0031). For showing an actor's history; replay never needs it.
 */
async function* readInputLog(chunks, options = {}) {
  const open = chunks?.[Symbol.asyncIterator] ?? chunks?.[Symbol.iterator];
  if (chunks === null || typeof chunks !== 'object' || typeof open !== 'function' || chunks instanceof Uint8Array) {
    throw new TinyactorsError('readInputLog(): expected an iterable or async iterable of Uint8Array chunks', 'invalid');
  }
  const classes = { ...STANDARD_CLASSES, ...(options?.classes ?? {}) };
  const utf8 = new TextDecoder('utf-8', { fatal: true });
  let bytes = new Uint8Array(0);
  let start = 0;
  for await (const chunk of { [Symbol.asyncIterator]: () => {
    const iterator = open.call(chunks);
    return { next: async () => iterator.next() };
  } }) {
    if (!(chunk instanceof Uint8Array)) {
      throw new TinyactorsError('readInputLog(): a chunk is a Uint8Array', 'invalid');
    }
    const joined = new Uint8Array(bytes.length - start + chunk.length);
    joined.set(bytes.subarray(start), 0);
    joined.set(chunk, bytes.length - start);
    bytes = joined;
    start = 0;
    for (;;) {
      const span = spanOf(bytes, start, bytes.length);
      if (span === 0) {
        break;
      }
      yield decode(bytes, start, classes, utf8);
      start += span;
    }
  }
  if (start !== bytes.length) {
    throw corrupt('the log ends in the middle of a record');
  }
}

module.exports = { readInputLog };
