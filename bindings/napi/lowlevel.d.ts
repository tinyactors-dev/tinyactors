/** Exact C names. Counts/sentinels and native value rooting are supplied by Node. */
declare const opaque: unique symbol;
export interface SystemHandle {
  readonly [opaque]: "System";
}
export interface ArenaHandle {
  readonly [opaque]: "Arena";
}
export interface DefinitionHandle {
  readonly [opaque]: "Definition";
}
export interface ContextHandle {
  readonly [opaque]: "Context";
}
export interface MemoryHandle {
  readonly [opaque]: "Memory";
}
export interface CursorHandle {
  readonly [opaque]: "Cursor";
}
export type Integer = bigint | number;
export interface Result {
  status: number;
}
export type Output<K extends string, T> = Result & { [P in K]: T | null };
export interface Layout {
  size: number;
  alignment: number;
}
export interface ArenaOptions {
  initial_chunk_bytes: number;
  maximum_chunk_bytes: number;
  maximum_bytes: number;
  maximum_allocation: number;
}
export interface MemoryStats {
  reserved_bytes: number;
  peak_reserved_bytes: number;
  used_bytes: number;
  chunk_count: number;
  descendant_count: number;
  failed_allocations: bigint;
}
export interface ActorLimits {
  maximum_bytes?: number;
  external_events?: number;
  internal_events?: number;
  queued_event_bytes?: number;
  delayed_sends?: number;
  invocations?: number;
  io_requests?: number;
  io_request_bytes?: number;
  microsteps_per_macrostep?: number;
  actions_per_macrostep?: number;
}
export interface SystemLimits {
  maximum_bytes?: number;
  actors?: number;
  definitions?: number;
  names?: number;
  queued_events?: number;
  delayed_sends?: number;
  invocations?: number;
  io_requests?: number;
  definition_states?: number;
  definition_transitions?: number;
  definition_actions?: number;
  state_depth?: number;
  action_depth?: number;
  event_name_bytes?: number;
  trace_metadata_bytes?: number;
}
export interface TraceContext {
  trace_id: Uint8Array;
  span_id: Uint8Array;
  trace_flags?: number;
  remote?: boolean | number;
  tracestate?: string | null;
  baggage?: string | null;
}
export interface Target {
  kind: number;
  instance?: Integer;
  text?: string | null;
}
export type Payload = { kind: number; native?: any; bytes?: Uint8Array };
export interface Parameter {
  name: string;
  value: any;
}
export interface Body {
  kind: number;
  content?: Payload;
  parameters?: readonly Parameter[];
}
export interface Message {
  name: string;
  data?: Payload;
  trace?: TraceContext | null;
}
export interface Event extends Message {
  type?: number;
  sendid?: string | null;
  origin?: string | null;
  origintype?: string | null;
  invokeid?: string | null;
  message_id?: bigint;
}
export interface TextExpression {
  literal?: string | null;
  expression?: Integer;
}
export interface ParameterExpression {
  name: string;
  expression?: Integer;
  location?: Integer;
}
export interface BodyExpression {
  kind?: number;
  content?: Integer;
  bytes_content?: Integer;
  parameters?: readonly ParameterExpression[];
}
export interface Action {
  kind: number;
  [field: string]: any;
}
export interface Block {
  actions?: readonly Action[];
}
export interface DataDeclaration {
  id: string;
  initializer?: Integer;
}
export interface Invoke {
  type?: TextExpression;
  source?: TextExpression;
  inline_definition?: DefinitionHandle;
  body?: BodyExpression;
  /** Returned descriptions include a retained handle and diagnostic child metadata. */
  inline_revision?: bigint;
  inline_description?: Description;
  id?: string;
  idlocation?: Integer;
  autoforward?: number | boolean;
  finalize_kind?: number;
  finalize?: Block;
}
export interface DataLayout {
  storage?: number;
  bytes_layout?: Layout;
  initial_bytes?: Uint8Array;
  schema?: string | null;
}
export interface ChartState {
  id: string;
  kind?: number;
  initial?: { target?: string; content?: Block };
  states?: readonly ChartState[];
  transitions?: readonly {
    kind?: number;
    event?: string;
    condition?: Integer;
    target?: string;
    content?: Block;
  }[];
  onentry?: readonly Block[];
  onexit?: readonly Block[];
  data?: readonly DataDeclaration[];
  invokes?: readonly Invoke[];
  done_data?: BodyExpression;
}
export interface Chart {
  name: string;
  data_model?: string;
  code_owner?: any;
  data_layout?: DataLayout;
  binding?: number;
  initial?: { target?: string; content?: Block };
  states: readonly ChartState[];
  data?: readonly DataDeclaration[];
  root_script?: Integer;
}
export interface StateDescription {
  name: string;
  parent: number;
  kind: number;
  initial?: { targets?: { states: readonly number[] }; content?: Block };
  first_transition?: number;
  transition_count?: number;
  onentry?: readonly Block[];
  onexit?: readonly Block[];
  data?: readonly DataDeclaration[];
  invokes?: readonly Invoke[];
  done_data?: BodyExpression;
}
export interface TransitionDescription {
  source: number;
  kind?: number;
  events?: readonly string[];
  condition?: Integer;
  targets?: { states: readonly number[] };
  content?: Block;
}
export interface Description {
  name: string;
  data_model?: string;
  code_owner?: any;
  data_layout?: DataLayout;
  binding?: number;
  states: readonly StateDescription[];
  transitions?: readonly TransitionDescription[];
  root_script?: Integer;
}
export interface Diagnostic {
  status: number;
  actor: bigint;
  revision: bigint;
  state: number;
  transition: number;
  detail: string | null;
}
export interface Termination {
  actor: bigint;
  status: number;
  reason: number;
}
export interface DataView {
  storage: number;
  native?: any;
  bytes?: Uint8Array;
}
export interface SystemValues {
  sessionid: string;
  name: string;
  actor: bigint;
  revision: bigint;
  event?: Event;
  ioprocessors: { type: string; location: string | null }[];
}
export interface ContextView {
  actor: bigint;
  revision: bigint;
  code_owner: any;
  state: DataView;
  system: SystemValues;
  phase: number;
  state_id: number;
  transition_id: number;
  now_ns: bigint;
  trace: TraceContext | null;
}
export interface ActorInfo {
  status: number;
  reason: number;
  revision: bigint;
  sessionid: string;
  registered_name: string | null;
  state: DataView;
  stable: boolean;
  external_events: number;
  internal_events: number;
  delayed_sends: number;
  invocations: number;
  memory: MemoryStats;
}
export interface SystemStats {
  memory: MemoryStats;
  actors: number;
  runnable_actors: number;
  definitions: number;
  names: number;
  queued_events: number;
  delayed_sends: number;
  invocations: number;
  io_requests: number;
  accepted_messages: bigint;
  rejected_messages: bigint;
  completed_macrosteps: bigint;
  microsteps: bigint;
  faulted_actors: bigint;
  dropped_trace_records: bigint;
}
export interface DataBinding {
  owner: number;
  declaration: DataDeclaration;
  override_value: any;
  has_override: boolean;
}
export interface DataHooks {
  create?(context: ContextHandle, seed: any): any;
  declare?(context: ContextHandle, binding: DataBinding): void | number;
  initialize?(context: ContextHandle, binding: DataBinding): void | number;
  bind_system?(context: ContextHandle, values: SystemValues): void | number;
  bind_event?(context: ContextHandle, event: Event): void | number;
  evaluate?(context: ContextHandle, code: bigint): any;
  evaluate_bytes?(context: ContextHandle, code: bigint): Uint8Array;
  test?(context: ContextHandle, code: bigint): boolean;
  read_location?(context: ContextHandle, code: bigint): any;
  assign?(context: ContextHandle, code: bigint, value: any): void | number;
  to_text?(context: ContextHandle, value: any): string;
  from_text?(text: string): any;
  to_delay?(context: ContextHandle, value: any): Integer;
  run?(context: ContextHandle, kind: number, code: bigint): void | number;
  iteration_open?(
    context: ContextHandle,
    code: bigint,
  ): { snapshot: any; count: number };
  iteration_bind?(
    context: ContextHandle,
    step: {
      snapshot: any;
      ordinal: number;
      item: string;
      index: string | null;
    },
  ): void | number;
  pack_body?(body: Body): any;
  finalize?(
    context: ContextHandle,
    input: { parameters: ParameterExpression[]; returned_data: Payload },
  ): void | number;
  log?(record: {
    context: Omit<ContextView, "code_owner" | "revision">;
    label: string;
    value: any;
  }): void;
}
export interface IOProcessor {
  type: string;
  aliases?: readonly string[];
  location?: (request: {
    actor: bigint;
    sessionid: string;
  }) => string | null | undefined;
}
export interface Service {
  type: string;
  aliases?: readonly string[];
  event_processor: string;
}
export interface SpanStart {
  operation: number;
  kind: number;
  actor: bigint;
  revision: bigint;
  message_id: bigint;
  event_name: string | null;
  destination: string | null;
  processor: string | null;
  now_ns: bigint;
  parent: TraceContext | null;
  links: TraceContext[];
}
export interface TraceHooks {
  start(input: SpanStart): { handle?: any; context?: TraceContext };
  error?(handle: any, diagnostic: Diagnostic): void;
  end(input: {
    handle: any;
    context: TraceContext;
    now_ns: bigint;
    status: number;
  }): void;
}
export interface SystemConfig {
  abi_version?: number;
  struct_size?: number;
  session_prefix?: string;
  data_model?: string;
  host_user?: any;
  data?: DataHooks;
  snapshot?: (value: any) => any;
  validate_code?: (
    description: Description,
  ) => number | { status: number; error?: Partial<Diagnostic> };
  diagnostic?: (diagnostic: Diagnostic) => void;
  terminal?: (termination: Termination) => void;
  trace?: TraceHooks;
  ioprocessors?: readonly IOProcessor[];
  services?: readonly Service[];
  arenas?: readonly (ArenaOptions | null)[];
  limits?: SystemLimits;
  actor_defaults?: ActorLimits;
  marshal_limits?: { nodes?: number; depth?: number; bytes?: number };
}
export interface RunBudget {
  work_units: number;
  actor_quantum: number;
}
export interface RunResult {
  work_units: number;
  microsteps: number;
  external_events: number;
  terminal_actors: number;
  runnable: boolean;
  io_ready: boolean;
  next_deadline_ns: bigint;
}
export interface IORequest {
  request: bigint;
  kind: number;
  actor: bigint;
  revision: bigint;
  invocation: bigint;
  invokeid: string | null;
  type: string;
  target: string | null;
  event: Event;
  body: Body;
  trace: TraceContext | null;
  accepted_ns: bigint;
  due_ns: bigint;
}
export interface MigrationState {
  from: number;
  to: number;
  data_initialized: boolean;
}
export interface Migration {
  actor: bigint;
  from: Description;
  to: Description;
  old_state: DataView;
  new_bytes: Uint8Array;
  old_configuration: number[];
  new_configuration: number[];
  state_map: MigrationState[];
  system: SystemValues;
}
export interface InspectRecord {
  kind: number;
  actor: bigint;
  revision: bigint;
  info?: ActorInfo;
  active?: { id: number; name: string; kind: number };
  history?: { id: number; recorded: boolean; target: number; ordinal: number };
  event?: Event;
}

export function ta_text_from_cstr(text: string | null): string | null;
export function ta_arena_create(
  allocator: null,
  options: ArenaOptions,
): Output<"arena", ArenaHandle>;
export function ta_arena_open(
  parent: ArenaHandle,
  options: ArenaOptions,
): Output<"arena", ArenaHandle>;
export function ta_arena_alloc(
  arena: ArenaHandle,
  layout: Layout,
): Output<"memory", MemoryHandle>;
export function ta_arena_defer(
  arena: ArenaHandle,
  cleanup: (argument: any) => void,
  argument?: any,
): number;
export function ta_arena_close(arena: ArenaHandle): void;
export function ta_arena_stats(arena: ArenaHandle): MemoryStats;
export function ta_arena_set_limit(
  arena: ArenaHandle,
  maximum_bytes: number,
): number;
export function ta_system_config_init(): SystemConfig;
export function ta_system_create(
  parent: ArenaHandle | null,
  config: SystemConfig,
): Output<"system", SystemHandle>;
export function ta_system_set_limits(
  system: SystemHandle,
  limits: SystemLimits,
): number;
export function ta_definition_build(
  system: SystemHandle,
  chart: Chart,
): Output<"definition", DefinitionHandle> & { error: Diagnostic | null };
export function ta_definition_prepare(
  system: SystemHandle,
  input: {
    description: Description;
    options?: {
      storage?: number;
      storage_release?: (user: any) => void;
      storage_user?: any;
    };
  },
): Output<"definition", DefinitionHandle> & { error: Diagnostic | null };
export function ta_definition_retain(definition: DefinitionHandle): void;
export function ta_definition_release(definition: DefinitionHandle): void;
export function ta_definition_revision(definition: DefinitionHandle): bigint;
export function ta_definition_description(
  definition: DefinitionHandle,
): Description;
export function ta_definition_footprint(definition: DefinitionHandle): {
  control_bytes: number;
  state_bytes: number;
  minimum_chunk_bytes: number;
};
export function ta_definition_publish(
  system: SystemHandle,
  binding: { key: string; expected_revision?: Integer },
  definition: DefinitionHandle,
): number;
export function ta_definition_unpublish(
  system: SystemHandle,
  binding: { key: string; expected_revision?: Integer },
): number;
export function ta_definition_lookup(
  system: SystemHandle,
  key: string,
): Output<"definition", DefinitionHandle>;
export function ta_actor_spawn(
  system: SystemHandle,
  options: {
    definition?: DefinitionHandle;
    definition_key?: string;
    seed?: any;
    initial_bytes?: Uint8Array;
    parameters?: readonly Parameter[];
    name?: string;
    limits?: ActorLimits;
    trace?: TraceContext;
  },
): Result & { actor: bigint };
export function ta_actor_inspect(
  system: SystemHandle,
  actor: Integer,
): Output<"info", ActorInfo>;
export function ta_actor_set_limits(
  system: SystemHandle,
  actor: Integer,
  limits: ActorLimits,
): number;
export function ta_actor_stop(system: SystemHandle, actor: Integer): number;
export function ta_actor_release(system: SystemHandle, actor: Integer): number;
export function ta_actor_upgrade(
  system: SystemHandle,
  options: {
    actor: Integer;
    replacement: DefinitionHandle;
    expected_revision?: Integer;
    migrate?: (migration: Migration) => any;
  },
): number;
export function ta_system_inspect(
  system: SystemHandle,
  options: {
    actor?: Integer;
    revision?: Integer;
    stable_only?: boolean;
    details?: number;
    work_units: number;
    visit: (record: InspectRecord) => void | number;
  },
  cursor?: CursorHandle,
): Result & { cursor: CursorHandle };
export function ta_name_bind(
  system: SystemHandle,
  binding: { name: string; expected?: Integer },
  replacement: Integer,
): number;
export function ta_name_unbind(
  system: SystemHandle,
  binding: { name: string; expected?: Integer },
): number;
export function ta_name_lookup(
  system: SystemHandle,
  name: string,
): Result & { actor: bigint };
export function ta_session_lookup(
  system: SystemHandle,
  sessionid: string,
): Result & { actor: bigint };
export function ta_target_parse(text: string | null): Output<"target", Target>;
export function ta_post(
  system: SystemHandle,
  options: { target: Target; message: Message },
): Result & { message_id: bigint };
export function ta_context_inspect(context: ContextHandle): ContextView;
export function ta_context_in(context: ContextHandle, state: string): boolean;
export function ta_context_host_user(context: ContextHandle): any;
export function ta_context_bytes(
  context: ContextHandle,
): Output<"bytes", Uint8Array>;
export function ta_context_arena(
  context: ContextHandle,
  lifetime: number,
): Output<"arena", ArenaHandle>;
export function ta_context_raise(
  context: ContextHandle,
  event: string,
  data?: Payload,
): number;
export function ta_context_send(
  context: ContextHandle,
  send: {
    event: string;
    type?: string;
    target?: Target;
    body?: Body;
    delay_ns?: Integer;
    sendid?: string;
  },
): Result & { message_id: bigint };
export function ta_context_cancel(
  context: ContextHandle,
  sendid: string,
): number;
export function ta_system_run(
  system: SystemHandle,
  options: { now_ns: Integer; budget: RunBudget },
): Output<"result", RunResult>;
export function ta_system_next_deadline(system: SystemHandle): bigint;
export function ta_system_stats(system: SystemHandle): SystemStats;
export function ta_system_shutdown(system: SystemHandle): number;
export function ta_system_destroy(system: SystemHandle): number;
export function ta_io_next(system: SystemHandle): Output<"request", IORequest>;
export function ta_io_complete(
  system: SystemHandle,
  request: Integer,
  status: number,
): number;
export function ta_io_receive(
  system: SystemHandle,
  route: { processor: string; destination: Integer },
  event: Event,
): number;
export function ta_io_receive_error(
  system: SystemHandle,
  route: { processor: string; destination: Integer },
  sendid: string,
): number;
export function ta_invocation_emit(
  system: SystemHandle,
  invocation: Integer,
  message: Message,
): number;
export function ta_invocation_done(
  system: SystemHandle,
  invocation: Integer,
  body: Body,
): number;
export function ta_invocation_fail(
  system: SystemHandle,
  invocation: Integer,
): number;
export function ta_invocation_is_cancelled(
  system: SystemHandle,
  invocation: Integer,
): Result & { cancelled: boolean };
export function memoryRead(memory: MemoryHandle): Uint8Array;
export function memoryWrite(memory: MemoryHandle, bytes: Uint8Array): void;
export function contextWriteBytes(
  context: ContextHandle,
  bytes: Uint8Array,
): number;
export function takeErrors(system: SystemHandle): unknown[];
/** Explicitly abandons core leases. Stop real transports/services before use. */
export function disposeSystem(system: SystemHandle): void;
export function bindingStats(system: SystemHandle): {
  native_roots: number;
  taken_leases: number;
  callback_errors: number;
};

export const TA_ABI_VERSION: number;
export const TA_INDEX_NONE: number;
export const TA_REVISION_ANY: bigint;
export const TA_TIME_NEVER: bigint;
export const TA_UNLIMITED: number;
export const TA_OK: number;
export const TA_EMPTY: number;
export const TA_AGAIN: number;
export const TA_INVALID: number;
export const TA_UNSUPPORTED: number;
export const TA_NOT_FOUND: number;
export const TA_STALE: number;
export const TA_CONFLICT: number;
export const TA_BUSY: number;
export const TA_NO_MEMORY: number;
export const TA_LIMIT: number;
export const TA_EXECUTION_ERROR: number;
export const TA_COMMUNICATION_ERROR: number;
export const TA_CANCELLED: number;
export const TA_FAULTED: number;
export const TA_DATA_NATIVE: number;
export const TA_DATA_BYTES: number;
export const TA_PAYLOAD_NONE: number;
export const TA_PAYLOAD_NATIVE: number;
export const TA_PAYLOAD_BYTES: number;
export const TA_BODY_NONE: number;
export const TA_BODY_CONTENT: number;
export const TA_BODY_PARAMETERS: number;
export const TA_EVENT_EXTERNAL: number;
export const TA_EVENT_INTERNAL: number;
export const TA_EVENT_PLATFORM: number;
export const TA_TARGET_SELF: number;
export const TA_TARGET_INTERNAL: number;
export const TA_TARGET_INSTANCE: number;
export const TA_TARGET_NAME: number;
export const TA_TARGET_PARENT: number;
export const TA_TARGET_INVOKE: number;
export const TA_TARGET_URI: number;
export const TA_ACTION_RAISE: number;
export const TA_ACTION_ASSIGN: number;
export const TA_ACTION_SCRIPT: number;
export const TA_ACTION_IF: number;
export const TA_ACTION_FOREACH: number;
export const TA_ACTION_LOG: number;
export const TA_ACTION_SEND: number;
export const TA_ACTION_CANCEL: number;
export const TA_ACTION_CUSTOM: number;
export const TA_TRANSITION_EXTERNAL: number;
export const TA_TRANSITION_INTERNAL: number;
export const TA_BINDING_EARLY: number;
export const TA_BINDING_LATE: number;
export const TA_FINALIZE_ABSENT: number;
export const TA_FINALIZE_AUTOMATIC: number;
export const TA_FINALIZE_BLOCK: number;
export const TA_IO_SEND: number;
export const TA_IO_INVOKE_START: number;
export const TA_IO_INVOKE_CANCEL: number;
export const TA_STATE_ROOT: number;
export const TA_STATE_ATOMIC: number;
export const TA_STATE_COMPOUND: number;
export const TA_STATE_PARALLEL: number;
export const TA_STATE_FINAL: number;
export const TA_STATE_HISTORY_SHALLOW: number;
export const TA_STATE_HISTORY_DEEP: number;
export const TA_LIFETIME_SYSTEM: number;
export const TA_LIFETIME_DEFINITION: number;
export const TA_LIFETIME_ACTOR: number;
export const TA_LIFETIME_ACTIVATION: number;
export const TA_LIFETIME_MESSAGE: number;
export const TA_LIFETIME_MACROSTEP: number;
export const TA_LIFETIME_IO: number;
export const TA_LIFETIME_TRACE: number;
export const TA_LIFETIME_UPGRADE: number;
export const TA_LIFETIME_COUNT: number;
export const TA_CHART_STATE: number;
export const TA_CHART_PARALLEL: number;
export const TA_CHART_FINAL: number;
export const TA_CHART_HISTORY_SHALLOW: number;
export const TA_CHART_HISTORY_DEEP: number;
export const TA_ACTOR_STARTING: number;
export const TA_ACTOR_READY: number;
export const TA_ACTOR_SUSPENDED: number;
export const TA_ACTOR_STOPPING: number;
export const TA_ACTOR_DONE: number;
export const TA_ACTOR_STOPPED: number;
export const TA_ACTOR_FAILED: number;
export const TA_INSPECT_ACTOR: number;
export const TA_INSPECT_ACTIVE_STATE: number;
export const TA_INSPECT_HISTORY_STATE: number;
export const TA_INSPECT_CURRENT_EVENT: number;
export const TA_INSPECT_DATA: number;
export const TA_INSPECT_ACTIVE: number;
export const TA_INSPECT_HISTORY: number;
export const TA_INSPECT_EVENT: number;
export const TA_VISIT_CONTINUE: number;
export const TA_VISIT_PAUSE: number;
export const TA_VISIT_STOP: number;
export const TA_SPAN_SEND: number;
export const TA_SPAN_PROCESS: number;
export const TA_SPAN_INVOKE: number;
export const TA_SPAN_UPGRADE: number;
export const TA_SPAN_INTERNAL: number;
export const TA_SPAN_PRODUCER: number;
export const TA_SPAN_CONSUMER: number;
export const TA_SPAN_CLIENT: number;
export const TA_PHASE_INITIALIZE: number;
export const TA_PHASE_ROOT_SCRIPT: number;
export const TA_PHASE_GUARD: number;
export const TA_PHASE_ONENTRY: number;
export const TA_PHASE_ONEXIT: number;
export const TA_PHASE_TRANSITION: number;
export const TA_PHASE_FINALIZE: number;
export const TA_PHASE_INVOKE: number;
export const TA_PHASE_DONE_DATA: number;
export const TA_PHASE_MIGRATE: number;
export const TA_DEFINITION_COPY: number;
export const TA_DEFINITION_BORROW: number;
export const TA_SCXML_IOPROCESSOR: string;
export const TA_SCXML_INVOKE_TYPE: string;
export const TA_BASIC_HTTP_IOPROCESSOR: string;
