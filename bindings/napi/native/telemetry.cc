#include "bridge.hpp"

namespace tn {
namespace {
struct Scope {
  napi_env e;
  napi_handle_scope h = nullptr;
  explicit Scope(Env x) : e(x.env) {
    if (napi_open_handle_scope(e, &h) != napi_ok)
      throw std::runtime_error("Cannot open N-API handle scope");
  }
  ~Scope() {
    if (h)
      napi_close_handle_scope(e, h);
  }
};
struct Owned {
  System *s;
  ta_value_t v{};
  ~Owned() {
    if (v.handle)
      releaseRoot(s, v);
  }
  void keep(ta_value_t x) { v = x; }
  ta_value_t take() {
    ta_value_t x = v;
    v = {};
    return x;
  }
};
static napi_value top(System &s, const char *n) {
  return s.callback(nullptr, n);
}
template <class F> static ta_status_t safe(System &s, F f) {
  try {
    if (s.closing || s.silent || addon(s.js).closing)
      return TA_CANCELLED;
    return f();
  } catch (const std::exception &e) {
    s.capture(e);
    return TA_EXECUTION_ERROR;
  } catch (...) {
    return TA_EXECUTION_ERROR;
  }
}
template <class F> static void vsafe(System &s, F f) {
  try {
    if (!s.closing && !s.silent && !addon(s.js).closing)
      f();
  } catch (const std::exception &e) {
    s.capture(e);
  } catch (...) {
  }
}
static napi_value diagnostic(System &s, const ta_diagnostic_t &d) {
  return writeDiagnostic(s.js, d);
}
static ta_status_t validate(void *user, const ta_definition_desc_t *desc,
                            ta_diagnostic_t *error) {
  auto &s = *static_cast<System *>(user);
  return safe(s, [&] {
    Scope scope(s.js);
    napi_value result;
    auto status = s.invoke(top(s, "validate_code"),
                           {writeDescription(s.js, *desc, &s)}, &result);
    if (status != TA_OK)
      return status;
    auto code =
        s.js.u32(s.js.isObject(result) ? s.js.get(result, "status") : result);
    if (code > TA_FAULTED)
      throw std::range_error("Invalid validation status");
    error->status = static_cast<ta_status_t>(code);
    if (s.js.isObject(result)) {
      auto details = s.js.get(result, "error");
      error->state = s.js.u32(s.js.get(details, "state"), TA_INDEX_NONE);
      error->transition =
          s.js.u32(s.js.get(details, "transition"), TA_INDEX_NONE);
    }
    return error->status;
  });
}
static void report(void *u, const ta_diagnostic_t *d) {
  System &s = *static_cast<System *>(u);
  vsafe(s, [&] {
    Scope q(s.js);
    s.invoke(top(s, "diagnostic"), {diagnostic(s, *d)}, nullptr);
  });
}
static void terminal(void *u, const ta_actor_termination_t *t) {
  System &s = *static_cast<System *>(u);
  vsafe(s, [&] {
    Scope q(s.js);
    auto o = s.js.object();
    s.js.set(o, "actor", s.js.bigint(t->actor.id));
    s.js.set(o, "status", s.js.number(t->status));
    s.js.set(o, "reason", s.js.number(t->reason));
    s.invoke(top(s, "terminal"), {o}, nullptr);
  });
}
static napi_value traceStartArgs(System &s, const ta_span_start_t *x) {
  auto o = s.js.object();
  s.js.set(o, "operation", s.js.number(x->operation));
  s.js.set(o, "kind", s.js.number(x->kind));
  s.js.set(o, "actor", s.js.bigint(x->actor.id));
  s.js.set(o, "revision", s.js.bigint(x->revision));
  s.js.set(o, "message_id", s.js.bigint(x->message_id));
  s.js.set(o, "definition_name", s.js.text(x->definition_name));
  s.js.set(o, "event_name", s.js.text(x->event_name));
  if (x->event_data)
    s.js.set(o, "event_data", writePayload(s.js, *x->event_data));
  s.js.set(o, "destination", s.js.text(x->destination));
  s.js.set(o, "processor", s.js.text(x->processor));
  s.js.set(o, "now_ns", s.js.bigint(x->now_ns));
  s.js.set(o, "parent", writeTrace(s.js, x->parent));
  auto a = s.js.array(x->link_count);
  for (size_t i = 0; i < x->link_count; i++)
    s.js.element(a, i, writeTrace(s.js, &x->links[i]));
  s.js.set(o, "links", a);
  return o;
}
static ta_status_t arenaText(ta_arena_t *arena, ta_text_t input,
                             ta_text_t *out) {
  if (!input.data) {
    *out = {};
    return TA_OK;
  }
  void *data = nullptr;
  ta_layout_t layout{input.size + 1, 1};
  auto status = ta_arena_alloc(arena, &layout, &data);
  if (status == TA_OK) {
    if (input.size)
      std::memcpy(data, input.data, input.size);
    *out = {static_cast<const char *>(data), input.size};
  }
  return status;
}
static ta_status_t start(void *user, const ta_span_start_t *input,
                         ta_span_t *out) {
  auto &s = *static_cast<System *>(user);
  return safe(s, [&] {
    Scope scope(s.js);
    napi_value result;
    auto status = s.invoke(s.callback("trace", "start"),
                           {traceStartArgs(s, input)}, &result);
    if (status != TA_OK)
      return status;
    if (!s.js.isObject(result))
      return TA_INVALID;
    Parse parser(s.js, &s);
    auto context = readTrace(parser, s.js.get(result, "context"));
    ta_span_t candidate{};
    if (context) {
      candidate.context = *context;
      status = arenaText(input->arena, context->tracestate,
                         &candidate.context.tracestate);
      if (status != TA_OK)
        return status;
      status =
          arenaText(input->arena, context->baggage, &candidate.context.baggage);
      if (status != TA_OK)
        return status;
    }
    auto handle = s.js.get(result, "handle");
    if (!s.js.absent(handle))
      candidate.handle = root(s, handle).handle;
    *out = candidate;
    return TA_OK;
  });
}
static void error(void *u, uintptr_t h, const ta_diagnostic_t *d) {
  System &s = *static_cast<System *>(u);
  vsafe(s, [&] {
    Scope q(s.js);
    s.invoke(s.callback("trace", "error"),
             {value({h}, s.js), diagnostic(s, *d)}, nullptr);
  });
}
static void end(void *user, const ta_span_end_t *input) {
  auto &s = *static_cast<System *>(user);
  Owned handle{&s, {input && input->span ? input->span->handle : 0}};
  vsafe(s, [&] {
    Scope scope(s.js);
    auto out = s.js.object();
    s.js.set(out, "handle", value(handle.v, s.js));
    s.js.set(out, "context", writeTrace(s.js, &input->span->context));
    s.js.set(out, "now_ns", s.js.bigint(input->now_ns));
    s.js.set(out, "status", s.js.number(input->status));
    s.invoke(s.callback("trace", "end"), {out}, nullptr);
  });
}
} // namespace
void installTelemetry(System &s, napi_value) {
  auto &c = s.config;
  for (const char *name :
       {"validate_code", "diagnostic", "terminal", "snapshot"}) {
    auto fn = s.callback(nullptr, name);
    if (!s.js.absent(fn) && !s.js.isFunction(fn))
      throw std::invalid_argument(std::string(name) + " must be a function");
  }
  for (const char *name : {"start", "error", "end"}) {
    auto fn = s.callback("trace", name);
    if (!s.js.absent(fn) && !s.js.isFunction(fn))
      throw std::invalid_argument(std::string("trace.") + name +
                                  " must be a function");
  }
  c.validate_code = s.js.isFunction(s.callback(nullptr, "validate_code"))
                        ? validate
                        : nullptr;
  c.diagnostic =
      s.js.isFunction(s.callback(nullptr, "diagnostic")) ? report : nullptr;
  c.terminal =
      s.js.isFunction(s.callback(nullptr, "terminal")) ? terminal : nullptr;
  auto st = s.callback("trace", "start"), en = s.callback("trace", "end");
  if (s.js.isFunction(st) != s.js.isFunction(en))
    throw std::invalid_argument(
        "trace.start and trace.end must be supplied together");
  if (s.js.isFunction(st))
    c.trace = {&s, start,
               s.js.isFunction(s.callback("trace", "error")) ? error : nullptr,
               end};
}
} // namespace tn
