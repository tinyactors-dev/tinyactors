// The conformance suite's adapter: tinyactors-test/docs/driver.md over the
// package's public module, `@tinyactors/node`, and `node:` built-ins only
// (its lint, tinyactors-test/lint, refuses any other import).
//
// Run by `mise run conform` as `node conform.mjs` or `bun conform.mjs`.
// Frames are a four-byte big-endian length and one CBOR message, read and
// written with the package's own decode and encode. Calls are answered one
// at a time, in order; a method is the binding's own calls, as an
// application makes them. A method the adapter does not offer yet is
// answered -32601, and the runner skips what needs it. Standard output
// holds frames only; diagnostics go to standard error. The adapter exits
// with status 0 when its input ends.
import { Buffer } from 'node:buffer';
import process from 'node:process';
import { decode, encode } from '@tinyactors/node';

// The driver's methods this adapter offers, by name: (params) => result,
// or a promise of it. None yet: they come with the Host (docs/plan.xml).
const methods = new Map();

class DriverError extends Error {
  constructor(code, reason, path, message) {
    super(message);
    this.code = code;
    this.reason = reason;
    this.path = path;
  }
}

function failure(id, error) {
  return { id, error: { code: error.code, message: error.message, data: { reason: error.reason, path: error.path } } };
}

async function answer(frame) {
  let call;
  try {
    call = decode(frame);
  } catch (error) {
    return failure(null, new DriverError(-32700, 'malformed', '', `the frame is not CBOR: ${error.message}`));
  }
  if (call === null || typeof call !== 'object' || Array.isArray(call) || !Number.isInteger(call.id)) {
    return failure(null, new DriverError(-32600, 'malformed', '', 'not a call: a map with an integer id'));
  }
  const method = methods.get(call.method);
  if (method === undefined) {
    return failure(call.id, new DriverError(-32601, 'unknown', 'method', `${call.method} is not offered`));
  }
  try {
    return { id: call.id, result: await method(call.params ?? {}) };
  } catch (error) {
    if (error instanceof DriverError) return failure(call.id, error);
    throw error;
  }
}

function write(reply) {
  const body = encode(reply);
  const header = Buffer.alloc(4);
  header.writeUInt32BE(body.length);
  process.stdout.write(Buffer.concat([header, body]));
}

// The input as it arrives: whole frames are answered in order, each after
// the one before it settled.
let pending = Buffer.alloc(0);
let queue = Promise.resolve();

process.stdin.on('data', (chunk) => {
  pending = Buffer.concat([pending, chunk]);
  while (pending.length >= 4) {
    const size = pending.readUInt32BE(0);
    if (pending.length < 4 + size) break;
    const frame = new Uint8Array(pending.subarray(4, 4 + size));
    pending = pending.subarray(4 + size);
    queue = queue.then(() => answer(frame)).then(write);
  }
});

process.stdin.on('end', () => {
  queue.then(() => {
    if (pending.length > 0) {
      process.stderr.write(`conform: the input ended inside a frame (${pending.length} bytes)\n`);
      process.exitCode = 1;
    }
  });
});

// An error the adapter did not expect is a crash the runner reports with
// standard error's end.
process.on('unhandledRejection', (error) => {
  process.stderr.write(`conform: ${error?.stack ?? error}\n`);
  process.exit(70);
});
