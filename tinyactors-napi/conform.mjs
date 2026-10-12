// The conformance suite's adapter (tinyactors-test/docs/driver.md): calls
// as length-prefixed CBOR frames on standard input, replies on standard
// output, each method made with the package's public API only, and the
// frames read and written with its own `encode` and `decode`. It imports
// nothing but @tinyactors/node and node: built-ins (tinyactors-test/lint).
//
// A binding offers every method but the C-level ones (driver.md, "Kinds"):
// envelope.read and value.encode answer -32601. w3c.run is M2's
// (ROADMAP.md) and answers -32601 until then.
import process from 'node:process';
import { encode, decode, compile, Host, SimulatedClock, TinyactorsError } from '@tinyactors/node';

// ---- the protocol's errors (driver.md, "Frames and messages") ----------------------

class ProtocolError extends Error {
  constructor(code, reason, path, message) {
    super(message);
    this.code = code;
    this.reason = reason;
    this.path = path;
  }
}

const UNKNOWN_METHOD = (method) => new ProtocolError(-32601, 'unknown', 'method', `no method ${method}`);
const missing = (name) => new ProtocolError(-32602, 'missing', `params.${name}`, `${name} is required`);
const wrongType = (path, message) => new ProtocolError(-32602, 'type', path, message);
const noHost = () => new ProtocolError(-32002, 'state', '', 'no host: call host.create first');

// The status of what the API refused: core's, or the call's status.
function statusOf(error) {
  if (error instanceof TinyactorsError && typeof error.status === 'number') return error.status;
  throw error;
}

function required(params, name) {
  if (params?.[name] === undefined) throw missing(name);
  return params[name];
}

// ---- datamodels: the host's options for the one asked for ----------------------------

// Both of Node's datamodels are named "ecmascript" (the vtable's name), so a
// host holds the one asked for (driver.md, "The chart language").
function datamodelOptions(params) {
  const datamodel = params?.datamodel ?? 'quickjs';
  if (datamodel !== 'quickjs' && datamodel !== 'native')
    throw new ProtocolError(-32602, 'range', 'params.datamodel', `no datamodel ${datamodel}`);
  const options = { datamodel };
  if (datamodel === 'quickjs' && params?.realm !== undefined) options.quickjs = { realm: params.realm };
  return options;
}

// ---- documents and images ---------------------------------------------------------------

// A src="" names a document of the call by its name, with a leading
// file://, file: or ./ removed (driver.md, "src").
function documentName(source) {
  for (const prefix of ['file://', 'file:', './']) if (source.startsWith(prefix)) return source.slice(prefix.length);
  return source;
}

function compileCharts(charts, documents) {
  return compile(charts, {
    resolve: ({ source }) => documents.find((document) => document.name === documentName(source)),
  });
}

// ---- the vector methods: one host between calls ------------------------------------------

let current = null; // { host, clock, envelopes }

function hostOf() {
  if (!current) throw noHost();
  return current;
}

function hostCreate(params) {
  if (current) throw new ProtocolError(-32002, 'state', '', 'a host already: call host.free first');
  const options = datamodelOptions(params);
  const config = decode(required(params, 'config'));
  const clock = new SimulatedClock();
  const envelopes = [];
  const keep = (record) => envelopes.push(record.envelope);
  try {
    const host = new Host({ config, ...options, clock, on: { peer: keep, peerError: keep } });
    current = { host, clock, envelopes };
    return { status: 0, datamodel: 'ecmascript' };
  } catch (error) {
    if (!(error instanceof TinyactorsError)) throw error;
    if (error.code === undefined) return { status: error.status };
    const refusal = { error: { code: error.code, message: error.message, data: { reason: error.reason, path: error.path } } };
    return { status: -3, error: encode(refusal) };
  }
}

function hostFree() {
  hostOf().host.close();
  current = null;
  return {};
}

// A request through request(method, params), its reply written from what
// it returned or threw, with the request's id (tinyactors-napi/ROADMAP.md,
// "Requests to core": the requests it cannot send).
function hostRequest(params) {
  const { host } = hostOf();
  const request = decode(required(params, 'request'));
  const id = request?.id ?? null;
  let reply;
  try {
    reply = { id, result: host.request(request?.method, request?.params) };
  } catch (error) {
    if (!(error instanceof TinyactorsError) || error.code === undefined) throw error;
    reply = { id, error: { code: error.code, message: error.message, data: { reason: error.reason, path: error.path } } };
  }
  const bytes = encode(reply);
  return { status: 0, length: bytes.length, reply: bytes };
}

function imageAdd(params) {
  const { host } = hostOf();
  const charts = required(params, 'charts');
  try {
    return { status: 0, segment: host.addImage(compileCharts(charts, charts)) };
  } catch (error) {
    return { status: statusOf(error) };
  }
}

function actorSpawn(params) {
  const { host } = hostOf();
  try {
    return { status: 0, session: host.spawn(required(params, 'chart'), { params: params.params }) };
  } catch (error) {
    return { status: statusOf(error) };
  }
}

function actorSend(params) {
  const { host } = hostOf();
  try {
    host.send(required(params, 'session'), required(params, 'event'), params.data);
    return { status: 0 };
  } catch (error) {
    return { status: statusOf(error) };
  }
}

// The simulated clock moves to `monotonic` (ticks of the vectors' configs,
// 1000 per second: milliseconds).
function clockAdvance(params) {
  const { clock } = hostOf();
  const monotonic = Number(params?.monotonic ?? 0);
  if (monotonic > clock.now()) clock.increment(monotonic - clock.now());
  return {};
}

// The loop until idle (increment returns only then), and the envelopes the
// peer handlers were given since the last host.run, in order.
function hostRun() {
  const state = hostOf();
  state.clock.increment(0);
  const envelopes = state.envelopes.splice(0);
  return { status: 0, envelopes };
}

function peerAdd(params) {
  const { host } = hostOf();
  try {
    return { status: 0, peer: host.addPeer({ id: required(params, 'id'), trusted: params.trusted === true }) };
  } catch (error) {
    return { status: statusOf(error) };
  }
}

function envelopeDeliver(params) {
  const { host } = hostOf();
  try {
    return { status: 0, route: host.deliver(required(params, 'peer'), required(params, 'cbor')) };
  } catch (error) {
    return { status: statusOf(error) };
  }
}

// ---- scenario.run (driver.md, "The scenario environment") ----------------------------------

// "@alias" at any depth stands for that actor's address.
function withAddresses(value, aliases) {
  if (typeof value === 'string' && value.startsWith('@') && aliases.has(value.slice(1)))
    return `#_scxml_${aliases.get(value.slice(1))}`;
  if (Array.isArray(value)) return value.map((item) => withAddresses(item, aliases));
  if (value && typeof value === 'object' && !(value instanceof Uint8Array)) {
    const copy = {};
    for (const [key, item] of Object.entries(value)) copy[key] = withAddresses(item, aliases);
    return copy;
  }
  return value;
}

// A step the host refused: the scenario fails, and says why.
class Refused extends Error {}

function scenarioRun(params) {
  const documents = required(params, 'documents');
  const steps = required(params, 'steps');
  if (!Array.isArray(steps)) throw wrongType('params.steps', 'steps is an array');
  const clock = new SimulatedClock();
  let outcome = null;
  const host = new Host({
    config: { systemID: 'conformance', system: { ticksPerSecond: 1000 } },
    ...datamodelOptions(params),
    clock,
    on: { log: ({ label, text }) => (outcome ??= label === 'Outcome' ? (text === 'pass' ? 'pass' : 'fail') : null) },
  });
  const aliases = new Map();
  const session = (alias) => {
    if (!aliases.has(alias)) throw new Refused(`a send to an alias no spawn made: ${alias}`);
    return aliases.get(alias);
  };
  const step = (index, map) => {
    const at = `params.steps[${index}]`;
    if (!map || typeof map !== 'object') throw wrongType(at, 'a step is a map');
    if (map.image !== undefined) {
      const charts = map.image.map((name) => {
        const document = documents.find((each) => each.name === name);
        if (!document) throw wrongType(at, `image names a document the call does not have: ${name}`);
        return document;
      });
      let image;
      try {
        image = compileCharts(charts, documents);
      } catch (error) {
        if (error instanceof TinyactorsError) return 'compile';
        throw error;
      }
      host.addImage(image);
    } else if (map.spawn !== undefined) {
      const params = map.params === undefined ? undefined : withAddresses(map.params, aliases);
      aliases.set(map.spawn, host.spawn(map.chart, { name: map.spawn, params }));
    } else if (map.send !== undefined) {
      host.send(session(map.send), map.event, withAddresses(map.data, aliases));
    } else if (map.sendMany !== undefined) {
      const events = map.sendMany.map((send) => ({ session: session(send.to), event: send.event, data: withAddresses(send.data, aliases) }));
      if (host.sendMany(events) !== events.length) throw new Refused('ta_host_send_many took fewer events');
    } else if (map.advance !== undefined) {
      clock.increment(Number(map.advance));
    } else {
      throw new Refused(`a step this adapter does not run yet (ROADMAP.md): ${Object.keys(map).join(', ')}`);
    }
    clock.increment(0);
    return null;
  };
  try {
    for (let index = 0; index < steps.length && !outcome; index++) {
      try {
        const compiled = step(index, steps[index]); // the handler may set the outcome meanwhile
        outcome ??= compiled;
      } catch (error) {
        if (error instanceof ProtocolError) throw error;
        process.stderr.write(`conform: step ${index}: ${error?.message ?? error}\n`);
        outcome ??= 'fail';
      }
    }
  } finally {
    host.close();
  }
  return { outcome: outcome ?? 'timeout' };
}

// ---- the method table and the frames -------------------------------------------------------

const METHODS = {
  'scenario.run': scenarioRun,
  'host.create': hostCreate,
  'host.free': hostFree,
  'host.request': hostRequest,
  'image.add': imageAdd,
  'actor.spawn': actorSpawn,
  'actor.send': actorSend,
  'clock.advance': clockAdvance,
  'host.run': hostRun,
  'peer.add': peerAdd,
  'envelope.deliver': envelopeDeliver,
};

function answer(call) {
  const id = call?.id ?? null;
  try {
    if (!call || typeof call !== 'object' || typeof call.method !== 'string')
      throw new ProtocolError(-32600, 'type', '', 'not a call');
    const method = METHODS[call.method];
    if (!method) throw UNKNOWN_METHOD(call.method);
    return { id, result: method(call.params ?? {}) };
  } catch (error) {
    if (!(error instanceof ProtocolError)) {
      process.stderr.write(`conform: ${call?.method}: ${error?.stack ?? error}\n`);
      error = new ProtocolError(-32603, 'internal', '', String(error?.message ?? error));
    }
    return { id, error: { code: error.code, message: error.message, data: { reason: error.reason, path: error.path } } };
  }
}

function frame(bytes) {
  const out = new Uint8Array(4 + bytes.length);
  new DataView(out.buffer).setUint32(0, bytes.length);
  out.set(bytes, 4);
  return out;
}

let pending = new Uint8Array(0);

function take(chunk) {
  const joined = new Uint8Array(pending.length + chunk.length);
  joined.set(pending, 0);
  joined.set(chunk, pending.length);
  pending = joined;
  while (pending.length >= 4) {
    const length = new DataView(pending.buffer, pending.byteOffset, 4).getUint32(0);
    if (pending.length < 4 + length) break;
    const message = pending.slice(4, 4 + length);
    pending = pending.slice(4 + length);
    let call;
    let reply;
    try {
      call = decode(message);
      reply = answer(call);
    } catch {
      reply = { id: null, error: { code: -32700, message: 'not CBOR', data: { reason: 'malformed', path: '' } } };
    }
    process.stdout.write(frame(encode(reply)));
  }
}

process.stdin.on('data', (chunk) => take(new Uint8Array(chunk.buffer, chunk.byteOffset, chunk.byteLength)));
process.stdin.on('end', () => {});
