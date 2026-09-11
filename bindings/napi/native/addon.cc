#include "bridge.hpp"

namespace tn {
namespace {
enum class Op {
  ArenaCreate,
  ArenaOpen,
  ArenaAlloc,
  ArenaDefer,
  ArenaClose,
  ArenaStats,
  ArenaLimit,
  ConfigInit,
  SystemCreate,
  SystemLimits,
  SystemRun,
  SystemDeadline,
  SystemInspect,
  SystemStats,
  SystemShutdown,
  SystemDestroy,
  DefPrepare,
  DefBuild,
  DefRetain,
  DefRelease,
  DefRevision,
  DefDescription,
  DefFootprint,
  DefPublish,
  DefUnpublish,
  DefLookup,
  ActorSpawn,
  ActorInspect,
  ActorLimits,
  ActorStop,
  ActorRelease,
  ActorUpgrade,
  NameBind,
  NameUnbind,
  NameLookup,
  SessionLookup,
  TargetParse,
  Post,
  ContextInspect,
  ContextIn,
  ContextUser,
  ContextBytes,
  ContextArena,
  ContextRaise,
  ContextSend,
  ContextCancel,
  IoNext,
  IoComplete,
  IoReceive,
  IoError,
  InvocationEmit,
  InvocationDone,
  InvocationFail,
  InvocationCancelled,
  Text,
  MemoryRead,
  MemoryWrite,
  ContextWrite,
  TakeErrors,
  Dispose,
  BindingStats
};
struct Call {
  Env js;
  Op op;
  napi_value args[3];
  size_t argc = 3;
  napi_value at(size_t index) const {
    return index < argc ? args[index] : js.undefined();
  }
};
struct NamedOp {
  const char *name;
  Op op;
};
const NamedOp operations[] = {
    {"ta_arena_create", Op::ArenaCreate},
    {"ta_arena_open", Op::ArenaOpen},
    {"ta_arena_alloc", Op::ArenaAlloc},
    {"ta_arena_defer", Op::ArenaDefer},
    {"ta_arena_close", Op::ArenaClose},
    {"ta_arena_stats", Op::ArenaStats},
    {"ta_arena_set_limit", Op::ArenaLimit},
    {"ta_system_config_init", Op::ConfigInit},
    {"ta_system_create", Op::SystemCreate},
    {"ta_system_set_limits", Op::SystemLimits},
    {"ta_system_run", Op::SystemRun},
    {"ta_system_next_deadline", Op::SystemDeadline},
    {"ta_system_inspect", Op::SystemInspect},
    {"ta_system_stats", Op::SystemStats},
    {"ta_system_shutdown", Op::SystemShutdown},
    {"ta_system_destroy", Op::SystemDestroy},
    {"ta_definition_prepare", Op::DefPrepare},
    {"ta_definition_build", Op::DefBuild},
    {"ta_definition_retain", Op::DefRetain},
    {"ta_definition_release", Op::DefRelease},
    {"ta_definition_revision", Op::DefRevision},
    {"ta_definition_description", Op::DefDescription},
    {"ta_definition_footprint", Op::DefFootprint},
    {"ta_definition_publish", Op::DefPublish},
    {"ta_definition_unpublish", Op::DefUnpublish},
    {"ta_definition_lookup", Op::DefLookup},
    {"ta_actor_spawn", Op::ActorSpawn},
    {"ta_actor_inspect", Op::ActorInspect},
    {"ta_actor_set_limits", Op::ActorLimits},
    {"ta_actor_stop", Op::ActorStop},
    {"ta_actor_release", Op::ActorRelease},
    {"ta_actor_upgrade", Op::ActorUpgrade},
    {"ta_name_bind", Op::NameBind},
    {"ta_name_unbind", Op::NameUnbind},
    {"ta_name_lookup", Op::NameLookup},
    {"ta_session_lookup", Op::SessionLookup},
    {"ta_target_parse", Op::TargetParse},
    {"ta_post", Op::Post},
    {"ta_context_inspect", Op::ContextInspect},
    {"ta_context_in", Op::ContextIn},
    {"ta_context_host_user", Op::ContextUser},
    {"ta_context_bytes", Op::ContextBytes},
    {"ta_context_arena", Op::ContextArena},
    {"ta_context_raise", Op::ContextRaise},
    {"ta_context_send", Op::ContextSend},
    {"ta_context_cancel", Op::ContextCancel},
    {"ta_io_next", Op::IoNext},
    {"ta_io_complete", Op::IoComplete},
    {"ta_io_receive", Op::IoReceive},
    {"ta_io_receive_error", Op::IoError},
    {"ta_invocation_emit", Op::InvocationEmit},
    {"ta_invocation_done", Op::InvocationDone},
    {"ta_invocation_fail", Op::InvocationFail},
    {"ta_invocation_is_cancelled", Op::InvocationCancelled},
    {"ta_text_from_cstr", Op::Text},
    {"memoryRead", Op::MemoryRead},
    {"memoryWrite", Op::MemoryWrite},
    {"contextWriteBytes", Op::ContextWrite},
    {"takeErrors", Op::TakeErrors},
    {"disposeSystem", Op::Dispose},
    {"bindingStats", Op::BindingStats}};
struct SizeField {
  const char *name;
  size_t offset;
};
const SizeField actorFields[] = {
    {"maximum_bytes", offsetof(ta_actor_limits_t, maximum_bytes)},
    {"external_events", offsetof(ta_actor_limits_t, external_events)},
    {"internal_events", offsetof(ta_actor_limits_t, internal_events)},
    {"queued_event_bytes", offsetof(ta_actor_limits_t, queued_event_bytes)},
    {"delayed_sends", offsetof(ta_actor_limits_t, delayed_sends)},
    {"invocations", offsetof(ta_actor_limits_t, invocations)},
    {"io_requests", offsetof(ta_actor_limits_t, io_requests)},
    {"io_request_bytes", offsetof(ta_actor_limits_t, io_request_bytes)},
    {"microsteps_per_macrostep",
     offsetof(ta_actor_limits_t, microsteps_per_macrostep)},
    {"actions_per_macrostep",
     offsetof(ta_actor_limits_t, actions_per_macrostep)}};
const SizeField systemFields[] = {
    {"maximum_bytes", offsetof(ta_system_limits_t, maximum_bytes)},
    {"actors", offsetof(ta_system_limits_t, actors)},
    {"definitions", offsetof(ta_system_limits_t, definitions)},
    {"names", offsetof(ta_system_limits_t, names)},
    {"queued_events", offsetof(ta_system_limits_t, queued_events)},
    {"delayed_sends", offsetof(ta_system_limits_t, delayed_sends)},
    {"invocations", offsetof(ta_system_limits_t, invocations)},
    {"io_requests", offsetof(ta_system_limits_t, io_requests)},
    {"definition_states", offsetof(ta_system_limits_t, definition_states)},
    {"definition_transitions",
     offsetof(ta_system_limits_t, definition_transitions)},
    {"definition_actions", offsetof(ta_system_limits_t, definition_actions)},
    {"state_depth", offsetof(ta_system_limits_t, state_depth)},
    {"action_depth", offsetof(ta_system_limits_t, action_depth)},
    {"event_name_bytes", offsetof(ta_system_limits_t, event_name_bytes)},
    {"trace_metadata_bytes",
     offsetof(ta_system_limits_t, trace_metadata_bytes)}};
template <size_t N>
napi_value writeSizes(Env js, const void *input, const SizeField (&fields)[N]) {
  auto out = js.object();
  for (auto &field : fields) {
    size_t n = *reinterpret_cast<const size_t *>(
        static_cast<const char *>(input) + field.offset);
    js.set(out, field.name,
           js.number(n == SIZE_MAX ? INFINITY : static_cast<double>(n)));
  }
  return out;
}
napi_value writeArenaOptions(Env js, const ta_arena_options_t &o) {
  auto out = js.object();
  js.set(out, "initial_chunk_bytes", js.number(o.initial_chunk_bytes));
  js.set(out, "maximum_chunk_bytes",
         js.number(o.maximum_chunk_bytes == SIZE_MAX ? INFINITY
                                                     : o.maximum_chunk_bytes));
  js.set(out, "maximum_bytes",
         js.number(o.maximum_bytes == SIZE_MAX ? INFINITY : o.maximum_bytes));
  js.set(out, "maximum_allocation",
         js.number(o.maximum_allocation == SIZE_MAX ? INFINITY
                                                    : o.maximum_allocation));
  return out;
}
napi_value defaults(Env js) {
  ta_system_config_t config;
  ta_system_config_init(&config);
  auto out = js.object();
  js.set(out, "abi_version", js.number(config.abi_version));
  js.set(out, "struct_size", js.number(config.struct_size));
  js.set(out, "session_prefix", js.string("node"));
  js.set(out, "data_model", js.string("javascript"));
  js.set(out, "limits", writeSizes(js, &config.limits, systemFields));
  js.set(out, "actor_defaults",
         writeSizes(js, &config.actor_defaults, actorFields));
  auto arenas = js.array(TA_LIFETIME_COUNT);
  for (size_t i = 0; i < TA_LIFETIME_COUNT; ++i)
    js.element(arenas, i, writeArenaOptions(js, config.arenas[i]));
  js.set(out, "arenas", arenas);
  return out;
}
struct Location {
  System *owner;
  size_t index;
};
ta_status_t location(void *user, const ta_location_request_t *request,
                     ta_text_t *out) {
  auto &loc = *static_cast<Location *>(user);
  auto &s = *loc.owner;
  if (s.closing || s.silent)
    return TA_CANCELLED;
  napi_handle_scope scope = nullptr;
  try {
    s.js.check(napi_open_handle_scope(s.js.env, &scope));
    auto registrations = s.js.get(s.configuration(), "ioprocessors");
    auto fn = s.js.get(s.js.element(registrations, loc.index), "location");
    auto input = s.js.object();
    s.js.set(input, "actor", s.js.bigint(request->actor.id));
    s.js.set(input, "sessionid", s.js.text(request->sessionid));
    napi_value result;
    auto status = s.invoke(fn, {input}, &result);
    if (status == TA_OK && !s.js.absent(result)) {
      auto text = s.js.utf8(result);
      void *data;
      ta_layout_t layout{text.size() + 1, 1};
      status = ta_arena_alloc(request->scratch, &layout, &data);
      if (status == TA_OK) {
        std::memcpy(data, text.c_str(), text.size() + 1);
        *out = {static_cast<const char *>(data), text.size()};
      }
    }
    if (scope)
      napi_close_handle_scope(s.js.env, scope);
    return status;
  } catch (const std::exception &e) {
    s.capture(e);
    if (scope)
      napi_close_handle_scope(s.js.env, scope);
    return TA_EXECUTION_ERROR;
  }
}
const char *const *aliases(Parse &p, napi_value values) {
  auto count = p.js.length(values);
  if (!count)
    return nullptr;
  auto items = p.terminated<const char *>(count);
  for (size_t i = 0; i < count; ++i) {
    items[i] = p.cstr(p.js.element(values, i));
    if (!items[i] || !items[i][0])
      throw std::invalid_argument(
          "Processor/service aliases must be nonempty strings");
  }
  return items;
}
void registrations(Parse &p, napi_value input) {
  auto &s = *p.owner;
  Env js = p.js;
  auto processors = js.get(input, "ioprocessors");
  size_t count = js.length(processors);
  auto items = p.terminated<ta_ioprocessor_desc_t>(count);
  s.config.ioprocessors = items;
  for (size_t i = 0; i < count; ++i) {
    auto entry = js.element(processors, i);
    items[i].type = p.cstr(js.get(entry, "type"));
    if (!items[i].type || !items[i].type[0])
      throw std::invalid_argument("Every I/O processor needs a nonempty type");
    items[i].aliases = aliases(p, js.get(entry, "aliases"));
    auto fn = js.get(entry, "location");
    if (!js.absent(fn)) {
      if (!js.isFunction(fn))
        throw std::invalid_argument("ioprocessor.location must be a function");
      auto data = std::make_shared<Location>(Location{&s, i});
      items[i].user = data.get();
      items[i].location = location;
      s.registrations.push_back(data);
    }
  }
  auto services = js.get(input, "services");
  count = js.length(services);
  auto svc = p.terminated<ta_service_desc_t>(count);
  s.config.services = svc;
  for (size_t i = 0; i < count; ++i) {
    auto entry = js.element(services, i);
    svc[i].type = p.cstr(js.get(entry, "type"));
    if (!svc[i].type || !svc[i].type[0])
      throw std::invalid_argument("Every service needs a nonempty type");
    svc[i].aliases = aliases(p, js.get(entry, "aliases"));
    svc[i].event_processor = p.cstr(js.get(entry, "event_processor"));
  }
}
napi_value createSystem(Call &call) {
  Env js = call.js;
  auto input = call.at(1);
  if (js.absent(input))
    input = js.object();
  auto s = std::make_shared<System>(js);
  ta_system_config_init(&s->config);
  js.check(napi_create_reference(js.env, input, 0, &s->callbacks));
  napi_value global;
  js.check(napi_get_global(js.env, &global));
  auto clone = js.get(global, "structuredClone");
  if (!js.isFunction(clone))
    throw std::invalid_argument("Node structuredClone is required");
  js.check(napi_create_reference(js.env, clone, 1, &s->clone));
  Parse p(js, s.get());
  s->config.abi_version = js.u32(js.get(input, "abi_version"), TA_ABI_VERSION);
  s->config.struct_size =
      js.size(js.get(input, "struct_size"), sizeof(ta_system_config_t));
  auto prefix = js.get(input, "session_prefix");
  s->config.session_prefix =
      p.text(js.absent(prefix) ? js.string("node") : prefix);
  auto model = js.get(input, "data_model");
  s->config.data_model =
      p.text(js.absent(model) ? js.string("javascript") : model);
  s->config.limits =
      readSystemLimits(p, js.get(input, "limits"), &s->config.limits);
  s->config.actor_defaults = readActorLimits(p, js.get(input, "actor_defaults"),
                                             &s->config.actor_defaults);
  auto profiles = js.get(input, "arenas");
  size_t length = js.length(profiles);
  if (length > TA_LIFETIME_COUNT)
    throw std::range_error("Too many arena profiles");
  for (size_t i = 0; i < length; ++i)
    if (!js.absent(js.element(profiles, i)))
      s->config.arenas[i] = readArenaOptions(p, js.element(profiles, i));
  registrations(p, input);
  installCallbacks(*s, input);
  installTelemetry(*s, input);
  if (js.absent(call.at(0)))
    s->parent = createArena(js, {4096, SIZE_MAX, SIZE_MAX, SIZE_MAX});
  else
    s->parent = arena(js, call.at(0));
  ta_status_t status =
      ta_system_create(s->parent->arena, &s->config, &s->system);
  if (status != TA_OK)
    return js.result(status, "system");
  /* config text/registration pointers are consumed by core creation; all
     subsequent bridge uses of config are limits and callback tables. */
  s->config.session_prefix = {};
  s->config.data_model = {};
  s->config.ioprocessors = nullptr;
  s->config.services = nullptr;
  auto out = wrap(s, input);
  js.set(out, "parent", call.at(0));
  return js.result(status, "system", out);
}
struct Deferred {
  Env js;
  std::weak_ptr<Arena> owner;
  napi_ref callback = nullptr, argument = nullptr;
  ~Deferred() {
    if (callback)
      napi_delete_reference(js.env, callback);
    if (argument)
      napi_delete_reference(js.env, argument);
  }
};
void deferred(void *user) {
  std::unique_ptr<Deferred> d(static_cast<Deferred *>(user));
  auto a = d->owner.lock();
  if (!a || addon(d->js).closing)
    return;
  for (auto parent = a; parent; parent = parent->parent)
    if (parent->suppress)
      return;
  auto s = a->owner.lock();
  if (s && (s->closing || s->silent))
    return;
  napi_handle_scope scope = nullptr;
  try {
    d->js.check(napi_open_handle_scope(d->js.env, &scope));
    napi_value fn, box;
    d->js.check(napi_get_reference_value(d->js.env, d->callback, &fn));
    d->js.check(napi_get_reference_value(d->js.env, d->argument, &box));
    auto result = d->js.call(fn, d->js.undefined(), {d->js.element(box, 0)});
    bool promise = false;
    d->js.check(napi_is_promise(d->js.env, result, &promise));
    if (promise)
      throw std::invalid_argument("Arena cleanup must be synchronous");
  } catch (const std::exception &e) {
    if (s)
      s->capture(e);
    else {
      bool pending = false;
      napi_is_exception_pending(d->js.env, &pending);
      if (pending) {
        napi_value error;
        napi_get_and_clear_last_exception(d->js.env, &error);
      }
    }
  }
  if (scope)
    napi_close_handle_scope(d->js.env, scope);
}
bool containsArena(const std::shared_ptr<Arena> &child, const Arena *parent) {
  for (auto a = child; a; a = a->parent)
    if (a.get() == parent)
      return true;
  return false;
}
napi_value arenaCall(Call &call) {
  Env js = call.js;
  Parse p(js);
  if (call.op == Op::ArenaCreate) {
    if (!js.absent(call.at(0)))
      throw std::invalid_argument("Node owns the chunk allocator; pass null");
    auto options = readArenaOptions(p, call.at(1));
    auto a = std::make_shared<Arena>(js);
    auto alloc = allocator(js);
    auto status = ta_arena_create(&alloc, &options, &a->arena);
    if (status == TA_OK)
      watchArena(a);
    return js.result(status, "arena", status == TA_OK ? wrap(a) : nullptr);
  }
  auto a = arena(js, call.at(0));
  auto system = a->owner.lock();
  if (system && system->closing)
    throw std::invalid_argument("Arena owner is closing");
  switch (call.op) {
  case Op::ArenaOpen: {
    auto options = readArenaOptions(p, call.at(1));
    auto child = std::make_shared<Arena>(js);
    child->parent = a;
    child->owner = a->owner;
    auto status = ta_arena_open(a->arena, &options, &child->arena);
    if (status == TA_OK)
      watchArena(child);
    return js.result(status, "arena",
                     status == TA_OK ? wrap(child, call.at(0)) : nullptr);
  }
  case Op::ArenaAlloc: {
    auto layout = readLayout(p, call.at(1));
    auto memory = std::make_shared<Memory>(js);
    memory->owner = a;
    memory->size = layout.size;
    auto status = ta_arena_alloc(a->arena, &layout, &memory->data);
    return js.result(status, "memory",
                     status == TA_OK ? wrap(memory, call.at(0)) : nullptr);
  }
  case Op::ArenaDefer: {
    if (!js.isFunction(call.at(1)))
      throw std::invalid_argument("Arena cleanup must be a function");
    auto d = std::make_unique<Deferred>();
    d->js = js;
    d->owner = a;
    js.check(napi_create_reference(js.env, call.at(1), 1, &d->callback));
    auto box = js.array(1);
    js.element(box, 0, call.at(2));
    js.check(napi_create_reference(js.env, box, 1, &d->argument));
    auto status = ta_arena_defer(a->arena, deferred, d.get());
    if (status == TA_OK)
      d.release();
    return js.number(status);
  }
  case Op::ArenaClose: {
    if (a->borrowed)
      throw std::invalid_argument("Cannot close a runtime-owned arena");
    for (auto &weak : addon(js).resources)
      if (auto r = weak.lock())
        if (r->kind == Kind::System && !r->closed) {
          auto s = std::static_pointer_cast<System>(r);
          if (containsArena(s->parent, a.get()))
            throw std::invalid_argument("Destroy the contained actor system "
                                        "before closing its parent arena");
        }
    a->close();
    return js.undefined();
  }
  case Op::ArenaStats: {
    ta_memory_stats_t stats{};
    ta_arena_stats(a->arena, &stats);
    return writeMemory(js, stats);
  }
  case Op::ArenaLimit:
    return js.number(ta_arena_set_limit(a->arena, js.size(call.at(1))));
  default:
    throw std::logic_error("Invalid arena operation");
  }
}
std::shared_ptr<Definition> ownedDefinition(System &s, napi_value input) {
  auto d = definition(s.js, input);
  if (d->owner.get() != &s)
    throw std::invalid_argument("Definition belongs to another actor system");
  return d;
}
ta_message_t message(Parse &p, napi_value input) {
  return {p.text(p.js.get(input, "name")),
          readPayload(p, p.js.get(input, "data")),
          readTrace(p, p.js.get(input, "trace"))};
}
struct StorageLease {
  std::unique_ptr<Parse> parser;
  System *owner;
  napi_ref callback = nullptr, argument = nullptr;
  ~StorageLease() {
    if (callback)
      napi_delete_reference(owner->js.env, callback);
    if (argument)
      napi_delete_reference(owner->js.env, argument);
  }
};
void storageReleased(void *user) {
  std::unique_ptr<StorageLease> lease(static_cast<StorageLease *>(user));
  auto &s = *lease->owner;
  if (!lease->callback || s.closing || s.silent || addon(s.js).closing)
    return;
  napi_handle_scope scope = nullptr;
  try {
    s.js.check(napi_open_handle_scope(s.js.env, &scope));
    napi_value fn, box;
    s.js.check(napi_get_reference_value(s.js.env, lease->callback, &fn));
    s.js.check(napi_get_reference_value(s.js.env, lease->argument, &box));
    s.invoke(fn, {s.js.element(box, 0)}, nullptr);
  } catch (const std::exception &e) {
    s.capture(e);
  }
  if (scope)
    napi_close_handle_scope(s.js.env, scope);
}
napi_value prepare(Call &call, const std::shared_ptr<System> &s) {
  Env js = call.js;
  auto p = std::make_unique<Parse>(js, s.get());
  ta_definition_result_t result{};
  ta_status_t status;
  if (call.op == Op::DefBuild) {
    auto chart = readChart(*p, call.at(1));
    status = ta_definition_build(s->system, &chart, &result);
  } else {
    auto input = call.at(1);
    auto desc = p->items<ta_definition_desc_t>(1);
    *desc = readDescription(*p, js.get(input, "description"));
    auto options = js.get(input, "options");
    ta_definition_input_t native{desc,
                                 {static_cast<ta_definition_storage_t>(
                                      js.u32(js.get(options, "storage"))),
                                  nullptr, nullptr}};
    std::unique_ptr<StorageLease> lease;
    if (native.options.storage == TA_DEFINITION_BORROW) {
      lease = std::make_unique<StorageLease>();
      lease->owner = s.get();
      lease->parser = std::move(p);
      auto fn = js.get(options, "storage_release");
      if (!js.absent(fn)) {
        if (!js.isFunction(fn))
          throw std::invalid_argument("storage_release must be a function");
        js.check(napi_create_reference(js.env, fn, 1, &lease->callback));
        auto box = js.array(1);
        js.element(box, 0, js.get(options, "storage_user"));
        js.check(napi_create_reference(js.env, box, 1, &lease->argument));
      }
      native.options.storage_user = lease.get();
      native.options.storage_release = storageReleased;
    }
    status = ta_definition_prepare(s->system, &native, &result);
    if (status == TA_OK && lease)
      lease.release();
  }
  auto out = js.result(status, "definition",
                       status == TA_OK
                           ? wrapDefinition(s, result.definition, call.at(0))
                           : nullptr);
  js.set(out, "error",
         status == TA_OK ? js.null() : writeDiagnostic(js, result.error));
  return out;
}
struct Inspection {
  System *system;
  napi_value callback;
  ta_status_t error = TA_OK;
};
ta_visit_t visit(void *user, const ta_inspect_record_t *record) {
  auto &input = *static_cast<Inspection *>(user);
  auto &s = *input.system;
  napi_handle_scope scope = nullptr;
  ta_visit_t next = TA_VISIT_STOP;
  try {
    s.js.check(napi_open_handle_scope(s.js.env, &scope));
    auto out = s.js.object();
    s.js.set(out, "kind", s.js.number(record->kind));
    s.js.set(out, "actor", s.js.bigint(record->actor.id));
    s.js.set(out, "revision",
             s.js.bigint(ta_definition_revision(record->definition)));
    switch (record->kind) {
    case TA_INSPECT_ACTOR:
      s.js.set(out, "info", writeActorInfo(s.js, record->as.info));
      break;
    case TA_INSPECT_CURRENT_EVENT:
      s.js.set(out, "event", writeEvent(s.js, record->as.event));
      break;
    case TA_INSPECT_ACTIVE_STATE: {
      auto state = s.js.object();
      s.js.set(state, "id", s.js.number(record->as.active.id));
      s.js.set(state, "name", s.js.text(record->as.active.description->name));
      s.js.set(state, "kind", s.js.number(record->as.active.description->kind));
      s.js.set(out, "active", state);
      break;
    }
    case TA_INSPECT_HISTORY_STATE: {
      auto history = s.js.object();
      s.js.set(history, "id", s.js.number(record->as.history.id));
      s.js.set(history, "recorded", s.js.boolean(record->as.history.recorded));
      s.js.set(history, "target", s.js.number(record->as.history.target));
      s.js.set(history, "ordinal", s.js.number(record->as.history.ordinal));
      s.js.set(out, "history", history);
      break;
    }
    }
    napi_value result;
    input.error = s.invoke(input.callback, {out}, &result);
    if (input.error == TA_OK) {
      auto n = s.js.u32(result);
      if (n > TA_VISIT_STOP)
        throw std::range_error("Invalid inspection visit result");
      next = static_cast<ta_visit_t>(n);
    }
  } catch (const std::exception &e) {
    s.capture(e);
    input.error = TA_EXECUTION_ERROR;
  }
  if (scope)
    napi_close_handle_scope(s.js.env, scope);
  return next;
}
napi_value inspect(Call &call, const std::shared_ptr<System> &s) {
  Env js = call.js;
  auto options = call.at(1);
  std::shared_ptr<Cursor> cursor;
  if (js.absent(call.at(2))) {
    cursor = std::make_shared<Cursor>(js);
    cursor->owner = s;
  } else {
    cursor =
        std::static_pointer_cast<Cursor>(unwrap(js, call.at(2), Kind::Cursor));
    if (cursor->owner.lock() != s)
      throw std::invalid_argument(
          "Inspection cursor belongs to another system");
  }
  Inspection input{s.get(), js.get(options, "visit")};
  if (!js.isFunction(input.callback))
    throw std::invalid_argument("Inspection requires options.visit(record)");
  ta_inspect_options_t native{};
  native.actor.id = js.u64(js.get(options, "actor"));
  native.revision = js.u64(js.get(options, "revision"));
  native.stable_only = js.truth(js.get(options, "stable_only"));
  native.details = js.u32(js.get(options, "details"));
  native.work_units = js.size(js.get(options, "work_units"));
  native.sink = {&input, visit};
  auto status = ta_system_inspect(s->system, &native, &cursor->cursor);
  if (input.error != TA_OK)
    status = input.error;
  return js.result(status, "cursor",
                   js.absent(call.at(2)) ? wrap(cursor, call.at(0))
                                         : call.at(2));
}
struct Migration {
  System *system;
  napi_value callback;
};
ta_status_t migrate(void *user, const ta_migration_t *input,
                    ta_value_t *state) {
  auto &m = *static_cast<Migration *>(user);
  auto &s = *m.system;
  napi_handle_scope scope = nullptr;
  try {
    s.js.check(napi_open_handle_scope(s.js.env, &scope));
    auto out = s.js.object();
    s.js.set(out, "actor", s.js.bigint(input->actor.id));
    s.js.set(
        out, "from",
        writeDescription(s.js, *ta_definition_description(input->from), &s));
    s.js.set(out, "to",
             writeDescription(s.js, *ta_definition_description(input->to), &s));
    s.js.set(out, "old_state", writeData(s.js, input->old_state));
    auto bytes = s.js.buffer({input->new_bytes.data, input->new_bytes.size});
    s.js.set(out, "new_bytes", bytes);
    s.js.set(out, "system", writeSystemValues(s.js, *input->system));
    auto old = s.js.array(input->configuration_count),
         current = s.js.array(input->configuration_count);
    for (size_t i = 0; i < input->configuration_count; ++i) {
      s.js.element(old, i, s.js.number(input->old_configuration[i]));
      s.js.element(current, i, s.js.number(input->new_configuration[i]));
    }
    s.js.set(out, "old_configuration", old);
    s.js.set(out, "new_configuration", current);
    auto mapping = s.js.array(input->state_map_count);
    for (size_t i = 0; i < input->state_map_count; ++i) {
      auto item = s.js.object();
      s.js.set(item, "from", s.js.number(input->state_map[i].from));
      s.js.set(item, "to", s.js.number(input->state_map[i].to));
      s.js.set(item, "data_initialized",
               s.js.boolean(input->state_map[i].data_initialized));
      s.js.element(mapping, i, item);
    }
    s.js.set(out, "state_map", mapping);
    napi_value result;
    auto status = s.invoke(m.callback, {out}, &result);
    if (status == TA_OK) {
      if (ta_definition_description(input->to)->data_layout.storage ==
          TA_DATA_NATIVE)
        *state = root(s, result);
      else {
        auto b = s.js.bytes(bytes);
        if (b.size != input->new_bytes.size)
          throw std::invalid_argument("Migration detached byte buffer");
        if (b.size)
          std::memcpy(input->new_bytes.data, b.data, b.size);
      }
    }
    if (scope)
      napi_close_handle_scope(s.js.env, scope);
    return status;
  } catch (const std::exception &e) {
    s.capture(e);
    if (scope)
      napi_close_handle_scope(s.js.env, scope);
    return TA_EXECUTION_ERROR;
  }
}
napi_value definitionCall(Call &call) {
  auto d = definition(call.js, call.at(0));
  Guard guard(*d->owner);
  Env js = call.js;
  switch (call.op) {
  case Op::DefRetain:
    ta_definition_retain(d->definition);
    ++d->references;
    return js.undefined();
  case Op::DefRelease:
    ta_definition_release(d->definition);
    if (!--d->references) {
      d->definition = nullptr;
      d->closed = true;
    }
    return js.undefined();
  case Op::DefRevision:
    return js.bigint(ta_definition_revision(d->definition));
  case Op::DefDescription:
    return writeDescription(js, *ta_definition_description(d->definition),
                            d->owner.get());
  case Op::DefFootprint: {
    ta_actor_footprint_t footprint{};
    ta_definition_footprint(d->definition, &footprint);
    auto out = js.object();
    js.set(out, "control_bytes", js.number(footprint.control_bytes));
    js.set(out, "state_bytes", js.number(footprint.state_bytes));
    js.set(out, "minimum_chunk_bytes",
           js.number(footprint.minimum_chunk_bytes));
    return out;
  }
  default:
    throw std::logic_error("Invalid definition operation");
  }
}
napi_value contextCall(Call &call) {
  Env js = call.js;
  auto c = context(js, call.at(0));
  Parse p(js, c->owner);
  switch (call.op) {
  case Op::ContextInspect:
    return writeContext(*c->owner, c->context);
  case Op::ContextIn:
    return js.boolean(ta_context_in(c->context, p.text(call.at(1))));
  case Op::ContextUser:
    return js.get(c->owner->configuration(), "host_user");
  case Op::ContextBytes: {
    ta_mut_bytes_t bytes{};
    auto status = ta_context_bytes(c->context, &bytes);
    return js.result(status, "bytes",
                     status == TA_OK ? js.buffer({bytes.data, bytes.size})
                                     : nullptr);
  }
  case Op::ContextWrite: {
    auto input = p.bytes(call.at(1));
    ta_mut_bytes_t bytes{};
    auto status = ta_context_bytes(c->context, &bytes);
    if (status == TA_OK) {
      if (input.size != bytes.size)
        throw std::range_error("Context byte write requires exact state size");
      if (bytes.size)
        std::memcpy(bytes.data, input.data, bytes.size);
    }
    return js.number(status);
  }
  case Op::ContextArena: {
    ta_arena_t *out;
    auto status = ta_context_arena(
        c->context, static_cast<ta_lifetime_t>(js.u32(call.at(1))), &out);
    return js.result(status, "arena",
                     status == TA_OK ? wrap(borrowArena(*c->owner, out))
                                     : nullptr);
  }
  case Op::ContextRaise: {
    auto event = p.text(call.at(1));
    auto payload = readPayload(p, call.at(2));
    return js.number(ta_context_raise(c->context, event, &payload));
  }
  case Op::ContextSend: {
    auto send = readSend(p, call.at(1));
    uint64_t id = 0;
    auto status = ta_context_send(c->context, &send, &id);
    return js.result(status, "message_id", js.bigint(id));
  }
  case Op::ContextCancel:
    return js.number(ta_context_cancel(c->context, p.text(call.at(1))));
  default:
    throw std::logic_error("Invalid context operation");
  }
}
napi_value systemCall(Call &call) {
  Env js = call.js;
  auto s = system(js, call.at(0));
  Guard guard(*s);
  auto options = call.at(1);
  if (call.op == Op::DefBuild || call.op == Op::DefPrepare)
    return prepare(call, s);
  if (call.op == Op::SystemInspect)
    return inspect(call, s);
  Parse p(js, s.get());
  switch (call.op) {
  case Op::SystemLimits: {
    auto limits = readSystemLimits(p, options, &s->config.limits);
    auto status = ta_system_set_limits(s->system, &limits);
    if (status == TA_OK)
      s->config.limits = limits;
    return js.number(status);
  }
  case Op::SystemRun: {
    auto budget = js.get(options, "budget");
    ta_run_options_t input{js.u64(js.get(options, "now_ns")),
                           {js.size(js.get(budget, "work_units")),
                            js.size(js.get(budget, "actor_quantum"))}};
    ta_run_result_t result{};
    auto status = ta_system_run(s->system, &input, &result);
    if (status == TA_OK)
      s->last_now = input.now_ns;
    auto out = js.object();
    js.set(out, "work_units", js.number(result.work_units));
    js.set(out, "microsteps", js.number(result.microsteps));
    js.set(out, "external_events", js.number(result.external_events));
    js.set(out, "terminal_actors", js.number(result.terminal_actors));
    js.set(out, "runnable", js.boolean(result.runnable));
    js.set(out, "io_ready", js.boolean(result.io_ready));
    js.set(out, "next_deadline_ns", js.bigint(result.next_deadline_ns));
    return js.result(status, "result", out);
  }
  case Op::SystemDeadline:
    return js.bigint(ta_system_next_deadline(s->system));
  case Op::SystemStats: {
    ta_system_stats_t stats{};
    ta_system_stats(s->system, &stats);
    return writeSystemStats(js, stats);
  }
  case Op::SystemShutdown:
    return js.number(ta_system_shutdown(s->system));
  case Op::SystemDestroy: {
    auto status = ta_system_destroy(s->system);
    if (status == TA_OK) {
      s->system = nullptr;
      s->closed = true;
      s->parent.reset();
    }
    return js.number(status);
  }
  case Op::DefPublish:
  case Op::DefUnpublish: {
    ta_definition_binding_t binding{
        p.text(js.get(options, "key")),
        js.u64(js.get(options, "expected_revision"))};
    if (call.op == Op::DefUnpublish)
      return js.number(ta_definition_unpublish(s->system, &binding));
    auto d = ownedDefinition(*s, call.at(2));
    return js.number(ta_definition_publish(s->system, &binding, d->definition));
  }
  case Op::DefLookup: {
    ta_definition_t *out = nullptr;
    auto status = ta_definition_lookup(s->system, p.text(options), &out);
    return js.result(status, "definition",
                     status == TA_OK ? wrapDefinition(s, out, call.at(0))
                                     : nullptr);
  }
  case Op::ActorSpawn: {
    ta_spawn_options_t input{};
    auto def = js.get(options, "definition");
    if (!js.absent(def))
      input.definition = ownedDefinition(*s, def)->definition;
    input.definition_key = p.text(js.get(options, "definition_key"));
    if (js.has(options, "seed"))
      input.seed = p.native(js.get(options, "seed"));
    input.initial_bytes = p.bytes(js.get(options, "initial_bytes"));
    input.name = p.text(js.get(options, "name"));
    input.trace = readTrace(p, js.get(options, "trace"));
    auto params = js.get(options, "parameters");
    input.parameter_count = js.length(params);
    auto parameters = p.items<ta_parameter_t>(input.parameter_count);
    input.parameters = parameters;
    for (size_t i = 0; i < input.parameter_count; ++i) {
      auto item = js.element(params, i);
      parameters[i] = {p.text(js.get(item, "name")),
                       p.native(js.get(item, "value"))};
    }
    ta_actor_limits_t limits{};
    if (!js.absent(js.get(options, "limits"))) {
      limits = readActorLimits(p, js.get(options, "limits"),
                               &s->config.actor_defaults);
      input.limits = &limits;
    }
    ta_actor_ref_t actor{};
    auto status = ta_actor_spawn(s->system, &input, &actor);
    return js.result(status, "actor", js.bigint(actor.id));
  }
  case Op::ActorInspect: {
    ta_actor_info_t info{};
    auto status = ta_actor_inspect(s->system, {js.u64(options)}, &info);
    return js.result(status, "info",
                     status == TA_OK ? writeActorInfo(js, info) : nullptr);
  }
  case Op::ActorLimits: {
    auto limits = readActorLimits(p, call.at(2), &s->config.actor_defaults);
    return js.number(
        ta_actor_set_limits(s->system, {js.u64(options)}, &limits));
  }
  case Op::ActorStop:
    return js.number(ta_actor_stop(s->system, {js.u64(options)}));
  case Op::ActorRelease:
    return js.number(ta_actor_release(s->system, {js.u64(options)}));
  case Op::ActorUpgrade: {
    auto d = ownedDefinition(*s, js.get(options, "replacement"));
    Migration migration{s.get(), js.get(options, "migrate")};
    ta_upgrade_options_t native{
        {js.u64(js.get(options, "actor"))},
        d->definition,
        js.u64(js.get(options, "expected_revision"), TA_REVISION_ANY),
        &migration,
        nullptr};
    if (!js.absent(migration.callback)) {
      if (!js.isFunction(migration.callback))
        throw std::invalid_argument("migrate must be a function");
      native.migrate = migrate;
    }
    return js.number(ta_actor_upgrade(s->system, &native));
  }
  case Op::NameBind:
  case Op::NameUnbind: {
    ta_name_binding_t binding{p.text(js.get(options, "name")),
                              {js.u64(js.get(options, "expected"))}};
    return js.number(
        call.op == Op::NameBind
            ? ta_name_bind(s->system, &binding, {js.u64(call.at(2))})
            : ta_name_unbind(s->system, &binding));
  }
  case Op::NameLookup:
  case Op::SessionLookup: {
    ta_actor_ref_t actor{};
    auto text = p.text(options);
    auto status = call.op == Op::NameLookup
                      ? ta_name_lookup(s->system, text, &actor)
                      : ta_session_lookup(s->system, text, &actor);
    return js.result(status, "actor", js.bigint(actor.id));
  }
  case Op::Post: {
    ta_post_options_t input{readTarget(p, js.get(options, "target")),
                            message(p, js.get(options, "message"))};
    uint64_t id = 0;
    auto status = ta_post(s->system, &input, &id);
    return js.result(status, "message_id", js.bigint(id));
  }
  case Op::IoNext: {
    const ta_io_request_t *request = nullptr;
    auto status = ta_io_next(s->system, &request);
    if (status == TA_OK) {
      try {
        s->leases.insert(request->request.id);
      } catch (...) {
        ta_io_complete(s->system, request->request, TA_CANCELLED);
        throw;
      }
    }
    return js.result(status, "request",
                     status == TA_OK ? writeRequest(js, *request) : nullptr);
  }
  case Op::IoComplete: {
    uint64_t id = js.u64(options);
    auto status = ta_io_complete(s->system, {id},
                                 static_cast<ta_status_t>(js.u32(call.at(2))));
    if (status == TA_OK)
      s->leases.erase(id);
    return js.number(status);
  }
  case Op::IoReceive:
  case Op::IoError: {
    ta_io_route_t route{p.text(js.get(options, "processor")),
                        {js.u64(js.get(options, "destination"))}};
    if (call.op == Op::IoError)
      return js.number(
          ta_io_receive_error(s->system, &route, p.text(call.at(2))));
    auto event = readEvent(p, call.at(2));
    return js.number(ta_io_receive(s->system, &route, &event));
  }
  case Op::InvocationEmit: {
    auto input = message(p, call.at(2));
    return js.number(ta_invocation_emit(s->system, {js.u64(options)}, &input));
  }
  case Op::InvocationDone: {
    auto input = readBody(p, call.at(2));
    return js.number(ta_invocation_done(s->system, {js.u64(options)}, &input));
  }
  case Op::InvocationFail:
    return js.number(ta_invocation_fail(s->system, {js.u64(options)}));
  case Op::InvocationCancelled: {
    uint8_t cancelled = 0;
    auto status =
        ta_invocation_is_cancelled(s->system, {js.u64(options)}, &cancelled);
    return js.result(status, "cancelled", js.boolean(cancelled));
  }
  case Op::TakeErrors: {
    auto out = js.array(s->errors.size());
    size_t i = 0;
    for (auto ref : s->errors) {
      napi_value box;
      js.check(napi_get_reference_value(js.env, ref, &box));
      js.element(out, i++, js.element(box, 0));
      napi_delete_reference(js.env, ref);
    }
    s->errors.clear();
    return out;
  }
  case Op::Dispose:
    s->dispose();
    return js.undefined();
  case Op::BindingStats: {
    auto out = js.object();
    js.set(out, "native_roots", js.number(s->roots));
    js.set(out, "taken_leases", js.number(s->leases.size()));
    js.set(out, "callback_errors", js.number(s->errors.size()));
    return out;
  }
  default:
    throw std::logic_error("Invalid system operation");
  }
}
napi_value dispatch(Call &call) {
  auto op = call.op;
  Env js = call.js;
  if (op == Op::ConfigInit)
    return defaults(js);
  if (op == Op::SystemCreate)
    return createSystem(call);
  if (op >= Op::ArenaCreate && op <= Op::ArenaLimit)
    return arenaCall(call);
  if (op >= Op::DefRetain && op <= Op::DefFootprint)
    return definitionCall(call);
  if ((op >= Op::ContextInspect && op <= Op::ContextCancel) ||
      op == Op::ContextWrite)
    return contextCall(call);
  if (op == Op::TargetParse) {
    Parse p(js);
    ta_target_t target{};
    auto status = ta_target_parse(p.text(call.at(0)), &target);
    auto out = js.object();
    js.set(out, "kind", js.number(target.kind));
    js.set(out, "instance", js.bigint(target.instance.id));
    js.set(out, "text", js.text(target.text));
    return js.result(status, "target", out);
  }
  if (op == Op::Text) {
    if (js.absent(call.at(0)))
      return js.null();
    auto text = js.utf8(call.at(0));
    return js.string(text.substr(0, text.find('\0')));
  }
  if (op == Op::MemoryRead || op == Op::MemoryWrite) {
    auto memory =
        std::static_pointer_cast<Memory>(unwrap(js, call.at(0), Kind::Memory));
    memory->owner->require();
    if (op == Op::MemoryRead)
      return js.buffer({memory->data, memory->size});
    auto input = js.bytes(call.at(1));
    if (input.size != memory->size)
      throw std::range_error("Memory write requires exact allocation size");
    if (input.size)
      std::memcpy(memory->data, input.data, input.size);
    return js.undefined();
  }
  return systemCall(call);
}
napi_value callback(napi_env env, napi_callback_info info) {
  Env js{env};
  try {
    void *data;
    Call call{js, Op::ConfigInit, {}, 3};
    js.check(
        napi_get_cb_info(env, info, &call.argc, call.args, nullptr, &data));
    call.op = static_cast<const NamedOp *>(data)->op;
    return dispatch(call);
  } catch (const std::exception &e) {
    bool pending = false;
    napi_is_exception_pending(env, &pending);
    if (!pending) {
      if (dynamic_cast<const std::range_error *>(&e))
        napi_throw_range_error(env, nullptr, e.what());
      else if (dynamic_cast<const std::invalid_argument *>(&e))
        napi_throw_type_error(env, nullptr, e.what());
      else
        napi_throw_error(env, nullptr, e.what());
    }
    return nullptr;
  }
}
} // namespace
napi_value initialize(napi_env env, napi_value exports) {
  Env js{env};
  try {
    initializeAddon(js);
    auto values = constants(js);
    napi_value keys;
    js.check(napi_get_property_names(env, values, &keys));
    for (size_t i = 0; i < js.length(keys); ++i) {
      auto key = js.utf8(js.element(keys, i));
      js.set(exports, key.c_str(), js.get(values, key.c_str()));
    }
    for (auto &op : operations) {
      napi_value fn;
      js.check(napi_create_function(env, op.name, NAPI_AUTO_LENGTH, callback,
                                    const_cast<NamedOp *>(&op), &fn));
      js.set(exports, op.name, fn);
    }
    return exports;
  } catch (const std::exception &e) {
    napi_throw_error(env, nullptr, e.what());
    return nullptr;
  }
}
} // namespace tn

NAPI_MODULE(tinyactors, tn::initialize)
