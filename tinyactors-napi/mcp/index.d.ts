import type { Server } from '@modelcontextprotocol/sdk/server/index.js';
import type { DecodedSpan, Host, HostOptions, Invocation, MigrationResult } from '@tinyactors/node';

/** What a tap keeps of what the host's handlers saw. */
export interface TapState {
  /** The options given to {@link Tap.options}, for copies of the host. */
  given: HostOptions | undefined;
  /** The spans the host exported, newest last. */
  spans: DecodedSpan[];
  /** The `<log>` lines, newest last. */
  logs: { session: bigint; label: string; text: string }[];
  /** The migrations' results, newest last. */
  migrations: MigrationResult[];
  /** Called with the next migration's result. */
  migrationWaiters: ((result: MigrationResult) => void)[];
  /** The running services, by invoking session. */
  services: Map<bigint, Invocation[]>;
  limits: { spans: number; logs: number; migrations: number };
}

export interface Tap {
  state: TapState;
  /** `given` with each handler wrapped: pass the result to `new Host(charts, ...)`. */
  options<T extends HostOptions>(given?: T): T;
}

/** A tap for a host: how many spans, log lines and migration results it keeps. */
export function tap(limits?: { spans?: number; logs?: number; migrations?: number }): Tap;

export interface ServedSystem {
  host: Host;
  /** The tap whose options made the host: traces, logs, services, migrations and copies of snapshots need it. */
  tap?: Tap;
  /** Each chart's SCXML by its name; `deploy` writes the draft's here. */
  sources?: Record<string, string>;
}

export interface MCPOptions {
  /** Allow `deploy` and `end_service`. */
  allowDeploy?: boolean;
  /** Allow `terminate`. */
  allowTerminate?: boolean;
  /** How many snapshots the server keeps (8). */
  snapshotKeep?: number;
}

export interface Served {
  url: string;
  close(): Promise<void>;
}

export interface MCP {
  /** A new MCP server over the systems, for one connection; every server shares the snapshots and drafts. */
  server(): Server;
  /** Serves Streamable HTTP at `http://host:port/path`, a server per MCP session. */
  listen(options?: { port?: number; host?: string; path?: string; token?: string }): Promise<Served>;
}

export function createMCP(systems: Record<string, ServedSystem>, options?: MCPOptions): MCP;

/** A tool of the catalog (`@tinyactors/node/mcp-tools.json`, core's `tinyactors-core/mcp/tools.json`). */
export interface CatalogTool {
  name: string;
  description: string;
  inputSchema: { type: 'object'; properties: Record<string, Record<string, unknown>>; required?: string[] };
  annotations: Record<string, boolean>;
  /** The server option that must be set for the tool to run. */
  gate?: 'allowDeploy' | 'allowTerminate';
  /** A method of core's snapshot reader, core's requests through the typed methods, or the binding's own operation. */
  run: { snapshot: string } | { host: string | Record<string, string> } | { binding: string };
}

/** A tool's arguments checked against its schema, its defaults filled in; a TypeError for others. */
export function argumentsOf(tool: CatalogTool, given?: Record<string, unknown>): Record<string, unknown>;

/** A migration's result for a model: failures grouped by reason, five sessions of each. */
export function migrationSummary(result: MigrationResult): {
  migration: number;
  migrated: number;
  failed: Record<string, { count: number; sample: bigint[] }>;
  pending: number;
};
