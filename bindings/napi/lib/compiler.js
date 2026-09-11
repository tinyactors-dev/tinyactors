"use strict";
const ll = require("../lowlevel");
const { sendTarget, delayNs } = require("./helpers");
const list = (value) =>
  value === undefined ? [] : Array.isArray(value) ? value : [value];
const own = (object, key) => Object.prototype.hasOwnProperty.call(object, key);
function sync(value) {
  if (value && typeof value.then === "function")
    throw new TypeError("Actor expressions must be synchronous.");
  return value;
}
function defineMachine(machine) {
  if (
    !machine ||
    typeof machine !== "object" ||
    typeof machine.id !== "string" ||
    !machine.id
  )
    throw new TypeError("machine.id must be a nonempty string.");
  if (
    machine.on ||
    machine.always ||
    machine.entry ||
    machine.exit ||
    machine.invoke
  )
    throw new TypeError(
      "Place transitions, entry/exit actions, and invokes on a child state.",
    );
  const factory =
    machine.state ||
    (({ input }) => (input === undefined ? {} : structuredClone(input)));
  if (typeof factory !== "function")
    throw new TypeError("machine.state must be a factory function.");
  const owner = { factory, functions: [] };
  const nodes = new Map(),
    root = { id: "", source: machine, parent: null, children: [] };
  const ancestors = new Set();
  function collect(parent, states) {
    if (states === undefined) return [];
    if (!states || typeof states !== "object" || Array.isArray(states))
      throw new TypeError("states must be a keyed object.");
    return Object.entries(states).map(([name, source]) => {
      if (!source || typeof source !== "object" || Array.isArray(source))
        throw new TypeError("A state must be an object: " + name);
      if (
        ancestors.has(source) ||
        ancestors.size >= 64 ||
        nodes.size >= 1000000
      )
        throw new RangeError(
          "State nesting is cyclic or exceeds the construction limit.",
        );
      const id = source.id || (parent.id ? parent.id + "." + name : name);
      if (
        typeof id !== "string" ||
        !id ||
        /[\s\0]/u.test(id) ||
        id === "$root" ||
        nodes.has(id)
      )
        throw new TypeError("Invalid or duplicate state id: " + id);
      const node = { id, source, parent, children: [] };
      nodes.set(id, node);
      ancestors.add(source);
      node.children = collect(node, source.states);
      ancestors.delete(source);
      return node;
    });
  }
  root.children = collect(root, machine.states);
  if (!root.children.length)
    throw new TypeError("A machine needs at least one state.");
  function token(fn) {
    if (fn === undefined || fn === null) return 0n;
    if (typeof fn !== "function")
      throw new TypeError("Guards and executable code must be functions.");
    owner.functions.push(fn);
    return BigInt(owner.functions.length);
  }
  function expression(value) {
    if (typeof value === "function") return value;
    const literal = structuredClone(value);
    return () => structuredClone(literal);
  }
  function textExpr(value) {
    if (typeof value === "function") return { expression: token(value) };
    if (value !== undefined && typeof value !== "string")
      throw new TypeError("Text operands must be strings or functions.");
    return { literal: value };
  }
  function bodyExpr(value) {
    return value === undefined
      ? { kind: ll.TA_BODY_NONE }
      : { kind: ll.TA_BODY_CONTENT, content: token(expression(value)) };
  }
  function resolve(value, source, initial = false) {
    if (value === undefined) return undefined;
    const names = Array.isArray(value)
      ? value
      : typeof value === "string"
        ? value.trim().split(/[ \t\r\n]+/)
        : null;
    if (
      !names ||
      !names.length ||
      names.some((name) => typeof name !== "string" || !name)
    )
      throw new TypeError("Targets must be state names or arrays of names.");
    return names
      .map((name) => {
        if (name.startsWith("#")) {
          if (nodes.has(name.slice(1))) return name.slice(1);
        } else if (name.startsWith(".")) {
          if (nodes.has(source.id + name)) return source.id + name;
        } else {
          for (
            let scope = initial ? source : source.parent;
            scope;
            scope = scope.parent
          ) {
            const candidate = scope.id ? scope.id + "." + name : name;
            if (nodes.has(candidate)) return candidate;
          }
          if (nodes.has(name)) return name;
        }
        throw new TypeError(
          'Unknown target "' +
            name +
            '" from "' +
            (source.id || "$root") +
            '".',
        );
      })
      .join(" ");
  }
  function block(actions) {
    return { actions: list(actions).map(action) };
  }
  function blocks(actions) {
    if (actions === undefined) return [];
    return Array.isArray(actions) && actions.some(Array.isArray)
      ? actions.map(block)
      : [block(actions)];
  }
  function action(input) {
    if (typeof input === "function")
      return { kind: ll.TA_ACTION_CUSTOM, code: token(input) };
    if (!input || typeof input !== "object")
      throw new TypeError("Actions must be functions or action helpers.");
    if (input.kind === "assign") {
      if (!input.patch || !["function", "object"].includes(typeof input.patch))
        throw new TypeError("assign requires an object or a patch function.");
      const patch = expression(input.patch);
      return {
        kind: ll.TA_ACTION_CUSTOM,
        code: token((context) =>
          Object.assign(context.state, sync(patch(context))),
        ),
      };
    }
    if (input.kind === "raise") {
      if (typeof input.event !== "string" || !input.event)
        throw new TypeError("raise requires a literal event name.");
      return { kind: ll.TA_ACTION_RAISE, event: input.event };
    }
    if (input.kind === "cancel")
      return { kind: ll.TA_ACTION_CANCEL, sendid: textExpr(input.sendid) };
    if (input.kind === "send") {
      const destination = input.to ?? input.target;
      if (
        typeof destination === "function" ||
        (destination &&
          typeof destination === "object" &&
          typeof destination.id === "bigint")
      ) {
        return {
          kind: ll.TA_ACTION_CUSTOM,
          code: token((context) => {
            const value = (item) =>
              typeof item === "function" ? sync(item(context)) : item;
            context.send(value(input.event), {
              ...input,
              to: value(destination),
              processor: value(input.processor ?? input.type),
              data: value(input.data),
              delayNs: value(input.delayNs),
              delayMs: value(input.delayMs),
            });
          }),
        };
      }
      const delayed =
        typeof input.delayNs === "function"
          ? input.delayNs
          : typeof input.delayMs === "function"
            ? (context) => delayNs({ delayMs: sync(input.delayMs(context)) })
            : null;
      return {
        kind: ll.TA_ACTION_SEND,
        event: textExpr(input.event),
        type: textExpr(input.processor ?? input.type),
        target: sendTarget(destination),
        body: bodyExpr(input.data),
        delay_ns: delayed ? 0n : delayNs(input),
        delay_expression: delayed ? token(delayed) : 0n,
        id: input.id,
      };
    }
    if (input.kind === "script" || input.kind === "custom")
      return {
        kind:
          input.kind === "script" ? ll.TA_ACTION_SCRIPT : ll.TA_ACTION_CUSTOM,
        code: token(input.run || input.code),
      };
    throw new TypeError("Unknown action helper: " + input.kind);
  }
  function transition(input, node, event) {
    if (typeof input === "string" || Array.isArray(input))
      input = { target: input };
    else if (typeof input === "function" || input?.kind)
      input = { actions: input };
    if (!input || typeof input !== "object")
      throw new TypeError(
        "A transition must be a target name, function, or object.",
      );
    return {
      kind: input.internal
        ? ll.TA_TRANSITION_INTERNAL
        : ll.TA_TRANSITION_EXTERNAL,
      event,
      target: resolve(input.target, node),
      condition: token(input.guard || input.cond),
      content: block(input.actions ?? input.action),
    };
  }
  function declarations(input) {
    if (input === undefined) return [];
    if (Array.isArray(input))
      return input.map((item) => ({
        id: item.id,
        initializer: token(expression(item.initializer)),
      }));
    if (!input || typeof input !== "object")
      throw new TypeError("data declarations must be an object.");
    return Object.entries(input).map(([id, value]) => ({
      id,
      initializer: token(expression(value)),
    }));
  }
  function invokes(input) {
    return list(input).map((invoke) => {
      if (!invoke || typeof invoke !== "object")
        throw new TypeError("invoke must be an object.");
      return {
        type: textExpr(invoke.type),
        source: textExpr(invoke.source),
        inline_definition: invoke.inlineDefinition,
        body: bodyExpr(invoke.input ?? invoke.data),
        id: invoke.id,
        autoforward: invoke.autoforward ? 1 : 0,
        finalize_kind: invoke.finalize
          ? ll.TA_FINALIZE_BLOCK
          : ll.TA_FINALIZE_ABSENT,
        finalize: block(invoke.finalize),
      };
    });
  }
  function state(node) {
    const input = node.source;
    let kind =
      input.type ||
      (input.parallel
        ? "parallel"
        : input.final
          ? "final"
          : input.history
            ? "history"
            : "state");
    if (kind === "history")
      kind = input.history === "deep" ? "deep" : "shallow";
    const kinds = {
      state: ll.TA_CHART_STATE,
      parallel: ll.TA_CHART_PARALLEL,
      final: ll.TA_CHART_FINAL,
      shallow: ll.TA_CHART_HISTORY_SHALLOW,
      deep: ll.TA_CHART_HISTORY_DEEP,
    };
    if (!own(kinds, kind)) throw new TypeError("Unknown state type: " + kind);
    const transitions = [];
    for (const [event, choices] of Object.entries(input.on || {}))
      for (const choice of list(choices))
        transitions.push(transition(choice, node, event));
    for (const choice of list(input.always))
      transitions.push(transition(choice, node));
    if (input.onDone)
      for (const choice of list(input.onDone)) {
        const event = "done.state." + node.id;
        const completion = transition(choice, node, event);
        const guard = completion.condition
          ? owner.functions[Number(completion.condition) - 1]
          : null;
        // SCXML descriptors match prefixes; onDone means this state's completion,
        // not the completion of a path-qualified descendant.
        completion.condition = token(
          (context) =>
            context.event?.type === event && (!guard || sync(guard(context))),
        );
        transitions.push(completion);
      }
    return {
      id: node.id,
      kind: kinds[kind],
      initial: {
        target: resolve(input.initial, node, true),
        content: block(input.initialActions),
      },
      states: node.children.map(state),
      transitions,
      onentry: blocks(input.entry),
      onexit: blocks(input.exit),
      data: declarations(input.data),
      invokes: invokes(input.invoke),
      done_data: bodyExpr(input.doneData),
    };
  }
  const chart = {
    name: machine.id,
    data_model: "javascript",
    code_owner: owner,
    binding:
      machine.binding === "late" ? ll.TA_BINDING_LATE : ll.TA_BINDING_EARLY,
    initial: {
      target: resolve(machine.initial, root, true),
      content: block(machine.initialActions),
    },
    states: root.children.map(state),
    data: declarations(machine.data),
    root_script: token(machine.root),
  };
  Object.freeze(owner.functions);
  Object.freeze(owner);
  return Object.freeze({ id: machine.id, owner, chart });
}
module.exports = { defineMachine };
