#include "bridge.hpp"

/* The wire representation used here is deliberately boring: C member names
 * are used verbatim, enum unions carry a `kind` member and their selected
 * fields are flattened into the containing action object. Payloads use
 * {kind: TA_PAYLOAD_NATIVE, native: value} or
 * {kind: TA_PAYLOAD_BYTES, bytes: buffer}.  Keeping this code independent of
 * the exported functions also makes malformed authoring objects fail before the
 * core sees them. */
namespace tn {
namespace {
static ta_block_t readBlock(Parse &, napi_value);
static napi_value f(Env &e, napi_value o, const char *k) { return e.get(o, k); }
static bool present(Env &e, napi_value v) { return !e.absent(v); }
static uint32_t count(Env &e, napi_value a) { return (uint32_t)e.length(a); }
static ta_text_t txt(Parse &p, napi_value o, const char *k) {
  return p.text(f(p.js, o, k));
}
static ta_code_t code(Env &e, napi_value o, const char *k) {
  auto number = e.u64(f(e, o, k));
  if (number > UINTPTR_MAX)
    throw std::range_error("Code token exceeds host uintptr_t");
  return static_cast<ta_code_t>(number);
}
static ta_targets_t targets(Parse &p, napi_value o) {
  Env &e = p.js;
  napi_value a = f(e, o, "states");
  ta_targets_t x{};
  if (!present(e, a))
    return x;
  p.enter();
  uint32_t n = count(e, a);
  auto *q = p.items<ta_state_id_t>(n);
  x.states = q;
  x.count = n;
  for (uint32_t i = 0; i < n; i++)
    q[i] = e.u32(e.element(a, i));
  p.leave();
  return x;
}
static ta_body_expr_t bodyexpr(Parse &p, napi_value o) {
  Env &e = p.js;
  ta_body_expr_t b{};
  if (!e.isObject(o))
    return b;
  b.kind = (ta_body_kind_t)e.u32(f(e, o, "kind"));
  b.content = code(e, o, "content");
  b.bytes_content = code(e, o, "bytes_content");
  napi_value a = f(e, o, "parameters");
  if (present(e, a)) {
    p.enter();
    b.parameter_count = count(e, a);
    auto *q = p.items<ta_parameter_expr_t>(b.parameter_count);
    b.parameters = q;
    for (uint32_t i = 0; i < b.parameter_count; i++) {
      napi_value v = e.element(a, i);
      q[i].name = txt(p, v, "name");
      q[i].expression = code(e, v, "expression");
      q[i].location = code(e, v, "location");
    }
    p.leave();
  }
  return b;
}
static ta_text_expr_t textexpr(Parse &p, napi_value o, const char *k) {
  ta_text_expr_t x{};
  Env &e = p.js;
  napi_value v = f(e, o, k);
  if (e.isObject(v)) {
    x.literal = p.text(f(e, v, "literal"));
    x.expression = code(e, v, "expression");
  } else if (present(e, v))
    x.literal = p.text(v);
  return x;
}
static ta_action_t action(Parse &p, napi_value o) {
  Env &e = p.js;
  ta_action_t a{};
  a.kind = (ta_action_kind_t)e.u32(f(e, o, "kind"));
  switch (a.kind) {
  case TA_ACTION_RAISE:
    a.as.raise.event = txt(p, o, "event");
    break;
  case TA_ACTION_ASSIGN:
    a.as.assign.location = code(e, o, "location");
    a.as.assign.value = code(e, o, "value");
    break;
  case TA_ACTION_SCRIPT:
    a.as.script.code = code(e, o, "code");
    break;
  case TA_ACTION_CUSTOM:
    a.as.custom.code = code(e, o, "code");
    break;
  case TA_ACTION_LOG:
    a.as.log.label = txt(p, o, "label");
    a.as.log.value = code(e, o, "value");
    break;
  case TA_ACTION_CANCEL:
    a.as.cancel.sendid = textexpr(p, o, "sendid");
    break;
  case TA_ACTION_SEND: {
    auto &s = a.as.send;
    s.event = textexpr(p, o, "event");
    s.type = textexpr(p, o, "type");
    s.target = readTarget(p, f(e, o, "target"));
    s.target_expression = code(e, o, "target_expression");
    s.body = bodyexpr(p, f(e, o, "body"));
    s.delay_ns = e.u64(f(e, o, "delay_ns"));
    s.delay_expression = code(e, o, "delay_expression");
    s.id = txt(p, o, "id");
    s.idlocation = code(e, o, "idlocation");
    break;
  }
  case TA_ACTION_IF: {
    napi_value ar = f(e, o, "branches");
    if (present(e, ar)) {
      p.enter();
      a.as.conditional.count = count(e, ar);
      auto *q = p.items<ta_branch_t>(a.as.conditional.count);
      a.as.conditional.branches = q;
      for (uint32_t i = 0; i < a.as.conditional.count; i++) {
        napi_value v = e.element(ar, i);
        q[i].condition = code(e, v, "condition");
        ta_block_t b = readBlock(p, f(e, v, "body"));
        q[i].body.actions = b.actions;
        q[i].body.count = b.count;
      }
      p.leave();
    }
    break;
  }
  case TA_ACTION_FOREACH:
    a.as.foreach_loop.collection = code(e, o, "collection");
    a.as.foreach_loop.item = txt(p, o, "item");
    a.as.foreach_loop.index = txt(p, o, "index");
    {
      ta_block_t b = readBlock(p, f(e, o, "body"));
      a.as.foreach_loop.body.actions = b.actions;
      a.as.foreach_loop.body.count = b.count;
    }
    break;
  default:
    break;
  }
  return a;
}
static ta_block_t readBlock(Parse &p, napi_value o) {
  Env &e = p.js;
  ta_block_t b{};
  napi_value a = f(e, o, "actions");
  if (!present(e, a))
    return b;
  p.enter();
  b.count = count(e, a);
  auto *q = p.items<ta_action_t>(b.count);
  b.actions = q;
  for (uint32_t i = 0; i < b.count; i++)
    q[i] = action(p, e.element(a, i));
  p.leave();
  return b;
}
static ta_blocks_t blocks(Parse &p, napi_value o, const char *k) {
  Env &e = p.js;
  ta_blocks_t b{};
  napi_value a = f(e, o, k);
  if (!present(e, a))
    return b;
  p.enter();
  b.count = count(e, a);
  auto *q = p.items<ta_block_t>(b.count);
  b.blocks = q;
  for (uint32_t i = 0; i < b.count; i++)
    q[i] = readBlock(p, e.element(a, i));
  p.leave();
  return b;
}
static ta_data_desc_t dataDesc(Parse &p, napi_value o) {
  ta_data_desc_t x{};
  x.id = txt(p, o, "id");
  x.initializer = code(p.js, o, "initializer");
  return x;
}
static ta_body_expr_t bodyExpr(Parse &p, napi_value o) {
  return bodyexpr(p, o);
}
static ta_invoke_desc_t invoke(Parse &p, napi_value input) {
  Env &e = p.js;
  ta_invoke_desc_t out{};
  out.type = textexpr(p, input, "type");
  out.source = textexpr(p, input, "source");
  out.body = bodyExpr(p, f(e, input, "body"));
  out.id = txt(p, input, "id");
  out.idlocation = code(e, input, "idlocation");
  out.autoforward = e.truth(f(e, input, "autoforward"));
  out.finalize_kind =
      static_cast<ta_finalize_kind_t>(e.u32(f(e, input, "finalize_kind")));
  out.finalize = readBlock(p, f(e, input, "finalize"));
  auto handle = f(e, input, "inline_definition");
  if (present(e, handle)) {
    auto inlineDefinition = definition(e, handle);
    if (inlineDefinition->owner.get() != p.owner)
      throw std::invalid_argument(
          "Inline definition belongs to another system");
    out.inline_definition = inlineDefinition->definition;
  }
  return out;
}
static ta_chart_state_t chartState(Parse &p, napi_value o) {
  Env &e = p.js;
  ta_chart_state_t x{};
  x.id = p.cstr(f(e, o, "id"));
  if (!x.id || !x.id[0])
    throw std::invalid_argument("Each chart state needs a nonempty id; omit C "
                                "sentinel elements in JavaScript");
  x.kind = (ta_chart_state_kind_t)e.u32(f(e, o, "kind"));
  napi_value i = f(e, o, "initial");
  x.initial.target = p.cstr(f(e, i, "target"));
  x.initial.content = readBlock(p, f(e, i, "content"));
  napi_value ch = f(e, o, "states");
  if (present(e, ch)) {
    p.enter();
    uint32_t n = count(e, ch);
    auto *q = p.terminated<ta_chart_state_t>(n);
    x.states = q;
    for (uint32_t k = 0; k < n; k++)
      q[k] = chartState(p, e.element(ch, k));
    q[n] = {};
    p.leave();
  }
  napi_value tr = f(e, o, "transitions");
  if (present(e, tr)) {
    p.enter();
    uint32_t n = count(e, tr);
    auto *q = p.terminated<ta_chart_transition_t>(n);
    x.transitions = q;
    for (uint32_t k = 0; k < n; k++) {
      napi_value v = e.element(tr, k);
      q[k].kind = (ta_transition_kind_t)e.u32(f(e, v, "kind"));
      q[k].event = p.cstr(f(e, v, "event"));
      q[k].condition = code(e, v, "condition");
      q[k].target = p.cstr(f(e, v, "target"));
      q[k].content = readBlock(p, f(e, v, "content"));
      if (!q[k].kind && !q[k].event && !q[k].condition && !q[k].target &&
          !q[k].content.count)
        throw std::invalid_argument(
            "Empty chart transition would be a C sentinel; omit it or supply a "
            "condition/action");
    }
    q[n] = {};
    p.leave();
  }
  napi_value d = f(e, o, "data");
  if (present(e, d)) {
    p.enter();
    uint32_t n = count(e, d);
    auto *q = p.items<ta_data_desc_t>(n);
    x.data.items = q;
    x.data.count = n;
    for (uint32_t k = 0; k < n; k++)
      q[k] = dataDesc(p, e.element(d, k));
    p.leave();
  }
  napi_value iv = f(e, o, "invokes");
  if (present(e, iv)) {
    p.enter();
    uint32_t n = count(e, iv);
    auto *q = p.items<ta_invoke_desc_t>(n);
    x.invokes.items = q;
    x.invokes.count = n;
    for (uint32_t k = 0; k < n; k++)
      q[k] = invoke(p, e.element(iv, k));
    p.leave();
  }
  x.onentry = blocks(p, o, "onentry");
  x.onexit = blocks(p, o, "onexit");
  x.done_data = bodyExpr(p, f(e, o, "done_data"));
  return x;
}
static ta_state_desc_t stateDesc(Parse &p, napi_value o) {
  Env &e = p.js;
  ta_state_desc_t x{};
  x.name = txt(p, o, "name");
  x.parent = e.u32(f(e, o, "parent"));
  x.kind = (ta_state_kind_t)e.u32(f(e, o, "kind"));
  napi_value ini = f(e, o, "initial");
  x.initial.targets = targets(p, f(e, ini, "targets"));
  x.initial.content = readBlock(p, f(e, ini, "content"));
  x.first_transition = e.u32(f(e, o, "first_transition"));
  x.transition_count = e.u32(f(e, o, "transition_count"));
  x.onentry = blocks(p, o, "onentry");
  x.onexit = blocks(p, o, "onexit");
  napi_value d = f(e, o, "data");
  if (present(e, d)) {
    p.enter();
    uint32_t n = count(e, d);
    auto *q = p.items<ta_data_desc_t>(n);
    x.data = q;
    x.data_count = n;
    for (uint32_t i = 0; i < n; i++)
      q[i] = dataDesc(p, e.element(d, i));
    p.leave();
  }
  napi_value iv = f(e, o, "invokes");
  if (present(e, iv)) {
    p.enter();
    uint32_t n = count(e, iv);
    auto *q = p.items<ta_invoke_desc_t>(n);
    x.invokes = q;
    x.invoke_count = n;
    for (uint32_t i = 0; i < n; i++)
      q[i] = invoke(p, e.element(iv, i));
    p.leave();
  }
  x.done_data = bodyExpr(p, f(e, o, "done_data"));
  return x;
}
static ta_transition_desc_t transition(Parse &p, napi_value o) {
  Env &e = p.js;
  ta_transition_desc_t x{};
  x.source = e.u32(f(e, o, "source"));
  x.kind = (ta_transition_kind_t)e.u32(f(e, o, "kind"));
  napi_value a = f(e, o, "events");
  if (present(e, a)) {
    p.enter();
    uint32_t n = count(e, a);
    auto *q = p.items<ta_text_t>(n);
    x.events = q;
    x.event_count = n;
    for (uint32_t i = 0; i < n; i++)
      q[i] = p.text(e.element(a, i));
    p.leave();
  }
  x.condition = code(e, o, "condition");
  x.targets = targets(p, f(e, o, "targets"));
  x.content = readBlock(p, f(e, o, "content"));
  return x;
}
struct DescriptionWalk {
  Addon &state;
  explicit DescriptionWalk(Env js) : state(addon(js)) {
    if (!state.description_depth)
      state.description_nodes = 0;
    if (state.description_depth >= 64 || state.description_nodes >= 1000000)
      throw std::range_error("Definition description traversal limit exceeded");
    ++state.description_depth;
    ++state.description_nodes;
  }
  ~DescriptionWalk() { --state.description_depth; }
};
static napi_value writeTextExpr(Env &e, const ta_text_expr_t &x) {
  auto o = e.object();
  e.set(o, "literal", e.text(x.literal));
  e.set(o, "expression", e.bigint((uint64_t)x.expression));
  return o;
}
static napi_value writeBodyExpr(Env &e, const ta_body_expr_t &x) {
  auto o = e.object();
  e.set(o, "kind", e.number(x.kind));
  e.set(o, "content", e.bigint((uint64_t)x.content));
  e.set(o, "bytes_content", e.bigint((uint64_t)x.bytes_content));
  auto a = e.array(x.parameter_count);
  for (uint32_t i = 0; i < x.parameter_count; i++) {
    auto q = e.object();
    e.set(q, "name", e.text(x.parameters[i].name));
    e.set(q, "expression", e.bigint((uint64_t)x.parameters[i].expression));
    e.set(q, "location", e.bigint((uint64_t)x.parameters[i].location));
    e.element(a, i, q);
  }
  e.set(o, "parameters", a);
  return o;
}
static napi_value writeTargets(Env &e, const ta_targets_t &x) {
  auto o = e.object();
  auto a = e.array(x.count);
  for (uint32_t i = 0; i < x.count; i++)
    e.element(a, i, e.number(x.states[i]));
  e.set(o, "states", a);
  return o;
}
static napi_value writeBlock(Env &e, const ta_block_t &x);
static napi_value writeAction(Env &e, const ta_action_t &x) {
  auto o = e.object();
  e.set(o, "kind", e.number(x.kind));
  switch (x.kind) {
  case TA_ACTION_RAISE:
    e.set(o, "event", e.text(x.as.raise.event));
    break;
  case TA_ACTION_ASSIGN:
    e.set(o, "location", e.bigint((uint64_t)x.as.assign.location));
    e.set(o, "value", e.bigint((uint64_t)x.as.assign.value));
    break;
  case TA_ACTION_SCRIPT:
    e.set(o, "code", e.bigint((uint64_t)x.as.script.code));
    break;
  case TA_ACTION_CUSTOM:
    e.set(o, "code", e.bigint((uint64_t)x.as.custom.code));
    break;
  case TA_ACTION_LOG:
    e.set(o, "label", e.text(x.as.log.label));
    e.set(o, "value", e.bigint((uint64_t)x.as.log.value));
    break;
  case TA_ACTION_CANCEL:
    e.set(o, "sendid", writeTextExpr(e, x.as.cancel.sendid));
    break;
  case TA_ACTION_SEND: {
    const auto &s = x.as.send;
    e.set(o, "event", writeTextExpr(e, s.event));
    e.set(o, "type", writeTextExpr(e, s.type));
    auto tg = e.object();
    e.set(tg, "kind", e.number(s.target.kind));
    e.set(tg, "instance", e.bigint(s.target.instance.id));
    e.set(tg, "text", e.text(s.target.text));
    e.set(o, "target", tg);
    e.set(o, "target_expression", e.bigint((uint64_t)s.target_expression));
    e.set(o, "body", writeBodyExpr(e, s.body));
    e.set(o, "delay_ns", e.bigint(s.delay_ns));
    e.set(o, "delay_expression", e.bigint((uint64_t)s.delay_expression));
    e.set(o, "id", e.text(s.id));
    e.set(o, "idlocation", e.bigint((uint64_t)s.idlocation));
    break;
  }
  case TA_ACTION_IF: {
    auto a = e.array(x.as.conditional.count);
    for (uint32_t i = 0; i < x.as.conditional.count; i++) {
      auto q = e.object();
      e.set(q, "condition",
            e.bigint((uint64_t)x.as.conditional.branches[i].condition));
      e.set(q, "body", writeBlock(e, x.as.conditional.branches[i].body));
      e.element(a, i, q);
    }
    e.set(o, "branches", a);
    break;
  }
  case TA_ACTION_FOREACH:
    e.set(o, "collection", e.bigint((uint64_t)x.as.foreach_loop.collection));
    e.set(o, "item", e.text(x.as.foreach_loop.item));
    e.set(o, "index", e.text(x.as.foreach_loop.index));
    e.set(o, "body", writeBlock(e, x.as.foreach_loop.body));
    break;
  default:
    break;
  }
  return o;
}
static napi_value writeBlock(Env &e, const ta_block_t &x) {
  auto o = e.object();
  auto a = e.array(x.count);
  for (uint32_t i = 0; i < x.count; i++)
    e.element(a, i, writeAction(e, x.actions[i]));
  e.set(o, "actions", a);
  return o;
}
static napi_value writeBlocks(Env &e, const ta_blocks_t &x) {
  auto a = e.array(x.count);
  for (uint32_t i = 0; i < x.count; i++)
    e.element(a, i, writeBlock(e, x.blocks[i]));
  return a;
}
static napi_value writeInvoke(Env &e, const ta_invoke_desc_t &x,
                              System *owner) {
  auto o = e.object();
  e.set(o, "type", writeTextExpr(e, x.type));
  e.set(o, "source", writeTextExpr(e, x.source));
  e.set(o, "body", writeBodyExpr(e, x.body));
  e.set(o, "id", e.text(x.id));
  e.set(o, "idlocation", e.bigint((uint64_t)x.idlocation));
  e.set(o, "autoforward", e.boolean(x.autoforward));
  e.set(o, "finalize_kind", e.number(x.finalize_kind));
  e.set(o, "finalize", writeBlock(e, x.finalize));
  if (x.inline_definition) {
    e.set(o, "inline_revision",
          e.bigint(ta_definition_revision(x.inline_definition)));
    e.set(o, "inline_description",
          writeDescription(e, *ta_definition_description(x.inline_definition),
                           owner));
    auto parent = owner->configuration();
    ta_definition_retain(x.inline_definition);
    e.set(o, "inline_definition",
          wrapDefinition(owner->shared_from_this(),
                         const_cast<ta_definition_t *>(x.inline_definition),
                         parent));
  }
  return o;
}
static napi_value writeTransition(Env &e, const ta_transition_desc_t &x) {
  auto o = e.object();
  e.set(o, "source", e.number(x.source));
  e.set(o, "kind", e.number(x.kind));
  auto a = e.array(x.event_count);
  for (uint32_t i = 0; i < x.event_count; i++)
    e.element(a, i, e.text(x.events[i]));
  e.set(o, "events", a);
  e.set(o, "condition", e.bigint((uint64_t)x.condition));
  e.set(o, "targets", writeTargets(e, x.targets));
  e.set(o, "content", writeBlock(e, x.content));
  return o;
}
static napi_value writeState(Env &e, const ta_state_desc_t &x, System *owner) {
  auto o = e.object();
  e.set(o, "name", e.text(x.name));
  e.set(o, "parent", e.number(x.parent));
  e.set(o, "kind", e.number(x.kind));
  auto ini = e.object();
  e.set(ini, "targets", writeTargets(e, x.initial.targets));
  e.set(ini, "content", writeBlock(e, x.initial.content));
  e.set(o, "initial", ini);
  e.set(o, "first_transition", e.number(x.first_transition));
  e.set(o, "transition_count", e.number(x.transition_count));
  e.set(o, "onentry", writeBlocks(e, x.onentry));
  e.set(o, "onexit", writeBlocks(e, x.onexit));
  auto d = e.array(x.data_count);
  for (uint32_t i = 0; i < x.data_count; i++) {
    auto q = e.object();
    e.set(q, "id", e.text(x.data[i].id));
    e.set(q, "initializer", e.bigint((uint64_t)x.data[i].initializer));
    e.element(d, i, q);
  }
  e.set(o, "data", d);
  auto iv = e.array(x.invoke_count);
  for (uint32_t i = 0; i < x.invoke_count; i++)
    e.element(iv, i, writeInvoke(e, x.invokes[i], owner));
  e.set(o, "invokes", iv);
  e.set(o, "done_data", writeBodyExpr(e, x.done_data));
  return o;
}
} // namespace

ta_arena_options_t readArenaOptions(Parse &p, napi_value o) {
  ta_arena_options_t x{};
  Env &e = p.js;
  x.initial_chunk_bytes = e.size(f(e, o, "initial_chunk_bytes"));
  x.maximum_chunk_bytes = e.size(f(e, o, "maximum_chunk_bytes"));
  x.maximum_bytes = e.size(f(e, o, "maximum_bytes"));
  x.maximum_allocation = e.size(f(e, o, "maximum_allocation"));
  return x;
}
ta_layout_t readLayout(Parse &p, napi_value o) {
  ta_layout_t x{};
  Env &e = p.js;
  if (e.absent(o))
    return x;
  x.size = e.size(f(e, o, "size"));
  x.alignment = e.size(f(e, o, "alignment"), 1);
  return x;
}
ta_actor_limits_t readActorLimits(Parse &p, napi_value o,
                                  const ta_actor_limits_t *base) {
  ta_actor_limits_t x = base ? *base : ta_actor_limits_t{};
  Env &e = p.js;
  x.maximum_bytes = e.size(f(e, o, "maximum_bytes"), x.maximum_bytes);
  x.external_events = e.size(f(e, o, "external_events"), x.external_events);
  x.internal_events = e.size(f(e, o, "internal_events"), x.internal_events);
  x.queued_event_bytes =
      e.size(f(e, o, "queued_event_bytes"), x.queued_event_bytes);
  x.delayed_sends = e.size(f(e, o, "delayed_sends"), x.delayed_sends);
  x.invocations = e.size(f(e, o, "invocations"), x.invocations);
  x.io_requests = e.size(f(e, o, "io_requests"), x.io_requests);
  x.io_request_bytes = e.size(f(e, o, "io_request_bytes"), x.io_request_bytes);
  x.microsteps_per_macrostep =
      e.size(f(e, o, "microsteps_per_macrostep"), x.microsteps_per_macrostep);
  x.actions_per_macrostep =
      e.size(f(e, o, "actions_per_macrostep"), x.actions_per_macrostep);
  return x;
}
ta_system_limits_t readSystemLimits(Parse &p, napi_value o,
                                    const ta_system_limits_t *base) {
  ta_system_limits_t x = base ? *base : ta_system_limits_t{};
  Env &e = p.js;
  x.maximum_bytes = e.size(f(e, o, "maximum_bytes"), x.maximum_bytes);
  x.actors = e.size(f(e, o, "actors"), x.actors);
  x.definitions = e.size(f(e, o, "definitions"), x.definitions);
  x.names = e.size(f(e, o, "names"), x.names);
  x.queued_events = e.size(f(e, o, "queued_events"), x.queued_events);
  x.delayed_sends = e.size(f(e, o, "delayed_sends"), x.delayed_sends);
  x.invocations = e.size(f(e, o, "invocations"), x.invocations);
  x.io_requests = e.size(f(e, o, "io_requests"), x.io_requests);
  x.definition_states =
      e.size(f(e, o, "definition_states"), x.definition_states);
  x.definition_transitions =
      e.size(f(e, o, "definition_transitions"), x.definition_transitions);
  x.definition_actions =
      e.size(f(e, o, "definition_actions"), x.definition_actions);
  x.state_depth = e.size(f(e, o, "state_depth"), x.state_depth);
  x.action_depth = e.size(f(e, o, "action_depth"), x.action_depth);
  x.event_name_bytes = e.size(f(e, o, "event_name_bytes"), x.event_name_bytes);
  x.trace_metadata_bytes =
      e.size(f(e, o, "trace_metadata_bytes"), x.trace_metadata_bytes);
  return x;
}
ta_target_t readTarget(Parse &p, napi_value o) {
  ta_target_t x{};
  Env &e = p.js;
  if (!e.isObject(o))
    return x;
  x.kind = (ta_target_kind_t)e.u32(f(e, o, "kind"));
  x.instance.id = e.u64(f(e, o, "instance"));
  x.text = p.text(f(e, o, "text"));
  return x;
}
ta_payload_t readPayload(Parse &p, napi_value o) {
  ta_payload_t x{};
  Env &e = p.js;
  if (!e.isObject(o))
    return x;
  x.kind = (ta_payload_kind_t)e.u32(f(e, o, "kind"));
  if (x.kind == TA_PAYLOAD_NATIVE)
    x.as.native = p.native(f(e, o, "native"));
  else if (x.kind == TA_PAYLOAD_BYTES)
    x.as.bytes = p.bytes(f(e, o, "bytes"));
  return x;
}
ta_body_t readBody(Parse &p, napi_value o) {
  ta_body_t x{};
  Env &e = p.js;
  if (!e.isObject(o))
    return x;
  x.kind = (ta_body_kind_t)e.u32(f(e, o, "kind"));
  x.content = readPayload(p, f(e, o, "content"));
  napi_value a = f(e, o, "parameters");
  if (present(e, a)) {
    p.enter();
    x.parameter_count = e.length(a);
    auto *q = p.items<ta_parameter_t>(x.parameter_count);
    x.parameters = q;
    for (size_t i = 0; i < x.parameter_count; i++) {
      auto v = e.element(a, i);
      q[i].name = txt(p, v, "name");
      q[i].value = p.native(f(e, v, "value"));
    }
    p.leave();
  }
  return x;
}
ta_event_t readEvent(Parse &p, napi_value o) {
  ta_event_t x{};
  Env &e = p.js;
  x.name = txt(p, o, "name");
  x.type = (ta_event_type_t)e.u32(f(e, o, "type"));
  x.sendid = txt(p, o, "sendid");
  x.origin = txt(p, o, "origin");
  x.origintype = txt(p, o, "origintype");
  x.invokeid = txt(p, o, "invokeid");
  x.data = readPayload(p, f(e, o, "data"));
  x.message_id = e.u64(f(e, o, "message_id"));
  x.trace = readTrace(p, f(e, o, "trace"));
  return x;
}
ta_send_t readSend(Parse &p, napi_value o) {
  ta_send_t x{};
  Env &e = p.js;
  x.target = readTarget(p, f(e, o, "target"));
  x.event = p.text(f(e, o, "event"));
  x.type = p.text(f(e, o, "type"));
  x.body = readBody(p, f(e, o, "body"));
  x.delay_ns = e.u64(f(e, o, "delay_ns"));
  x.sendid = p.text(f(e, o, "sendid"));
  return x;
}
ta_chart_t readChart(Parse &p, napi_value o) {
  ta_chart_t x{};
  Env &e = p.js;
  x.name = p.cstr(f(e, o, "name"));
  x.data_model = p.cstr(f(e, o, "data_model"));
  if (present(e, f(e, o, "code_owner")))
    x.code_owner = p.native(f(e, o, "code_owner"));
  napi_value dl = f(e, o, "data_layout");
  x.data_layout.storage = (ta_data_storage_t)e.u32(f(e, dl, "storage"));
  x.data_layout.bytes_layout = readLayout(p, f(e, dl, "bytes_layout"));
  x.data_layout.initial_bytes = p.bytes(f(e, dl, "initial_bytes"));
  x.data_layout.schema = p.text(f(e, dl, "schema"));
  x.binding = (ta_binding_t)e.u32(f(e, o, "binding"));
  x.root_script = code(e, o, "root_script");
  napi_value ini = f(e, o, "initial");
  x.initial.target = p.cstr(f(e, ini, "target"));
  x.initial.content = readBlock(p, f(e, ini, "content"));
  napi_value st = f(e, o, "states");
  if (present(e, st)) {
    p.enter();
    uint32_t n = count(e, st);
    auto *q = p.terminated<ta_chart_state_t>(n);
    x.states = q;
    for (uint32_t i = 0; i < n; i++)
      q[i] = chartState(p, e.element(st, i));
    q[n] = {};
    p.leave();
  }
  napi_value d = f(e, o, "data");
  if (present(e, d)) {
    p.enter();
    uint32_t n = count(e, d);
    auto *q = p.items<ta_data_desc_t>(n);
    x.data.items = q;
    x.data.count = n;
    for (uint32_t i = 0; i < n; i++)
      q[i] = dataDesc(p, e.element(d, i));
    p.leave();
  }
  return x;
}
ta_definition_desc_t readDescription(Parse &p, napi_value o) {
  ta_definition_desc_t x{};
  Env &e = p.js;
  x.name = p.text(f(e, o, "name"));
  x.data_model = p.text(f(e, o, "data_model"));
  if (present(e, f(e, o, "code_owner")))
    x.code_owner = p.native(f(e, o, "code_owner"));
  napi_value dl = f(e, o, "data_layout");
  x.data_layout.storage = (ta_data_storage_t)e.u32(f(e, dl, "storage"));
  x.data_layout.bytes_layout = readLayout(p, f(e, dl, "bytes_layout"));
  x.data_layout.initial_bytes = p.bytes(f(e, dl, "initial_bytes"));
  x.data_layout.schema = p.text(f(e, dl, "schema"));
  x.binding = (ta_binding_t)e.u32(f(e, o, "binding"));
  x.root_script = code(e, o, "root_script");
  napi_value st = f(e, o, "states");
  if (present(e, st)) {
    p.enter();
    uint32_t n = count(e, st);
    auto *q = p.items<ta_state_desc_t>(n);
    x.states = q;
    x.state_count = n;
    for (uint32_t i = 0; i < n; i++)
      q[i] = stateDesc(p, e.element(st, i));
    p.leave();
  }
  napi_value tr = f(e, o, "transitions");
  if (present(e, tr)) {
    p.enter();
    uint32_t n = count(e, tr);
    auto *q = p.items<ta_transition_desc_t>(n);
    x.transitions = q;
    x.transition_count = n;
    for (uint32_t i = 0; i < n; i++)
      q[i] = transition(p, e.element(tr, i));
    p.leave();
  }
  return x;
}
const ta_trace_context_t *readTrace(Parse &p, napi_value o) {
  Env &e = p.js;
  if (!e.isObject(o))
    return nullptr;
  auto *x = (ta_trace_context_t *)p.alloc(1, sizeof(ta_trace_context_t));
  std::memset(x, 0, sizeof(ta_trace_context_t));
  ta_bytes_t a = p.bytes(f(e, o, "trace_id"));
  ta_bytes_t b = p.bytes(f(e, o, "span_id"));
  if (a.size != 16 || b.size != 8)
    throw std::range_error("trace IDs must be 16 and 8 bytes");
  std::memcpy(x->trace_id, a.data, 16);
  std::memcpy(x->span_id, b.data, 8);
  uint32_t tf = e.u32(f(e, o, "trace_flags"));
  if (tf > 255)
    throw std::range_error("trace_flags");
  x->trace_flags = (uint8_t)tf;
  x->remote = e.truth(f(e, o, "remote"));
  x->tracestate = p.text(f(e, o, "tracestate"));
  x->baggage = p.text(f(e, o, "baggage"));
  return x;
}

napi_value writeTrace(Env e, const ta_trace_context_t *x) {
  if (!x)
    return e.null();
  auto o = e.object();
  e.set(o, "trace_id", e.buffer({x->trace_id, 16}));
  e.set(o, "span_id", e.buffer({x->span_id, 8}));
  e.set(o, "trace_flags", e.number(x->trace_flags));
  e.set(o, "remote", e.number(x->remote));
  e.set(o, "tracestate", e.text(x->tracestate));
  e.set(o, "baggage", e.text(x->baggage));
  return o;
}
napi_value writePayload(Env e, const ta_payload_t &x) {
  auto o = e.object();
  e.set(o, "kind", e.number(x.kind));
  if (x.kind == TA_PAYLOAD_NATIVE)
    e.set(o, "native", value(x.as.native, e));
  else if (x.kind == TA_PAYLOAD_BYTES)
    e.set(o, "bytes", e.buffer(x.as.bytes));
  return o;
}
napi_value writeBody(Env e, const ta_body_t &x) {
  auto o = e.object();
  e.set(o, "kind", e.number(x.kind));
  if (x.kind == TA_BODY_CONTENT)
    e.set(o, "content", writePayload(e, x.content));
  if (x.kind == TA_BODY_PARAMETERS) {
    auto a = e.array(x.parameter_count);
    for (size_t i = 0; i < x.parameter_count; i++) {
      auto q = e.object();
      e.set(q, "name", e.text(x.parameters[i].name));
      e.set(q, "value", value(x.parameters[i].value, e));
      e.element(a, i, q);
    }
    e.set(o, "parameters", a);
  }
  return o;
}
napi_value writeEvent(Env e, const ta_event_t &x) {
  auto o = e.object();
  e.set(o, "name", e.text(x.name));
  e.set(o, "type", e.number(x.type));
  e.set(o, "sendid", e.text(x.sendid));
  e.set(o, "origin", e.text(x.origin));
  e.set(o, "origintype", e.text(x.origintype));
  e.set(o, "invokeid", e.text(x.invokeid));
  e.set(o, "data", writePayload(e, x.data));
  e.set(o, "message_id", e.bigint(x.message_id));
  e.set(o, "trace", writeTrace(e, x.trace));
  return o;
}
napi_value writeData(Env e, const ta_data_view_t &x) {
  auto o = e.object();
  e.set(o, "storage", e.number(x.storage));
  if (x.storage == TA_DATA_NATIVE)
    e.set(o, "native", value(x.as.native, e));
  else
    e.set(o, "bytes", e.buffer(x.as.bytes));
  return o;
}
napi_value writeMemory(Env e, const ta_memory_stats_t &x) {
  auto o = e.object();
  e.set(o, "reserved_bytes", e.number((double)x.reserved_bytes));
  e.set(o, "peak_reserved_bytes", e.number((double)x.peak_reserved_bytes));
  e.set(o, "used_bytes", e.number((double)x.used_bytes));
  e.set(o, "chunk_count", e.number((double)x.chunk_count));
  e.set(o, "descendant_count", e.number((double)x.descendant_count));
  e.set(o, "failed_allocations", e.bigint(x.failed_allocations));
  return o;
}
napi_value writeDiagnostic(Env e, const ta_diagnostic_t &x) {
  auto o = e.object();
  e.set(o, "status", e.number(x.status));
  e.set(o, "actor", e.bigint(x.actor.id));
  e.set(o, "revision", e.bigint(x.revision));
  e.set(o, "state", e.number(x.state));
  e.set(o, "transition", e.number(x.transition));
  e.set(o, "detail", e.text(x.detail));
  return o;
}
napi_value writeActorInfo(Env e, const ta_actor_info_t &x) {
  auto o = e.object();
  e.set(o, "status", e.number(x.status));
  e.set(o, "reason", e.number(x.reason));
  e.set(o, "revision", e.bigint(x.revision));
  e.set(o, "sessionid", e.text(x.sessionid));
  e.set(o, "registered_name", e.text(x.registered_name));
  e.set(o, "state", writeData(e, x.state));
  e.set(o, "stable", e.boolean(x.stable));
  e.set(o, "external_events", e.number(x.external_events));
  e.set(o, "internal_events", e.number(x.internal_events));
  e.set(o, "delayed_sends", e.number(x.delayed_sends));
  e.set(o, "invocations", e.number(x.invocations));
  e.set(o, "memory", writeMemory(e, x.memory));
  return o;
}
napi_value writeRequest(Env e, const ta_io_request_t &x) {
  auto o = e.object();
  e.set(o, "request", e.bigint(x.request.id));
  e.set(o, "kind", e.number(x.kind));
  e.set(o, "actor", e.bigint(x.actor.id));
  e.set(o, "revision", e.bigint(x.revision));
  e.set(o, "invocation", e.bigint(x.invocation.id));
  e.set(o, "invokeid", e.text(x.invokeid));
  e.set(o, "type", e.text(x.type));
  e.set(o, "target", e.text(x.target));
  e.set(o, "event", writeEvent(e, x.event));
  e.set(o, "body", writeBody(e, x.body));
  e.set(o, "trace", writeTrace(e, x.trace));
  e.set(o, "accepted_ns", e.bigint(x.accepted_ns));
  e.set(o, "due_ns", e.bigint(x.due_ns));
  return o;
}
napi_value writeDescription(Env e, const ta_definition_desc_t &x,
                            System *owner) {
  DescriptionWalk walk(e);
  auto o = e.object();
  e.set(o, "name", e.text(x.name));
  e.set(o, "data_model", e.text(x.data_model));
  e.set(o, "code_owner", value(x.code_owner, e));
  auto dl = e.object();
  e.set(dl, "storage", e.number(x.data_layout.storage));
  auto lay = e.object();
  e.set(lay, "size", e.number((double)x.data_layout.bytes_layout.size));
  e.set(lay, "alignment",
        e.number((double)x.data_layout.bytes_layout.alignment));
  e.set(dl, "bytes_layout", lay);
  e.set(dl, "initial_bytes", e.buffer(x.data_layout.initial_bytes));
  e.set(dl, "schema", e.text(x.data_layout.schema));
  e.set(o, "data_layout", dl);
  e.set(o, "binding", e.number(x.binding));
  e.set(o, "root_script", e.bigint((uint64_t)x.root_script));
  auto st = e.array(x.state_count);
  for (uint32_t i = 0; i < x.state_count; i++)
    e.element(st, i, writeState(e, x.states[i], owner));
  e.set(o, "states", st);
  auto tr = e.array(x.transition_count);
  for (uint32_t i = 0; i < x.transition_count; i++)
    e.element(tr, i, writeTransition(e, x.transitions[i]));
  e.set(o, "transitions", tr);
  return o;
}
napi_value writeContext(System &s, ta_context_t *x) {
  ta_context_view_t v{};
  ta_context_inspect(x, &v);
  auto o = s.js.object();
  s.js.set(o, "actor", s.js.bigint(v.actor.id));
  s.js.set(o, "state", writeData(s.js, v.state));
  s.js.set(o, "phase", s.js.number(v.phase));
  s.js.set(o, "state_id", s.js.number(v.state_id));
  s.js.set(o, "transition_id", s.js.number(v.transition_id));
  s.js.set(o, "now_ns", s.js.bigint(v.now_ns));
  s.js.set(o, "revision", s.js.bigint(ta_definition_revision(v.definition)));
  const ta_definition_desc_t *d = ta_definition_description(v.definition);
  if (d)
    s.js.set(o, "code_owner", value(d->code_owner, s.js));
  s.js.set(o, "system",
           v.system ? writeSystemValues(s.js, *v.system) : s.js.null());
  s.js.set(o, "trace", writeTrace(s.js, v.trace));
  return o;
}
napi_value writeSystemValues(Env e, const ta_system_values_t &x) {
  auto o = e.object();
  e.set(o, "sessionid", e.text(x.sessionid));
  e.set(o, "name", e.text(x.name));
  e.set(o, "actor", e.bigint(x.actor.id));
  e.set(o, "revision", e.bigint(x.revision));
  if (x.event)
    e.set(o, "event", writeEvent(e, *x.event));
  auto a = e.array(x.ioprocessor_count);
  for (size_t i = 0; i < x.ioprocessor_count; i++) {
    auto q = e.object();
    e.set(q, "type", e.text(x.ioprocessors[i].type));
    e.set(q, "location", e.text(x.ioprocessors[i].location));
    e.element(a, i, q);
  }
  e.set(o, "ioprocessors", a);
  return o;
}
napi_value writeSystemStats(Env e, const ta_system_stats_t &x) {
  auto o = e.object();
  e.set(o, "memory", writeMemory(e, x.memory));
  e.set(o, "actors", e.number(x.actors));
  e.set(o, "runnable_actors", e.number(x.runnable_actors));
  e.set(o, "definitions", e.number(x.definitions));
  e.set(o, "names", e.number(x.names));
  e.set(o, "queued_events", e.number(x.queued_events));
  e.set(o, "delayed_sends", e.number(x.delayed_sends));
  e.set(o, "invocations", e.number(x.invocations));
  e.set(o, "io_requests", e.number(x.io_requests));
  e.set(o, "accepted_messages", e.bigint(x.accepted_messages));
  e.set(o, "rejected_messages", e.bigint(x.rejected_messages));
  e.set(o, "completed_macrosteps", e.bigint(x.completed_macrosteps));
  e.set(o, "microsteps", e.bigint(x.microsteps));
  e.set(o, "faulted_actors", e.bigint(x.faulted_actors));
  e.set(o, "dropped_trace_records", e.bigint(x.dropped_trace_records));
  return o;
}
napi_value constants(Env e) {
  auto o = e.object();
  e.set(o, "TA_ABI_VERSION", e.number(TA_ABI_VERSION));
  e.set(o, "TA_INDEX_NONE", e.number(TA_INDEX_NONE));
  e.set(o, "TA_REVISION_ANY", e.bigint(TA_REVISION_ANY));
  e.set(o, "TA_TIME_NEVER", e.bigint(TA_TIME_NEVER));
  e.set(o, "TA_UNLIMITED", e.number(INFINITY));
  e.set(o, "TA_OK", e.number(TA_OK));
  e.set(o, "TA_EMPTY", e.number(TA_EMPTY));
  e.set(o, "TA_AGAIN", e.number(TA_AGAIN));
  e.set(o, "TA_INVALID", e.number(TA_INVALID));
  e.set(o, "TA_UNSUPPORTED", e.number(TA_UNSUPPORTED));
  e.set(o, "TA_NOT_FOUND", e.number(TA_NOT_FOUND));
  e.set(o, "TA_STALE", e.number(TA_STALE));
  e.set(o, "TA_CONFLICT", e.number(TA_CONFLICT));
  e.set(o, "TA_BUSY", e.number(TA_BUSY));
  e.set(o, "TA_NO_MEMORY", e.number(TA_NO_MEMORY));
  e.set(o, "TA_LIMIT", e.number(TA_LIMIT));
  e.set(o, "TA_EXECUTION_ERROR", e.number(TA_EXECUTION_ERROR));
  e.set(o, "TA_COMMUNICATION_ERROR", e.number(TA_COMMUNICATION_ERROR));
  e.set(o, "TA_CANCELLED", e.number(TA_CANCELLED));
  e.set(o, "TA_FAULTED", e.number(TA_FAULTED));
  e.set(o, "TA_DATA_NATIVE", e.number(TA_DATA_NATIVE));
  e.set(o, "TA_DATA_BYTES", e.number(TA_DATA_BYTES));
  e.set(o, "TA_PAYLOAD_NONE", e.number(TA_PAYLOAD_NONE));
  e.set(o, "TA_PAYLOAD_NATIVE", e.number(TA_PAYLOAD_NATIVE));
  e.set(o, "TA_PAYLOAD_BYTES", e.number(TA_PAYLOAD_BYTES));
  e.set(o, "TA_BODY_NONE", e.number(TA_BODY_NONE));
  e.set(o, "TA_BODY_CONTENT", e.number(TA_BODY_CONTENT));
  e.set(o, "TA_BODY_PARAMETERS", e.number(TA_BODY_PARAMETERS));
  e.set(o, "TA_EVENT_EXTERNAL", e.number(TA_EVENT_EXTERNAL));
  e.set(o, "TA_EVENT_INTERNAL", e.number(TA_EVENT_INTERNAL));
  e.set(o, "TA_EVENT_PLATFORM", e.number(TA_EVENT_PLATFORM));
  e.set(o, "TA_TARGET_SELF", e.number(TA_TARGET_SELF));
  e.set(o, "TA_TARGET_INTERNAL", e.number(TA_TARGET_INTERNAL));
  e.set(o, "TA_TARGET_INSTANCE", e.number(TA_TARGET_INSTANCE));
  e.set(o, "TA_TARGET_NAME", e.number(TA_TARGET_NAME));
  e.set(o, "TA_TARGET_PARENT", e.number(TA_TARGET_PARENT));
  e.set(o, "TA_TARGET_INVOKE", e.number(TA_TARGET_INVOKE));
  e.set(o, "TA_TARGET_URI", e.number(TA_TARGET_URI));
  e.set(o, "TA_ACTION_RAISE", e.number(TA_ACTION_RAISE));
  e.set(o, "TA_ACTION_ASSIGN", e.number(TA_ACTION_ASSIGN));
  e.set(o, "TA_ACTION_SCRIPT", e.number(TA_ACTION_SCRIPT));
  e.set(o, "TA_ACTION_IF", e.number(TA_ACTION_IF));
  e.set(o, "TA_ACTION_FOREACH", e.number(TA_ACTION_FOREACH));
  e.set(o, "TA_ACTION_LOG", e.number(TA_ACTION_LOG));
  e.set(o, "TA_ACTION_SEND", e.number(TA_ACTION_SEND));
  e.set(o, "TA_ACTION_CANCEL", e.number(TA_ACTION_CANCEL));
  e.set(o, "TA_ACTION_CUSTOM", e.number(TA_ACTION_CUSTOM));
  e.set(o, "TA_TRANSITION_EXTERNAL", e.number(TA_TRANSITION_EXTERNAL));
  e.set(o, "TA_TRANSITION_INTERNAL", e.number(TA_TRANSITION_INTERNAL));
  e.set(o, "TA_BINDING_EARLY", e.number(TA_BINDING_EARLY));
  e.set(o, "TA_BINDING_LATE", e.number(TA_BINDING_LATE));
  e.set(o, "TA_FINALIZE_ABSENT", e.number(TA_FINALIZE_ABSENT));
  e.set(o, "TA_FINALIZE_AUTOMATIC", e.number(TA_FINALIZE_AUTOMATIC));
  e.set(o, "TA_FINALIZE_BLOCK", e.number(TA_FINALIZE_BLOCK));
  e.set(o, "TA_IO_SEND", e.number(TA_IO_SEND));
  e.set(o, "TA_IO_INVOKE_START", e.number(TA_IO_INVOKE_START));
  e.set(o, "TA_IO_INVOKE_CANCEL", e.number(TA_IO_INVOKE_CANCEL));
  e.set(o, "TA_STATE_ROOT", e.number(TA_STATE_ROOT));
  e.set(o, "TA_STATE_ATOMIC", e.number(TA_STATE_ATOMIC));
  e.set(o, "TA_STATE_COMPOUND", e.number(TA_STATE_COMPOUND));
  e.set(o, "TA_STATE_PARALLEL", e.number(TA_STATE_PARALLEL));
  e.set(o, "TA_STATE_FINAL", e.number(TA_STATE_FINAL));
  e.set(o, "TA_STATE_HISTORY_SHALLOW", e.number(TA_STATE_HISTORY_SHALLOW));
  e.set(o, "TA_STATE_HISTORY_DEEP", e.number(TA_STATE_HISTORY_DEEP));
  e.set(o, "TA_LIFETIME_SYSTEM", e.number(TA_LIFETIME_SYSTEM));
  e.set(o, "TA_LIFETIME_DEFINITION", e.number(TA_LIFETIME_DEFINITION));
  e.set(o, "TA_LIFETIME_ACTOR", e.number(TA_LIFETIME_ACTOR));
  e.set(o, "TA_LIFETIME_ACTIVATION", e.number(TA_LIFETIME_ACTIVATION));
  e.set(o, "TA_LIFETIME_MESSAGE", e.number(TA_LIFETIME_MESSAGE));
  e.set(o, "TA_LIFETIME_MACROSTEP", e.number(TA_LIFETIME_MACROSTEP));
  e.set(o, "TA_LIFETIME_IO", e.number(TA_LIFETIME_IO));
  e.set(o, "TA_LIFETIME_TRACE", e.number(TA_LIFETIME_TRACE));
  e.set(o, "TA_LIFETIME_UPGRADE", e.number(TA_LIFETIME_UPGRADE));
  e.set(o, "TA_LIFETIME_COUNT", e.number(TA_LIFETIME_COUNT));
  e.set(o, "TA_CHART_STATE", e.number(TA_CHART_STATE));
  e.set(o, "TA_CHART_PARALLEL", e.number(TA_CHART_PARALLEL));
  e.set(o, "TA_CHART_FINAL", e.number(TA_CHART_FINAL));
  e.set(o, "TA_CHART_HISTORY_SHALLOW", e.number(TA_CHART_HISTORY_SHALLOW));
  e.set(o, "TA_CHART_HISTORY_DEEP", e.number(TA_CHART_HISTORY_DEEP));
  e.set(o, "TA_ACTOR_STARTING", e.number(TA_ACTOR_STARTING));
  e.set(o, "TA_ACTOR_READY", e.number(TA_ACTOR_READY));
  e.set(o, "TA_ACTOR_SUSPENDED", e.number(TA_ACTOR_SUSPENDED));
  e.set(o, "TA_ACTOR_STOPPING", e.number(TA_ACTOR_STOPPING));
  e.set(o, "TA_ACTOR_DONE", e.number(TA_ACTOR_DONE));
  e.set(o, "TA_ACTOR_STOPPED", e.number(TA_ACTOR_STOPPED));
  e.set(o, "TA_ACTOR_FAILED", e.number(TA_ACTOR_FAILED));
  e.set(o, "TA_INSPECT_ACTOR", e.number(TA_INSPECT_ACTOR));
  e.set(o, "TA_INSPECT_ACTIVE_STATE", e.number(TA_INSPECT_ACTIVE_STATE));
  e.set(o, "TA_INSPECT_HISTORY_STATE", e.number(TA_INSPECT_HISTORY_STATE));
  e.set(o, "TA_INSPECT_CURRENT_EVENT", e.number(TA_INSPECT_CURRENT_EVENT));
  e.set(o, "TA_INSPECT_DATA", e.number(TA_INSPECT_DATA));
  e.set(o, "TA_INSPECT_ACTIVE", e.number(TA_INSPECT_ACTIVE));
  e.set(o, "TA_INSPECT_HISTORY", e.number(TA_INSPECT_HISTORY));
  e.set(o, "TA_INSPECT_EVENT", e.number(TA_INSPECT_EVENT));
  e.set(o, "TA_VISIT_CONTINUE", e.number(TA_VISIT_CONTINUE));
  e.set(o, "TA_VISIT_PAUSE", e.number(TA_VISIT_PAUSE));
  e.set(o, "TA_VISIT_STOP", e.number(TA_VISIT_STOP));
  e.set(o, "TA_SPAN_SEND", e.number(TA_SPAN_SEND));
  e.set(o, "TA_SPAN_PROCESS", e.number(TA_SPAN_PROCESS));
  e.set(o, "TA_SPAN_INVOKE", e.number(TA_SPAN_INVOKE));
  e.set(o, "TA_SPAN_UPGRADE", e.number(TA_SPAN_UPGRADE));
  e.set(o, "TA_SPAN_INTERNAL", e.number(TA_SPAN_INTERNAL));
  e.set(o, "TA_SPAN_PRODUCER", e.number(TA_SPAN_PRODUCER));
  e.set(o, "TA_SPAN_CONSUMER", e.number(TA_SPAN_CONSUMER));
  e.set(o, "TA_SPAN_CLIENT", e.number(TA_SPAN_CLIENT));
  e.set(o, "TA_PHASE_INITIALIZE", e.number(TA_PHASE_INITIALIZE));
  e.set(o, "TA_PHASE_ROOT_SCRIPT", e.number(TA_PHASE_ROOT_SCRIPT));
  e.set(o, "TA_PHASE_GUARD", e.number(TA_PHASE_GUARD));
  e.set(o, "TA_PHASE_ONENTRY", e.number(TA_PHASE_ONENTRY));
  e.set(o, "TA_PHASE_ONEXIT", e.number(TA_PHASE_ONEXIT));
  e.set(o, "TA_PHASE_TRANSITION", e.number(TA_PHASE_TRANSITION));
  e.set(o, "TA_PHASE_FINALIZE", e.number(TA_PHASE_FINALIZE));
  e.set(o, "TA_PHASE_INVOKE", e.number(TA_PHASE_INVOKE));
  e.set(o, "TA_PHASE_DONE_DATA", e.number(TA_PHASE_DONE_DATA));
  e.set(o, "TA_PHASE_MIGRATE", e.number(TA_PHASE_MIGRATE));
  e.set(o, "TA_DEFINITION_COPY", e.number(TA_DEFINITION_COPY));
  e.set(o, "TA_DEFINITION_BORROW", e.number(TA_DEFINITION_BORROW));
  e.set(o, "TA_SCXML_IOPROCESSOR", e.string(TA_SCXML_IOPROCESSOR));
  e.set(o, "TA_SCXML_INVOKE_TYPE", e.string(TA_SCXML_INVOKE_TYPE));
  e.set(o, "TA_BASIC_HTTP_IOPROCESSOR", e.string(TA_BASIC_HTTP_IOPROCESSOR));
  return o;
}
} // namespace tn
