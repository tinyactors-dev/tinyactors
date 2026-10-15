// @tinyactors/staticjs: staticjs, a small embeddable JavaScript engine for a
// pure, total subset of the language, as WebAssembly for Node and browsers.

/** Load the module: the one this package ships, or `source`. Each load is an instance of its own. */
export function load(source?: URL | string | Response | BufferSource | WebAssembly.Module): Promise<StaticJS>;

/** staticjs's limits (SJS_LIMITS_DEFAULT), by name. */
export const DEFAULT_LIMITS: Readonly<Limits>;

export interface Limits {
  maxSourceBytes: number;
  maxTokenBytes: number;
  maxNesting: number;
  maxLookahead: number;
  maxHeldEvents: number;
  maxStack: number;
  maxFrames: number;
  maxValueItems: number;
  maxStringBytes: number;
  maxBigintBits: number;
  maxRegexStates: number;
}

export interface ContextOptions {
  /** Over DEFAULT_LIMITS. */
  limits?: Partial<Limits>;
  /** The items one conversion between JavaScript and staticjs may make, either way. Default: limits.maxValueItems. */
  maxItems?: number;
}

export class StaticJS {
  /** staticjs's version, "0.9.0". */
  readonly version: string;
  /** A context: its own limits, and the values, programs and runs made in it. */
  createContext(options?: ContextOptions): Context;
}

export interface CompileOptions {
  /** Compile a module, whose value is the object of its exports. Default: an expression. */
  module?: boolean;
  /** The names each run gives values for. */
  globals?: readonly string[];
  /** What `import ... from "name"` finds: a module's exports, as a Value (a module run's resultValue()). */
  modules?: Record<string, Value>;
}

export class Context {
  /** A program; throws a StaticJSError for one staticjs refuses. */
  compile(source: string, options?: CompileOptions): Program;
  /** A JavaScript value made once in staticjs, to give to many runs. */
  value(value: unknown): Value;
  /** A value read from JSON, as JSON.parse reads it. */
  fromJSON(json: string | Uint8Array): Value;
  /** A value read from one CBOR neutral value. */
  fromCBOR(bytes: Uint8Array): Value;
  /** The clock a program reads; a program that reads it unset faults. A Temporal.Instant works. */
  setClock(instant: { epochMilliseconds: number } | { epochNanoseconds: bigint }): void;
  /** Move a set clock forward. */
  advanceClock(duration: { milliseconds?: number; nanoseconds?: number }): void;
  /** What Math.random draws from, as `node --random-seed=<seed>` does. */
  setSeed(seed: number): void;
  /** Bytes the context's values, programs and runs hold now. */
  readonly liveBytes: number;
  /** End the run, release every program and value, free the context. */
  dispose(): void;
  [Symbol.dispose](): void;
}

export interface FunctionSummary {
  readonly offset: number;
  readonly line: number;
  readonly column: number;
  readonly params: number;
  /** The shape a built-in may run in C ("PLUCK", "COMPARE_BY_KEY", ...), or "". */
  readonly shape: string;
}

export class Program {
  /** The names `start` takes values for. */
  readonly globals: readonly string[];
  /** Each function, the program's own code first. */
  functions(): FunctionSummary[];
  /**
   * A run on the values of the globals, by name. A function among them is a native the program calls.
   * The run takes the context's evaluator: a paused run of the context is abandoned.
   */
  start(globals?: Record<string, unknown>): Run;
  dispose(): void;
  [Symbol.dispose](): void;
}

export type Frame =
  | {
      readonly kind: 'function';
      readonly offset: number;
      readonly line: number;
      readonly column: number;
      /** The program the code is in, while it is not disposed. */
      readonly program: Program | undefined;
      /** The function's index in program.functions(). */
      readonly function: number;
    }
  | {
      readonly kind: 'loop';
      readonly offset: number;
      readonly line: number;
      readonly column: number;
      /** The built-in: "" and "map", or "Array" and "from". */
      readonly object: string;
      readonly name: string;
      /** How many elements it has passed, of how many; 0 and 0 for a sort. */
      readonly index: number;
      readonly length: number;
    };

export class Run {
  /** 'paused', 'done', 'faulted' or 'ended' (cancelled, or abandoned by a later start). */
  readonly state: 'paused' | 'done' | 'faulted' | 'ended';
  /** Fuel used so far. */
  readonly fuelUsed: number;
  /** Run on `fuel` more units: true when done, false when the fuel is spent. A fault throws a StaticJSError. */
  step(fuel: number): boolean;
  /** Where a paused run is, outermost first. */
  frames(): Frame[];
  /** The result as JavaScript, once done. */
  result(): unknown;
  /** The result kept in staticjs, once done. */
  resultValue(): Value;
  /** Abandon a paused run. */
  cancel(): void;
  dispose(): void;
  [Symbol.dispose](): void;
}

export class Value {
  readonly kind:
    | 'undefined' | 'null' | 'boolean' | 'number' | 'string' | 'bigint' | 'array' | 'object' | 'set' | 'map'
    | 'iterator' | 'function' | 'regexp' | 'temporal';
  /** The value as JavaScript, exactly; a function stays a Value. */
  toJS(): unknown;
  /** JavaScript source that evaluates to an equal value. */
  toString(): string;
  /** JSON text, as JSON.stringify gives it in staticjs. */
  json(): string;
  /** One CBOR neutral value. */
  cbor(): Uint8Array;
  /** A run calling this function (one a program made) with `args`. */
  start(args?: readonly unknown[]): Run;
  dispose(): void;
  [Symbol.dispose](): void;
}

export class StaticJSError extends Error {
  readonly kind: 'syntax' | 'unsupported' | 'name' | 'limit' | 'memory' | 'fault';
  /** From 1; 0 when there is no position. */
  readonly line: number;
  /** From 1, in bytes. */
  readonly column: number;
  readonly offset: number;
  /** The library's message. */
  readonly reason: string;
  /** The name or the token concerned, at most 64 bytes, or "". */
  readonly subject: string;
  /** A fault's frames: where the run faulted. */
  readonly frames: readonly Frame[];
}
