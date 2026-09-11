#include "bridge.hpp"
namespace tn {
namespace {
System &systemOf(ta_context_t *c) {
  return *static_cast<System *>(ta_context_host_user(c));
}
struct Scope {
  Env js;
  napi_handle_scope scope = nullptr;
  explicit Scope(Env env) : js(env) {
    js.check(napi_open_handle_scope(js.env, &scope));
  }
  ~Scope() {
    if (scope)
      napi_close_handle_scope(js.env, scope);
  }
};
template <class F> ta_status_t safe(System &s, F op) {
  if (s.closing || s.silent || addon(s.js).closing)
    return TA_CANCELLED;
  try {
    Scope scope(s.js);
    return op();
  } catch (const std::exception &e) {
    s.capture(e);
    return TA_EXECUTION_ERROR;
  } catch (...) {
    return TA_EXECUTION_ERROR;
  }
}
ta_status_t effect(System &s, const char *name,
                   const std::vector<napi_value> &args) {
  napi_value result;
  auto status = s.invoke(s.callback("data", name), args, &result);
  if (status != TA_OK)
    return status;
  auto code = s.js.u32(result, TA_OK);
  if (code > TA_FAULTED)
    throw std::range_error("Invalid status returned by data callback");
  return static_cast<ta_status_t>(code);
}
ta_status_t cloned(System &s, napi_value input, ta_value_t *out) {
  auto fn = s.callback(nullptr, "snapshot");
  napi_value result;
  if (!s.js.isFunction(fn))
    s.js.check(napi_get_reference_value(s.js.env, s.clone, &fn));
  auto status = s.invoke(fn, {input}, &result);
  if (status == TA_OK)
    *out = root(s, result);
  return status;
}
ta_status_t snapshot(void *user, ta_value_t input, ta_value_t *out) {
  auto &s = *static_cast<System *>(user);
  return safe(s, [&] { return cloned(s, value(input, s.js), out); });
}
ta_status_t create(ta_context_t *context, ta_value_t seed, ta_value_t *out) {
  auto &s = systemOf(context);
  return safe(s, [&] {
    CallbackContext ctx(context);
    auto fn = s.callback("data", "create");
    if (!s.js.isFunction(fn)) {
      if (seed.handle)
        return cloned(s, value(seed, s.js), out);
      *out = root(s, s.js.object());
      return TA_OK;
    }
    napi_value result;
    auto status = s.invoke(fn, {ctx.object, value(seed, s.js)}, &result);
    if (status == TA_OK)
      *out = root(s, result);
    return status;
  });
}
struct CodeCall {
  ta_context_t *context;
  ta_code_t code;
  ta_value_t *out;
};
ta_status_t codeValue(CodeCall call, const char *name) {
  auto &s = systemOf(call.context);
  return safe(s, [&] {
    CallbackContext ctx(call.context);
    napi_value result;
    auto status = s.invoke(s.callback("data", name),
                           {ctx.object, s.js.bigint(call.code)}, &result);
    if (status == TA_OK)
      *call.out = root(s, result);
    return status;
  });
}
ta_status_t evaluate(ta_context_t *c, ta_code_t code, ta_value_t *out) {
  return codeValue({c, code, out}, "evaluate");
}
ta_status_t readLocation(ta_context_t *c, ta_code_t code, ta_value_t *out) {
  return codeValue({c, code, out}, "read_location");
}
ta_status_t test(ta_context_t *context, ta_code_t code, uint8_t *out) {
  auto &s = systemOf(context);
  return safe(s, [&] {
    CallbackContext ctx(context);
    napi_value result;
    auto status = s.invoke(s.callback("data", "test"),
                           {ctx.object, s.js.bigint(code)}, &result);
    if (status == TA_OK)
      *out = s.js.truth(result);
    return status;
  });
}
ta_status_t assign(ta_context_t *c, ta_code_t code, ta_value_t input) {
  auto &s = systemOf(c);
  return safe(s, [&] {
    CallbackContext ctx(c);
    return effect(s, "assign",
                  {ctx.object, s.js.bigint(code), value(input, s.js)});
  });
}
ta_status_t run(ta_context_t *c, ta_action_kind_t kind, ta_code_t code) {
  auto &s = systemOf(c);
  return safe(s, [&] {
    CallbackContext ctx(c);
    return effect(s, "run", {ctx.object, s.js.number(kind), s.js.bigint(code)});
  });
}
ta_status_t bindSystem(ta_context_t *c, const ta_system_values_t *input) {
  auto &s = systemOf(c);
  return safe(s, [&] {
    if (!s.js.isFunction(s.callback("data", "bind_system")))
      return TA_OK;
    CallbackContext ctx(c);
    return effect(s, "bind_system",
                  {ctx.object, writeSystemValues(s.js, *input)});
  });
}
ta_status_t bindEvent(ta_context_t *c, const ta_event_t *input) {
  auto &s = systemOf(c);
  return safe(s, [&] {
    if (!s.js.isFunction(s.callback("data", "bind_event")))
      return TA_OK;
    CallbackContext ctx(c);
    return effect(s, "bind_event", {ctx.object, writeEvent(s.js, *input)});
  });
}
struct BindingCall {
  ta_context_t *context;
  const ta_data_binding_t *binding;
};
ta_status_t bindData(BindingCall call, const char *name) {
  auto &s = systemOf(call.context);
  return safe(s, [&] {
    CallbackContext ctx(call.context);
    auto input = s.js.object();
    auto &b = *call.binding;
    s.js.set(input, "owner", s.js.number(b.owner));
    s.js.set(input, "override_value", value(b.override_value, s.js));
    s.js.set(input, "has_override", s.js.boolean(b.override_value.handle != 0));
    auto declaration = s.js.object();
    s.js.set(declaration, "id", s.js.text(b.declaration->id));
    s.js.set(declaration, "initializer",
             s.js.bigint(b.declaration->initializer));
    s.js.set(input, "declaration", declaration);
    return effect(s, name, {ctx.object, input});
  });
}
ta_status_t declare(ta_context_t *c, const ta_data_binding_t *b) {
  return bindData({c, b}, "declare");
}
ta_status_t initialize(ta_context_t *c, const ta_data_binding_t *b) {
  return bindData({c, b}, "initialize");
}
ta_status_t copyBytes(ta_context_t *c, ta_bytes_t input, ta_bytes_t *out) {
  ta_arena_t *arena = nullptr;
  auto status = ta_context_arena(c, TA_LIFETIME_MACROSTEP, &arena);
  if (status != TA_OK)
    return status;
  ta_layout_t layout{input.size ? input.size : 1, 1};
  void *memory = nullptr;
  status = ta_arena_alloc(arena, &layout, &memory);
  if (status == TA_OK) {
    if (input.size)
      std::memcpy(memory, input.data, input.size);
    *out = {memory, input.size};
  }
  return status;
}
ta_status_t toText(ta_context_t *c, ta_value_t input, ta_text_t *out) {
  auto &s = systemOf(c);
  return safe(s, [&] {
    CallbackContext ctx(c);
    napi_value result;
    auto status = s.invoke(s.callback("data", "to_text"),
                           {ctx.object, value(input, s.js)}, &result);
    if (status != TA_OK)
      return status;
    auto text = s.js.utf8(result);
    ta_bytes_t copied{};
    status = copyBytes(c, {text.data(), text.size()}, &copied);
    if (status == TA_OK)
      *out = {static_cast<const char *>(copied.data), copied.size};
    return status;
  });
}
ta_status_t fromText(void *user, ta_text_t input, ta_value_t *out) {
  auto &s = *static_cast<System *>(user);
  return safe(s, [&] {
    auto text = s.js.text(input), fn = s.callback("data", "from_text");
    napi_value result = text;
    if (s.js.isFunction(fn)) {
      auto status = s.invoke(fn, {text}, &result);
      if (status != TA_OK)
        return status;
    }
    *out = root(s, result);
    return TA_OK;
  });
}
ta_status_t evaluateBytes(ta_context_t *c, ta_code_t code, ta_bytes_t *out) {
  auto &s = systemOf(c);
  return safe(s, [&] {
    CallbackContext ctx(c);
    napi_value result;
    auto status = s.invoke(s.callback("data", "evaluate_bytes"),
                           {ctx.object, s.js.bigint(code)}, &result);
    return status == TA_OK ? copyBytes(c, s.js.bytes(result), out) : status;
  });
}
ta_status_t toDelay(ta_context_t *c, ta_value_t input, uint64_t *out) {
  auto &s = systemOf(c);
  return safe(s, [&] {
    CallbackContext ctx(c);
    napi_value result;
    auto status = s.invoke(s.callback("data", "to_delay"),
                           {ctx.object, value(input, s.js)}, &result);
    if (status == TA_OK)
      *out = s.js.u64(result);
    return status;
  });
}
ta_status_t iterationOpen(ta_context_t *c, ta_code_t code,
                          ta_iteration_t *out) {
  auto &s = systemOf(c);
  return safe(s, [&] {
    CallbackContext ctx(c);
    napi_value result;
    auto status = s.invoke(s.callback("data", "iteration_open"),
                           {ctx.object, s.js.bigint(code)}, &result);
    if (status != TA_OK)
      return status;
    size_t count = s.js.size(s.js.get(result, "count"));
    auto items = s.js.get(result, "snapshot");
    *out = {root(s, items), count};
    return TA_OK;
  });
}
ta_status_t iterationBind(ta_context_t *c, const ta_iteration_step_t *step) {
  auto &s = systemOf(c);
  return safe(s, [&] {
    CallbackContext ctx(c);
    auto input = s.js.object();
    s.js.set(input, "snapshot", value(step->snapshot, s.js));
    s.js.set(input, "ordinal", s.js.number(step->ordinal));
    s.js.set(input, "item", s.js.text(step->item));
    s.js.set(input, "index", s.js.text(step->index));
    return effect(s, "iteration_bind", {ctx.object, input});
  });
}
ta_status_t packBody(void *user, const ta_body_t *body, ta_value_t *out) {
  auto &s = *static_cast<System *>(user);
  return safe(s, [&] {
    auto fn = s.callback("data", "pack_body");
    napi_value result;
    if (s.js.isFunction(fn)) {
      auto status = s.invoke(fn, {writeBody(s.js, *body)}, &result);
      if (status != TA_OK)
        return status;
    } else {
      result = s.js.object();
      for (size_t i = 0; i < body->parameter_count; ++i) {
        auto &parameter = body->parameters[i];
        auto name = s.js.text(parameter.name);
        napi_property_descriptor prop{nullptr,
                                      name,
                                      nullptr,
                                      nullptr,
                                      nullptr,
                                      value(parameter.value, s.js),
                                      napi_default_jsproperty,
                                      nullptr};
        s.js.check(napi_define_properties(s.js.env, result, 1, &prop));
      }
    }
    *out = root(s, result);
    return TA_OK;
  });
}
ta_status_t finalize(ta_context_t *c, const ta_finalize_call_t *call) {
  auto &s = systemOf(c);
  return safe(s, [&] {
    CallbackContext ctx(c);
    auto input = s.js.object(), parameters = s.js.array(call->count);
    for (size_t i = 0; i < call->count; ++i) {
      auto item = s.js.object();
      s.js.set(item, "name", s.js.text(call->parameters[i].name));
      s.js.set(item, "expression", s.js.bigint(call->parameters[i].expression));
      s.js.set(item, "location", s.js.bigint(call->parameters[i].location));
      s.js.element(parameters, i, item);
    }
    s.js.set(input, "parameters", parameters);
    s.js.set(input, "returned_data", writePayload(s.js, call->returned_data));
    return effect(s, "finalize", {ctx.object, input});
  });
}
void log(void *user, const ta_log_record_t *record) {
  auto &s = *static_cast<System *>(user);
  safe(s, [&] {
    auto input = s.js.object(), context = s.js.object();
    auto &view = record->context;
    s.js.set(context, "actor", s.js.bigint(view.actor.id));
    s.js.set(context, "state", writeData(s.js, view.state));
    s.js.set(context, "system",
             view.system ? writeSystemValues(s.js, *view.system) : s.js.null());
    s.js.set(context, "phase", s.js.number(view.phase));
    s.js.set(context, "state_id", s.js.number(view.state_id));
    s.js.set(context, "transition_id", s.js.number(view.transition_id));
    s.js.set(context, "now_ns", s.js.bigint(view.now_ns));
    s.js.set(context, "trace", writeTrace(s.js, view.trace));
    s.js.set(input, "context", context);
    s.js.set(input, "label", s.js.text(record->label));
    s.js.set(input, "value", value(record->value, s.js));
    return s.invoke(s.callback("data", "log"), {input}, nullptr);
  });
}
bool has(System &s, const char *name) {
  auto fn = s.callback("data", name);
  if (s.js.absent(fn))
    return false;
  if (!s.js.isFunction(fn))
    throw std::invalid_argument(std::string("data.") + name +
                                " must be a function");
  return true;
}
} // namespace
void installCallbacks(System &s, napi_value) {
  s.config.host_user = &s;
  s.config.values = {retainRoot, releaseRoot, snapshot};
  auto &d = s.config.data;
  has(s, "create");
  has(s, "bind_system");
  has(s, "bind_event");
  has(s, "from_text");
  has(s, "pack_body");
  d.create = create;
  d.bind_system = bindSystem;
  d.bind_event = bindEvent;
  d.from_text = fromText;
  d.pack_body = packBody;
  d.declare = has(s, "declare") ? declare : nullptr;
  d.initialize = has(s, "initialize") ? initialize : nullptr;
  d.evaluate = has(s, "evaluate") ? evaluate : nullptr;
  d.read_location = has(s, "read_location") ? readLocation : nullptr;
  d.test = has(s, "test") ? test : nullptr;
  d.assign = has(s, "assign") ? assign : nullptr;
  d.run = has(s, "run") ? run : nullptr;
  d.to_text = has(s, "to_text") ? toText : nullptr;
  d.to_delay = has(s, "to_delay") ? toDelay : nullptr;
  d.evaluate_bytes = has(s, "evaluate_bytes") ? evaluateBytes : nullptr;
  d.iteration_open = has(s, "iteration_open") ? iterationOpen : nullptr;
  d.iteration_bind = has(s, "iteration_bind") ? iterationBind : nullptr;
  d.finalize = has(s, "finalize") ? finalize : nullptr;
  d.log = has(s, "log") ? log : nullptr;
}
} // namespace tn
