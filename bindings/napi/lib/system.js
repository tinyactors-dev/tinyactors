"use strict";
const ll = require("../lowlevel");
const clone = structuredClone;
const statusNames = [
  "OK",
  "EMPTY",
  "AGAIN",
  "INVALID",
  "UNSUPPORTED",
  "NOT_FOUND",
  "STALE",
  "CONFLICT",
  "BUSY",
  "NO_MEMORY",
  "LIMIT",
  "EXECUTION_ERROR",
  "COMMUNICATION_ERROR",
  "CANCELLED",
  "FAULTED",
];
const actorStatuses = [
  "starting",
  "ready",
  "suspended",
  "stopping",
  "done",
  "stopped",
  "failed",
];

class TinyActorsError extends Error {
  constructor(operation, result) {
    const status = typeof result === "number" ? result : result.status;
    super(
      operation +
        ": " +
        (statusNames[status] || status) +
        (result.error?.detail ? " — " + result.error.detail : ""),
    );
    this.name = "TinyActorsError";
    this.status = status;
    this.result = result;
  }
}
function checked(result, operation) {
  if ((typeof result === "number" ? result : result.status) !== ll.TA_OK)
    throw new TinyActorsError(operation, result);
  return result;
}
function synchronous(value) {
  if (value && typeof value.then === "function")
    throw new TypeError(
      "Actor functions must be synchronous; model asynchronous work with sends or invokes.",
    );
  return value;
}
function payload(data) {
  return data === undefined
    ? { kind: ll.TA_PAYLOAD_NONE }
    : { kind: ll.TA_PAYLOAD_NATIVE, native: data };
}
function body(data) {
  return data === undefined
    ? { kind: ll.TA_BODY_NONE }
    : { kind: ll.TA_BODY_CONTENT, content: payload(data) };
}
function eventInput(event, data) {
  const input = typeof event === "string" ? { type: event, data } : event;
  if (!input || typeof input.type !== "string" || !input.type)
    throw new TypeError("An event needs a nonempty type string.");
  return { name: input.type, data: payload(input.data), trace: input.trace };
}
function unpack(value) {
  if (!value || value.kind === ll.TA_PAYLOAD_NONE) return undefined;
  return value.kind === ll.TA_PAYLOAD_NATIVE ? value.native : value.bytes;
}
function snapshotPayload(value, snapshot) {
  return value?.kind === ll.TA_PAYLOAD_NATIVE
    ? { ...value, native: synchronous(snapshot(value.native)) }
    : value;
}
function eventView(event) {
  return event
    ? {
        type: event.name,
        data: unpack(event.data),
        origin: event.origin,
        originType: event.origintype,
        sendId: event.sendid,
        invokeId: event.invokeid,
        messageId: event.message_id,
        trace: event.trace,
      }
    : undefined;
}
function target(value) {
  if (value == null || value === "self") return { kind: ll.TA_TARGET_SELF };
  if (typeof value === "bigint")
    return { kind: ll.TA_TARGET_INSTANCE, instance: value };
  if (value instanceof Actor)
    return { kind: ll.TA_TARGET_INSTANCE, instance: value.id };
  if (typeof value === "string") {
    if (value === "internal" || value === "#_internal")
      return { kind: ll.TA_TARGET_INTERNAL };
    if (value === "parent" || value === "#_parent")
      return { kind: ll.TA_TARGET_PARENT };
    return {
      kind:
        value.startsWith("#") || value.includes(":")
          ? ll.TA_TARGET_URI
          : ll.TA_TARGET_NAME,
      text: value,
    };
  }
  if (typeof value === "object" && typeof value.kind === "number") return value;
  throw new TypeError(
    "Send target must be an actor, BigInt, name, SCXML address, or target descriptor.",
  );
}
function nanoseconds(options) {
  if (options.delayNs !== undefined) return options.delayNs;
  if (options.delayMs !== undefined) {
    if (!Number.isSafeInteger(options.delayMs) || options.delayMs < 0)
      throw new RangeError("delayMs must be a nonnegative safe integer.");
    return BigInt(options.delayMs) * 1000000n;
  }
  return 0n;
}
function limits(input) {
  return Object.fromEntries(
    Object.entries(input || {}).map(([key, value]) => [
      key.replace(/[A-Z]/g, (c) => "_" + c.toLowerCase()),
      value,
    ]),
  );
}
function stateView(info, id, snapshot = clone) {
  return {
    id,
    status: actorStatuses[info.status],
    reason: info.reason,
    revision: info.revision,
    sessionId: info.sessionid,
    name: info.registered_name,
    stable: info.stable,
    state: synchronous(
      snapshot(
        info.state.storage === ll.TA_DATA_NATIVE
          ? info.state.native
          : info.state.bytes,
      ),
    ),
    pending: {
      external: info.external_events,
      internal: info.internal_events,
      timers: info.delayed_sends,
      invocations: info.invocations,
    },
    memory: info.memory,
  };
}
function executionContext(context, extra = {}) {
  const view = ll.ta_context_inspect(context);
  const alive = () => {
    ll.ta_context_host_user(context);
  };
  const api = {
    get state() {
      alive();
      return view.state.native;
    },
    get event() {
      alive();
      return eventView(view.system?.event);
    },
    get self() {
      alive();
      return view.actor;
    },
    get now() {
      alive();
      return view.now_ns;
    },
    get system() {
      alive();
      return view.system;
    },
    in(name) {
      return ll.ta_context_in(context, name);
    },
    send(event, options = {}) {
      const destination = options.to ?? options.target;
      if (
        destination instanceof Actor &&
        destination.system !== ll.ta_context_host_user(context)
      )
        throw new TypeError("Actor belongs to another system.");
      const msg = eventInput(event, options.data);
      return checked(
        ll.ta_context_send(context, {
          event: msg.name,
          type: options.processor,
          target: target(options.to ?? options.target),
          body: body(unpack(msg.data)),
          delay_ns: nanoseconds(options),
          sendid: options.id,
        }),
        "context.send",
      ).message_id;
    },
    raise(event, data) {
      const msg = eventInput(event, data);
      checked(
        ll.ta_context_raise(context, msg.name, msg.data),
        "context.raise",
      );
    },
    cancel(id) {
      checked(ll.ta_context_cancel(context, id), "context.cancel");
    },
  };
  return Object.assign(api, extra);
}
function evaluate(context, code, extra) {
  const owner = ll.ta_context_inspect(context).code_owner;
  const fn = owner?.functions?.[Number(code) - 1];
  if (typeof fn !== "function")
    throw new TypeError("Unknown definition code token: " + code);
  return synchronous(fn(executionContext(context, extra)));
}
function dataHooks() {
  return {
    create(context, input) {
      const view = ll.ta_context_inspect(context),
        owner = view.code_owner;
      const state = synchronous(
        owner.factory(executionContext(context, { input })),
      );
      if (!state || typeof state !== "object")
        throw new TypeError(
          "A machine state factory must return a fresh object.",
        );
      return state;
    },
    run(context, kind, code) {
      try {
        evaluate(context, code, { kind });
      } catch (error) {
        if (
          error instanceof TinyActorsError &&
          error.message.startsWith("context.")
        )
          return error.status;
        throw error;
      }
    },
    test(context, code) {
      return !!evaluate(context, code);
    },
    evaluate(context, code) {
      return evaluate(context, code);
    },
    read_location(context, code) {
      return evaluate(context, code);
    },
    assign(context, code, value) {
      evaluate(context, code, { value });
    },
    to_text(context, value) {
      return String(value);
    },
    to_delay(context, value) {
      return value;
    },
    declare(context, binding) {
      const state = ll.ta_context_inspect(context).state.native;
      Object.defineProperty(state, binding.declaration.id, {
        value: undefined,
        writable: true,
        enumerable: true,
        configurable: true,
      });
    },
    initialize(context, binding) {
      const state = ll.ta_context_inspect(context).state.native,
        code = binding.declaration.initializer;
      state[binding.declaration.id] = binding.has_override
        ? binding.override_value
        : code
          ? evaluate(context, code)
          : undefined;
    },
    evaluate_bytes(context, code) {
      return evaluate(context, code);
    },
    iteration_open(context, code) {
      const snapshot = Array.from(evaluate(context, code));
      return { snapshot, count: snapshot.length };
    },
    iteration_bind(context, step) {
      const state = ll.ta_context_inspect(context).state.native;
      state[step.item] = step.snapshot[step.ordinal];
      if (step.index) state[step.index] = step.ordinal;
    },
    finalize(context, input) {
      const result = unpack(input.returned_data);
      for (const parameter of input.parameters)
        if (
          parameter.location &&
          result &&
          Object.hasOwn(result, parameter.name)
        ) {
          evaluate(context, parameter.location, {
            value: result[parameter.name],
          });
        }
    },
  };
}

class Actor {
  constructor(system, id) {
    Object.defineProperties(this, {
      system: { value: system, enumerable: true },
      id: { value: id, enumerable: true },
    });
  }
  send(event, data) {
    return this.system._post(
      { kind: ll.TA_TARGET_INSTANCE, instance: this.id },
      eventInput(event, data),
    );
  }
  stop() {
    checked(ll.ta_actor_stop(this.system.handle, this.id), "actor.stop");
    this.system._wake();
  }
  release() {
    checked(ll.ta_actor_release(this.system.handle, this.id), "actor.release");
  }
  snapshot(options = {}) {
    const info = checked(
      ll.ta_actor_inspect(this.system.handle, this.id),
      "actor.snapshot",
    ).info;
    const out = stateView(info, this.id, this.system._snapshot);
    if (options.active !== false) {
      out.active = [];
      this.system.inspect(
        (record) => {
          if (record.kind === "active" && record.name !== "$root")
            out.active.push(record.name);
        },
        { actor: this, details: ll.TA_INSPECT_ACTIVE, workUnits: Infinity },
      );
    }
    return out;
  }
  inspect(options) {
    return this.snapshot(options);
  }
  upgrade(machine, options) {
    return this.system.upgrade(this, machine, options);
  }
}

class Lease {
  constructor(system, request) {
    this.system = system;
    // The bridge already copies metadata/bytes. Only native payloads alias roots.
    this.request = {
      ...request,
      event: {
        ...request.event,
        data: snapshotPayload(request.event.data, system._snapshot),
      },
      body: {
        ...request.body,
        content: snapshotPayload(request.body.content, system._snapshot),
        parameters: request.body.parameters?.map((item) => ({
          ...item,
          value: synchronous(system._snapshot(item.value)),
        })),
      },
    };
    this.completed = false;
  }
  complete(status = ll.TA_OK) {
    if (this.completed)
      throw new TinyActorsError("lease.complete", ll.TA_STALE);
    checked(
      ll.ta_io_complete(this.system.handle, this.request.request, status),
      "lease.complete",
    );
    this.completed = true;
    this.system._wake();
  }
  emit(event, data) {
    checked(
      ll.ta_invocation_emit(
        this.system.handle,
        this.request.invocation,
        eventInput(event, data),
      ),
      "invocation.emit",
    );
    this.system._wake();
  }
  done(data) {
    checked(
      ll.ta_invocation_done(
        this.system.handle,
        this.request.invocation,
        body(data),
      ),
      "invocation.done",
    );
    this.system._wake();
  }
  fail() {
    checked(
      ll.ta_invocation_fail(this.system.handle, this.request.invocation),
      "invocation.fail",
    );
    this.system._wake();
  }
  isCancelled() {
    return checked(
      ll.ta_invocation_is_cancelled(
        this.system.handle,
        this.request.invocation,
      ),
      "invocation.isCancelled",
    ).cancelled;
  }
}

class System {
  constructor(options = {}) {
    this._definitions = new WeakMap();
    this._owned = new Set();
    this._closed = false;
    this._now = 0n;
    this._automatic = null;
    this._scheduled = null;
    const config = { ...(options.config || {}) };
    for (const field of [
      "ioprocessors",
      "services",
      "trace",
      "snapshot",
      "diagnostic",
      "terminal",
    ])
      if (options[field] !== undefined) config[field] = options[field];
    if (options.sessionPrefix !== undefined)
      config.session_prefix = options.sessionPrefix;
    if (options.limits) config.limits = limits(options.limits);
    if (options.actorLimits)
      config.actor_defaults = limits(options.actorLimits);
    config.data = { ...dataHooks(), ...(config.data || {}) };
    config.host_user = this;
    this._snapshot = config.snapshot || clone;
    this.handle = checked(
      ll.ta_system_create(null, config),
      "createSystem",
    ).system;
  }
  prepare(machine) {
    if (!machine?.chart || !machine?.owner)
      throw new TypeError("Expected a machine returned by defineMachine().");
    let definition = this._definitions.get(machine);
    if (!definition) {
      definition = checked(
        ll.ta_definition_build(this.handle, machine.chart),
        "prepare " + machine.id,
      ).definition;
      this._definitions.set(machine, definition);
      this._owned.add(new WeakRef(definition));
      if (this._owned.size % 64 === 0)
        for (const ref of this._owned)
          if (!ref.deref()) this._owned.delete(ref);
    }
    return definition;
  }
  releaseDefinition(machine) {
    const definition = this._definitions.get(machine);
    if (definition) {
      ll.ta_definition_release(definition);
      this._definitions.delete(machine);
    }
  }
  spawn(machine, options = {}) {
    const input = {
      definition: this.prepare(machine),
      name: options.name,
      parameters: options.parameters,
      trace: options.trace,
      limits: options.limits ? limits(options.limits) : undefined,
    };
    if (Object.hasOwn(options, "input")) input.seed = options.input;
    const result = checked(ll.ta_actor_spawn(this.handle, input), "spawn");
    this._wake();
    return new Actor(this, result.actor);
  }
  spawnPublished(key, options = {}) {
    const input = {
      definition_key: key,
      name: options.name,
      trace: options.trace,
      parameters: options.parameters,
      limits: options.limits ? limits(options.limits) : undefined,
    };
    if (Object.hasOwn(options, "input")) input.seed = options.input;
    const result = checked(
      ll.ta_actor_spawn(this.handle, input),
      "spawnPublished",
    );
    this._wake();
    return new Actor(this, result.actor);
  }
  actor(nameOrId) {
    return new Actor(
      this,
      typeof nameOrId === "bigint"
        ? nameOrId
        : checked(ll.ta_name_lookup(this.handle, nameOrId), "actor lookup")
            .actor,
    );
  }
  bind(name, actor, expected = 0n) {
    if (!(actor instanceof Actor) || actor.system !== this)
      throw new TypeError("Actor belongs to another system.");
    if (expected instanceof Actor && expected.system !== this)
      throw new TypeError("Expected actor belongs to another system.");
    checked(
      ll.ta_name_bind(
        this.handle,
        { name, expected: expected instanceof Actor ? expected.id : expected },
        actor.id,
      ),
      "bind",
    );
  }
  unbind(name, expected = 0n) {
    if (expected instanceof Actor && expected.system !== this)
      throw new TypeError("Expected actor belongs to another system.");
    checked(
      ll.ta_name_unbind(this.handle, {
        name,
        expected: expected instanceof Actor ? expected.id : expected,
      }),
      "unbind",
    );
  }
  _post(destination, message) {
    const result = checked(
      ll.ta_post(this.handle, { target: destination, message }),
      "send",
    );
    this._wake();
    return result.message_id;
  }
  send(name, event, data) {
    return this._post(
      { kind: ll.TA_TARGET_NAME, text: name },
      eventInput(event, data),
    );
  }
  run(options = {}) {
    const now = options.nowNs ?? this._now;
    const result = checked(
      ll.ta_system_run(this.handle, {
        now_ns: now,
        budget: options.budget || {
          work_units: options.workUnits ?? 100000,
          actor_quantum: options.actorQuantum ?? 1000,
        },
      }),
      "run",
    ).result;
    this._now = now;
    return {
      workUnits: result.work_units,
      microsteps: result.microsteps,
      externalEvents: result.external_events,
      terminalActors: result.terminal_actors,
      runnable: result.runnable,
      ioReady: result.io_ready,
      nextDeadlineNs: result.next_deadline_ns,
    };
  }
  nextDeadline() {
    return ll.ta_system_next_deadline(this.handle);
  }
  stats() {
    return ll.ta_system_stats(this.handle);
  }
  takeErrors() {
    return ll.takeErrors(this.handle);
  }
  inspect(visit, options = {}) {
    if (typeof visit !== "function")
      throw new TypeError("inspect requires a callback.");
    if (options.actor instanceof Actor && options.actor.system !== this)
      throw new TypeError("Actor belongs to another system.");
    const record = (raw) => {
      let view;
      if (raw.kind === ll.TA_INSPECT_ACTOR)
        view = {
          kind: "actor",
          actor: raw.actor,
          info: stateView(raw.info, raw.actor, this._snapshot),
        };
      else if (raw.kind === ll.TA_INSPECT_ACTIVE_STATE)
        view = {
          ...raw.active,
          stateKind: raw.active.kind,
          kind: "active",
          actor: raw.actor,
        };
      else if (raw.kind === ll.TA_INSPECT_HISTORY_STATE)
        view = { kind: "history", actor: raw.actor, ...raw.history };
      else
        view = {
          kind: "event",
          actor: raw.actor,
          event: eventView({
            ...raw.event,
            data: snapshotPayload(raw.event.data, this._snapshot),
          }),
        };
      const control = synchronous(visit(view));
      if (control === "pause") return ll.TA_VISIT_PAUSE;
      if (control === "stop" || control === false) return ll.TA_VISIT_STOP;
      return control ?? ll.TA_VISIT_CONTINUE;
    };
    const result = ll.ta_system_inspect(
      this.handle,
      {
        actor:
          options.actor instanceof Actor ? options.actor.id : options.actor,
        revision: options.revision,
        stable_only: options.stableOnly,
        details: options.details ?? ll.TA_INSPECT_DATA,
        work_units: options.workUnits ?? 1000,
        visit: record,
      },
      options.cursor,
    );
    if (![ll.TA_OK, ll.TA_AGAIN, ll.TA_CANCELLED].includes(result.status))
      checked(result, "inspect");
    return {
      done: result.status === ll.TA_OK,
      stopped: result.status === ll.TA_CANCELLED,
      cursor: result.cursor,
      status: result.status,
    };
  }
  publish(machine, options = {}) {
    if (typeof options === "string") options = { key: options };
    checked(
      ll.ta_definition_publish(
        this.handle,
        {
          key: options.key ?? machine.id,
          expected_revision: options.expectedRevision ?? ll.TA_REVISION_ANY,
        },
        this.prepare(machine),
      ),
      "publish",
    );
  }
  unpublish(key, expectedRevision = ll.TA_REVISION_ANY) {
    checked(
      ll.ta_definition_unpublish(this.handle, {
        key,
        expected_revision: expectedRevision,
      }),
      "unpublish",
    );
  }
  upgrade(actor, machine, options = {}) {
    if (actor instanceof Actor && actor.system !== this)
      throw new TypeError("Actor belongs to another system.");
    const replacement = this.prepare(machine);
    checked(
      ll.ta_actor_upgrade(this.handle, {
        actor: actor instanceof Actor ? actor.id : actor,
        replacement,
        expected_revision: options.expectedRevision ?? ll.TA_REVISION_ANY,
        migrate: (input) => {
          const state = synchronous(this._snapshot(input.old_state.native));
          const replacementState = synchronous(
            options.migrate
              ? options.migrate({
                  state,
                  from: input.from,
                  to: input.to,
                  stateMap: input.state_map,
                })
              : state,
          );
          if (!replacementState || typeof replacementState !== "object")
            throw new TypeError(
              "Actor migration must return a fresh state object.",
            );
          return replacementState;
        },
      }),
      "upgrade",
    );
    this._wake();
  }
  ioNext() {
    const result = ll.ta_io_next(this.handle);
    if (result.status === ll.TA_EMPTY) return null;
    const request = checked(result, "ioNext").request;
    try {
      return new Lease(this, request);
    } catch (error) {
      checked(
        ll.ta_io_complete(
          this.handle,
          request.request,
          ll.TA_COMMUNICATION_ERROR,
        ),
        "ioNext snapshot failure",
      );
      this._wake();
      throw error;
    }
  }
  async drainIO(handler, options = {}) {
    const maximum = options.limit ?? 64;
    let count = 0;
    if (!Number.isSafeInteger(maximum) || maximum < 0)
      throw new RangeError(
        "I/O drain limit must be a nonnegative safe integer.",
      );
    while (count < maximum) {
      const lease = this.ioNext();
      if (!lease) break;
      count++;
      try {
        const result = await handler(lease.request, lease);
        if (!lease.completed)
          lease.complete(typeof result === "number" ? result : ll.TA_OK);
      } catch (error) {
        if (!lease.completed) lease.complete(ll.TA_COMMUNICATION_ERROR);
        if (options.onError) options.onError(error);
        else throw error;
      }
    }
    return count;
  }
  receive(route, event) {
    if (route.actor instanceof Actor && route.actor.system !== this)
      throw new TypeError("Actor belongs to another system.");
    const message = eventInput(event);
    checked(
      ll.ta_io_receive(
        this.handle,
        {
          processor: route.processor,
          destination:
            route.actor instanceof Actor ? route.actor.id : route.actor,
        },
        {
          ...message,
          origin: event.origin,
          origintype: event.originType,
          sendid: event.sendId,
        },
      ),
      "receive",
    );
    this._wake();
  }
  _cancelScheduled() {
    if (!this._scheduled) return;
    if (this._scheduled.immediate) clearImmediate(this._scheduled.handle);
    else clearTimeout(this._scheduled.handle);
    this._scheduled = null;
  }
  _schedule(delay) {
    if (!this._automatic || this._closed) return;
    this._cancelScheduled();
    const immediate = delay === 0;
    const handle = immediate
      ? setImmediate(() => this._tick())
      : setTimeout(() => this._tick(), delay);
    if (this._automatic.unref) handle.unref();
    this._scheduled = { handle, immediate };
  }
  _wake() {
    if (this._automatic) this._schedule(0);
  }
  _tick() {
    this._scheduled = null;
    const options = this._automatic;
    if (!options) return;
    try {
      const now = options.baseNow + process.hrtime.bigint() - options.baseClock;
      const result = this.run({
        nowNs: now,
        workUnits: options.workUnits ?? 1000,
        actorQuantum: options.actorQuantum ?? 100,
      });
      if (result.ioReady && options.onIO) options.onIO(this);
      if (!this._automatic) return;
      if (result.runnable) this._schedule(0);
      else if (result.nextDeadlineNs !== ll.TA_TIME_NEVER) {
        const delta =
          result.nextDeadlineNs > now ? result.nextDeadlineNs - now : 0n;
        this._schedule(
          Number(
            delta / 1000000n > 2147483646n
              ? 2147483647n
              : (delta + 999999n) / 1000000n,
          ),
        );
      }
    } catch (error) {
      this.stop();
      if (options.onError) options.onError(error);
      else process.emitWarning(error);
    }
  }
  start(options = {}) {
    if (this._closed) throw new TinyActorsError("start", ll.TA_STALE);
    if (this._automatic) return this;
    this._automatic = {
      ...options,
      baseClock: process.hrtime.bigint(),
      baseNow: BigInt(this._now),
    };
    this._wake();
    return this;
  }
  stop() {
    this._cancelScheduled();
    this._automatic = null;
    return this;
  }
  shutdown() {
    this.stop();
    checked(ll.ta_system_shutdown(this.handle), "shutdown");
  }
  close(options = {}) {
    if (this._closed) return;
    this.stop();
    if (!options.force) {
      checked(ll.ta_system_shutdown(this.handle), "close");
      while (this.run({ workUnits: 100000 }).runnable) {}
      const stats = this.stats();
      if (stats.io_requests || stats.invocations)
        throw new TinyActorsError(
          "close: finish I/O leases and stop services first",
          ll.TA_BUSY,
        );
    }
    ll.disposeSystem(this.handle);
    this._closed = true;
    this._owned.clear();
    this._definitions = new WeakMap();
  }
}
function createSystem(options) {
  return new System(options);
}
module.exports = { createSystem, System, Actor, Lease, TinyActorsError };
