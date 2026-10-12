// The types of @tinyactors/node (index.mjs re-exports the addon). Each
// item is in docs/api-map.md with the core functions behind it.

/**
 * What core refused. A refused request or config carries core's reply
 * unchanged: `code`, `reason`, `path` and the message. A refused call
 * carries its status (`status`, types.h), and the message is the status's
 * name (`TA_ERROR_INVALID`). Match on `code` and `reason`, or on
 * `status`; never on the message.
 */
export class TinyactorsError extends Error {
  readonly name: 'TinyactorsError';
  /** The JSON-RPC error code of a refused request or config (requests.md). */
  readonly code?: number;
  /** Why: `type`, `missing`, `unknown`, `range`, ... (requests.md). */
  readonly reason?: string;
  /** Where in the params or config, `system.maxActors`. */
  readonly path?: string;
  /** The status of a refused call: -3 `TA_ERROR_INVALID`, ... (types.h). */
  readonly status?: number;
}

/**
 * A neutral value: what crosses as CBOR (docs/values.md). Integers past
 * ±(2^53-1) are `bigint`, bytes are `Uint8Array`, an actor reference is its
 * address text.
 */
export type Value =
  | null
  | boolean
  | number
  | bigint
  | string
  | Uint8Array
  | Value[]
  | { [key: string]: Value };

/** Core's statuses (types.h), to match a TinyactorsError's `status` on. */
export const OK: 0;
export const ERROR_EXECUTION: -1;
export const ERROR_COMMUNICATION: -2;
export const ERROR_INVALID: -3;
export const ERROR_NO_MEMORY: -4;
export const ERROR_STALE: -5;
export const ERROR_STATE: -6;
export const ERROR_UNSUPPORTED: -7;
export const ERROR_FATAL: -8;
export const ERROR_ABORTED: -9;

/** An id core gives: a session, a segment, a peer. */
export type ID = bigint;

/** One chart's source: its name (for `src` and diagnostics) and its text. */
export interface Source {
  name: string;
  text: string;
}

/** What `compile` refuses with: one of core's diagnostics. */
export interface Diagnostic {
  /** 0 note, 1 warning, 2 error. */
  severity: number;
  code: number;
  line: number;
  column: number;
  file: string;
  message: string;
}

export interface CompileOptions {
  /** A static `src=""`: the document it names, or undefined for none. */
  resolve?(request: { base: string; source: string }): Source | undefined;
}

/**
 * The charts compiled into one image (`ta_compile_charts`); its bytes. The
 * first chart is the image's default. Refused: a TinyactorsError with
 * `diagnostics`.
 */
export function compile(charts: Source[], options?: CompileOptions): Uint8Array;

/** A `<log>`: its label and text. */
export interface LogRecord {
  session: ID;
  label: string;
  text: string;
}

/** An actor reached a top-level final state. */
export interface DoneRecord {
  session: ID;
  /** The name it was spawned with. */
  name: string;
  /** Its `<donedata>`. */
  data: Value | undefined;
  /** The id of the `<final>` that ended it. */
  final: string;
}

/** An actor faulted and was reclaimed. */
export interface ErrorRecord {
  session: ID;
  /** The fault code. */
  code: number;
}

/** An envelope (or an error envelope) for the system behind `peer`. */
export interface PeerRecord {
  peer: ID;
  envelope: Uint8Array;
}

/** What the host's effects are handed to, each while the loop runs. */
export interface Handlers {
  log?(record: LogRecord): void;
  done?(record: DoneRecord): void;
  error?(record: ErrorRecord): void;
  peer?(record: PeerRecord): void;
  peerError?(record: PeerRecord): void;
}

export interface HostOptions {
  /** The host's config (tinyactors-core/docs/host-config.md). */
  config: { systemID: string; [key: string]: Value };
  /** The datamodel of the host's "ecmascript" charts: this package's, or quickjs. */
  datamodel?: 'native' | 'quickjs';
  /** quickjs's settings, with `datamodel: 'quickjs'`. */
  quickjs?: { realm?: 'auto' | 'isolated' };
  /** A simulated clock: then the host arms no timer. */
  clock?: SimulatedClock;
  on?: Handlers;
}

/** One tinyactors host (`ta_host`), its turn on the runtime's event loop. */
export class Host {
  constructor(options: HostOptions);
  /**
   * An image's bytes (`compile`), added as a segment. The host holds the
   * bytes themselves until the segment is freed: do not change them.
   */
  addImage(image: ArrayBuffer | ArrayBufferView): ID;
  /** The actors a system image declares. */
  boot(segment: ID): void;
  /** A new actor of the current version of the chart named. */
  spawn(chart: string, options?: { name?: string; params?: Value; traceparent?: string }): ID;
  send(session: ID | number, event: string, data?: Value): void;
  /** One batch; the length of the accepted prefix. */
  sendMany(events: { session: ID | number; event: string; data?: Value }[]): number;
  terminate(session: ID | number): void;
  /** A request method (tinyactors-core/docs/requests.md): its result. */
  request(method: string, params?: { [key: string]: Value }): Value;
  addPeer(peer: { id: string; trusted?: boolean }): ID;
  /** An envelope from another process; the TA_ROUTE_* outcome. */
  deliver(peer: ID | number, envelope: ArrayBuffer | ArrayBufferView): number;
  /** Frees the host. From a handler, after the loop returns. */
  close(): void;
}

/** A caller's own clock (xstate's shape, core's meaning). */
export class SimulatedClock {
  constructor();
  /** Moves the time on by `ms` and runs each of its hosts until idle. */
  increment(ms: number): void;
  /** The simulated time, in milliseconds. */
  now(): number;
}

/** The value as CBOR (core's streaming writer, `ta_cbor_writer`), by the value table. */
export function encode(value: unknown): Uint8Array;
/** The value of one CBOR item (core's reader, `ta_cbor_read`), by the value table. */
export function decode(bytes: ArrayBuffer | ArrayBufferView): Value | undefined;
