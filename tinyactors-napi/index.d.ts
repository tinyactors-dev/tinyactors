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

/** The value as CBOR (`ta_cbor_encode`), by the value table. */
export function encode(value: unknown): Uint8Array;
/** The value of one CBOR item (`ta_cbor_decode`), by the value table. */
export function decode(bytes: ArrayBuffer | ArrayBufferView): Value | undefined;
