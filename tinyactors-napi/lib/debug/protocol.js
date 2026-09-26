'use strict';

// The base protocol of the Debug Adapter Protocol: each message is a JSON
// object after a `Content-Length` header and a blank line. A DebugSession
// takes and gives message objects, so a transport that frames messages
// itself (a WebSocket) needs none of this; one that carries bytes (stdio, a
// TCP socket) reads with DAPReader and writes with encodeDAPMessage. Host
// neutral: Uint8Array, TextEncoder and TextDecoder only.

const { TinyactorsError } = require('../errors');

const encoder = new TextEncoder();
const decoder = new TextDecoder('utf-8', { fatal: true });
const CR = 13;
const LF = 10;

/** One message as the bytes a byte stream carries: the header, then its JSON. */
function encodeDAPMessage(message) {
  const body = encoder.encode(JSON.stringify(message));
  const header = encoder.encode(`Content-Length: ${body.length}\r\n\r\n`);
  const out = new Uint8Array(header.length + body.length);
  out.set(header, 0);
  out.set(body, header.length);
  return out;
}

/**
 * Turns a byte stream into messages: push() each chunk as it arrives, and
 * `onMessage` gets every whole message in order. A header without
 * Content-Length, or a body that is not JSON, throws `invalid`: the stream
 * cannot be resynchronized after it.
 */
class DAPReader {
  #onMessage;
  #buffer = new Uint8Array(0);

  constructor(onMessage) {
    if (typeof onMessage !== 'function') {
      throw new TinyactorsError('DAPReader: expected a function for each message', 'invalid');
    }
    this.#onMessage = onMessage;
  }

  push(chunk) {
    const bytes = chunk instanceof Uint8Array ? chunk : encoder.encode(String(chunk));
    const joined = new Uint8Array(this.#buffer.length + bytes.length);
    joined.set(this.#buffer, 0);
    joined.set(bytes, this.#buffer.length);
    this.#buffer = joined;
    for (;;) {
      const end = headerEnd(this.#buffer);
      if (end < 0) {
        return;
      }
      const length = contentLength(decoder.decode(this.#buffer.subarray(0, end)));
      const start = end + 4;
      if (this.#buffer.length < start + length) {
        return;
      }
      const body = this.#buffer.subarray(start, start + length);
      this.#buffer = this.#buffer.slice(start + length);
      let message;
      try {
        message = JSON.parse(decoder.decode(body));
      } catch (error) {
        throw new TinyactorsError(`DAP: a message is not JSON: ${error.message}`, 'invalid');
      }
      this.#onMessage(message);
    }
  }
}

// The index of the blank line after the headers, or -1.
function headerEnd(bytes) {
  for (let at = 0; at + 3 < bytes.length; at += 1) {
    if (bytes[at] === CR && bytes[at + 1] === LF && bytes[at + 2] === CR && bytes[at + 3] === LF) {
      return at;
    }
  }
  return -1;
}

function contentLength(headers) {
  for (const line of headers.split('\r\n')) {
    const colon = line.indexOf(':');
    if (colon > 0 && line.slice(0, colon).trim().toLowerCase() === 'content-length') {
      const length = Number(line.slice(colon + 1).trim());
      if (Number.isSafeInteger(length) && length >= 0) {
        return length;
      }
    }
  }
  throw new TinyactorsError('DAP: a message has no Content-Length header', 'invalid');
}

module.exports = { DAPReader, encodeDAPMessage };
