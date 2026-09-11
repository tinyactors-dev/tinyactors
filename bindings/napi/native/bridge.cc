#include "bridge.hpp"

namespace tn {
namespace {
const napi_type_tag resourceTag = {UINT64_C(0xd25f80fb1f9be159),
                                   UINT64_C(0xa445bba31d125750)};
constexpr double maxSafe = 9007199254740991.0;
napi_valuetype type(Env js, napi_value input) {
  if (!input)
    return napi_undefined;
  napi_valuetype result;
  js.check(napi_typeof(js.env, input, &result));
  return result;
}
struct Scope {
  Env js;
  napi_handle_scope scope = nullptr;
  explicit Scope(Env e) : js(e) {
    js.check(napi_open_handle_scope(js.env, &scope));
  }
  ~Scope() {
    if (scope)
      napi_close_handle_scope(js.env, scope);
  }
};
ta_status_t acquire(void *, const ta_layout_t *minimum, ta_chunk_t *out) {
  const size_t alignment = std::max(minimum->alignment, alignof(void *));
  if (!alignment || (alignment & (alignment - 1)) ||
      minimum->size > SIZE_MAX - alignment - sizeof(void *))
    return TA_NO_MEMORY;
  void *raw = std::malloc(minimum->size + alignment - 1 + sizeof(void *));
  if (!raw)
    return TA_NO_MEMORY;
  uintptr_t pointer =
      (reinterpret_cast<uintptr_t>(raw) + sizeof(void *) + alignment - 1) &
      ~(alignment - 1);
  reinterpret_cast<void **>(pointer)[-1] = raw;
  *out = {reinterpret_cast<void *>(pointer), {minimum->size, alignment}};
  return TA_OK;
}
void release(void *, const ta_chunk_t *chunk) {
  std::free(static_cast<void **>(chunk->data)[-1]);
}
struct ArenaMarker {
  Env js;
  ta_arena_t *pointer = nullptr;
  std::weak_ptr<Arena> weak;
  std::shared_ptr<Arena> borrowed;
};
void arenaExpired(void *input) {
  std::unique_ptr<ArenaMarker> marker(static_cast<ArenaMarker *>(input));
  addon(marker->js).arenas.erase(marker->pointer);
  if (auto arena = marker->weak.lock()) {
    arena->arena = nullptr;
    arena->closed = true;
  }
}
void finalizeResource(napi_env, void *data, void *) {
  delete static_cast<std::shared_ptr<Resource> *>(data);
}
void cleanup(void *input) {
  auto &state = *static_cast<Addon *>(input);
  state.closing = true;
  // Silent teardown does not register resources. Walk weak entries directly so
  // cleanup does not need a new allocation while the process is exiting.
  for (auto &weak : state.resources)
    if (auto entry = weak.lock())
      if (entry->kind == Kind::System)
        static_cast<System *>(entry.get())->closing = true;
  for (auto &weak : state.resources)
    if (auto entry = weak.lock())
      if (entry->kind == Kind::Definition)
        static_cast<Definition *>(entry.get())->close();
  for (auto &weak : state.resources)
    if (auto entry = weak.lock())
      if (entry->kind == Kind::System)
        static_cast<System *>(entry.get())->dispose();
  for (auto &weak : state.resources)
    if (auto entry = weak.lock())
      if (entry->kind == Kind::Arena)
        static_cast<Arena *>(entry.get())->close(true);
}
void finalizeAddon(napi_env, void *data, void *) {
  delete static_cast<Addon *>(data);
}
ta_visit_t firstTerminal(void *user, const ta_inspect_record_t *record) {
  if (record->as.info.status >= TA_ACTOR_DONE) {
    *static_cast<ta_actor_ref_t *>(user) = record->actor;
    return TA_VISIT_STOP;
  }
  return TA_VISIT_CONTINUE;
}
} // namespace

void Env::check(napi_status status) const {
  if (status == napi_ok)
    return;
  const napi_extended_error_info *info = nullptr;
  napi_get_last_error_info(env, &info);
  throw std::runtime_error(info && info->error_message ? info->error_message
                                                       : "Node-API failure");
}
napi_value Env::object() const {
  napi_value v;
  check(napi_create_object(env, &v));
  return v;
}
napi_value Env::array(size_t n) const {
  napi_value v;
  check(napi_create_array_with_length(env, n, &v));
  return v;
}
napi_value Env::undefined() const {
  napi_value v;
  check(napi_get_undefined(env, &v));
  return v;
}
napi_value Env::null() const {
  napi_value v;
  check(napi_get_null(env, &v));
  return v;
}
napi_value Env::boolean(bool b) const {
  napi_value v;
  check(napi_get_boolean(env, b, &v));
  return v;
}
napi_value Env::number(double n) const {
  napi_value v;
  check(napi_create_double(env, n, &v));
  return v;
}
napi_value Env::bigint(uint64_t n) const {
  napi_value v;
  check(napi_create_bigint_uint64(env, n, &v));
  return v;
}
napi_value Env::string(const std::string &s) const {
  napi_value v;
  check(napi_create_string_utf8(env, s.data(), s.size(), &v));
  return v;
}
napi_value Env::text(ta_text_t s) const {
  return s.data ? string(std::string(s.data, s.size)) : null();
}
napi_value Env::buffer(ta_bytes_t b) const {
  napi_value v;
  check(napi_create_buffer_copy(env, b.size, b.data, nullptr, &v));
  return v;
}
napi_value Env::get(napi_value o, const char *key) const {
  if (absent(o))
    return undefined();
  if (!isObject(o) && !isFunction(o))
    throw std::invalid_argument("Expected a struct object");
  napi_value v;
  check(napi_get_named_property(env, o, key, &v));
  return v;
}
void Env::set(napi_value o, const char *key, napi_value v) const {
  check(napi_set_named_property(env, o, key, v));
}
napi_value Env::element(napi_value a, size_t i) const {
  if (i > UINT32_MAX)
    throw std::range_error("Array index exceeds uint32");
  napi_value v;
  check(napi_get_element(env, a, static_cast<uint32_t>(i), &v));
  return v;
}
void Env::element(napi_value a, size_t i, napi_value v) const {
  if (i > UINT32_MAX)
    throw std::range_error("Array index exceeds uint32");
  check(napi_set_element(env, a, static_cast<uint32_t>(i), v));
}
bool Env::has(napi_value o, const char *key) const {
  bool result = false;
  if (absent(o))
    return false;
  check(napi_has_named_property(env, o, key, &result));
  return result;
}
bool Env::absent(napi_value v) const {
  auto t = type(*this, v);
  return t == napi_undefined || t == napi_null;
}
bool Env::isFunction(napi_value v) const {
  return type(*this, v) == napi_function;
}
bool Env::isObject(napi_value v) const {
  return type(*this, v) == napi_object && !absent(v);
}
bool Env::isArray(napi_value v) const {
  bool result = false;
  if (!v)
    return false;
  check(napi_is_array(env, v, &result));
  return result;
}
bool Env::truth(napi_value v) const {
  napi_value b;
  bool result;
  check(napi_coerce_to_bool(env, v, &b));
  check(napi_get_value_bool(env, b, &result));
  return result;
}
uint64_t Env::u64(napi_value v, uint64_t fallback) const {
  if (absent(v))
    return fallback;
  if (type(*this, v) == napi_bigint) {
    uint64_t n;
    bool lossless;
    check(napi_get_value_bigint_uint64(env, v, &n, &lossless));
    if (!lossless)
      throw std::range_error("Expected uint64 BigInt");
    return n;
  }
  if (type(*this, v) != napi_number)
    throw std::invalid_argument(
        "Expected a nonnegative safe integer or BigInt");
  double n;
  check(napi_get_value_double(env, v, &n));
  if (!std::isfinite(n) || n < 0 || n > maxSafe || std::floor(n) != n)
    throw std::range_error(
        "Use BigInt for integers beyond Number.MAX_SAFE_INTEGER");
  return static_cast<uint64_t>(n);
}
size_t Env::size(napi_value v, size_t fallback) const {
  if (absent(v))
    return fallback;
  if (type(*this, v) == napi_number) {
    double n;
    check(napi_get_value_double(env, v, &n));
    if (n == INFINITY)
      return SIZE_MAX;
  }
  uint64_t n = u64(v);
  if (n > SIZE_MAX)
    throw std::range_error("Size exceeds host size_t");
  return static_cast<size_t>(n);
}
uint32_t Env::u32(napi_value v, uint32_t fallback) const {
  uint64_t n = u64(v, fallback);
  if (n > UINT32_MAX)
    throw std::range_error("Integer exceeds uint32");
  return static_cast<uint32_t>(n);
}
std::string Env::utf8(napi_value v) const {
  if (type(*this, v) != napi_string)
    throw std::invalid_argument("Expected a string");
  size_t n;
  check(napi_get_value_string_utf8(env, v, nullptr, 0, &n));
  std::string result(n, '\0');
  check(napi_get_value_string_utf8(env, v, result.data(), n + 1, &n));
  return result;
}
size_t Env::length(napi_value v) const {
  if (absent(v))
    return 0;
  if (!isArray(v))
    throw std::invalid_argument("Expected an array");
  uint32_t n;
  check(napi_get_array_length(env, v, &n));
  return n;
}
ta_bytes_t Env::bytes(napi_value v) const {
  if (absent(v))
    return {};
  bool typed = false;
  check(napi_is_typedarray(env, v, &typed));
  if (typed) {
    napi_typedarray_type t;
    size_t n, offset;
    void *data;
    napi_value backing;
    check(napi_get_typedarray_info(env, v, &t, &n, &data, &backing, &offset));
    if (t != napi_uint8_array && t != napi_uint8_clamped_array)
      throw std::invalid_argument("Expected Buffer or Uint8Array");
    bool detached = false;
    check(napi_is_detached_arraybuffer(env, backing, &detached));
    if (detached)
      throw std::invalid_argument("Detached byte buffer");
    return {data, n};
  }
  throw std::invalid_argument("Expected Buffer or Uint8Array");
}
napi_value Env::call(napi_value fn, napi_value receiver,
                     const std::vector<napi_value> &args) const {
  napi_value result;
  check(
      napi_call_function(env, receiver, fn, args.size(), args.data(), &result));
  return result;
}
napi_value Env::result(ta_status_t status, const char *key,
                       napi_value v) const {
  napi_value out = object();
  set(out, "status", number(status));
  if (key)
    set(out, key, v ? v : null());
  return out;
}

Addon &addon(Env js) {
  void *data;
  js.check(napi_get_instance_data(js.env, &data));
  return *static_cast<Addon *>(data);
}
void initializeAddon(Env js) {
  auto *state = new Addon{js, false, {}, {}};
  js.check(napi_set_instance_data(js.env, state, finalizeAddon, nullptr));
  js.check(napi_add_env_cleanup_hook(js.env, cleanup, state));
}
napi_value wrap(const std::shared_ptr<Resource> &resource, napi_value parent) {
  Env js = resource->js;
  auto out = js.object();
  auto holder = std::make_unique<std::shared_ptr<Resource>>(resource);
  js.check(
      napi_wrap(js.env, out, holder.get(), finalizeResource, nullptr, nullptr));
  holder.release();
  js.check(napi_type_tag_object(js.env, out, &resourceTag));
  if (parent) {
    napi_property_descriptor edge = {"owner", nullptr, nullptr,      nullptr,
                                     nullptr, parent,  napi_default, nullptr};
    js.check(napi_define_properties(js.env, out, 1, &edge));
  }
  auto &resources = addon(js).resources;
  if (resources.size() > 1024 && (resources.size() & 1023) == 0)
    resources.erase(std::remove_if(resources.begin(), resources.end(),
                                   [](auto &r) { return r.expired(); }),
                    resources.end());
  resources.push_back(resource);
  return out;
}
std::shared_ptr<Resource> unwrap(Env js, napi_value input, Kind kind) {
  bool valid = false;
  if (js.isObject(input))
    js.check(napi_check_object_type_tag(js.env, input, &resourceTag, &valid));
  if (!valid)
    throw std::invalid_argument("Expected a tinyactors opaque handle");
  void *data;
  js.check(napi_unwrap(js.env, input, &data));
  auto resource = *static_cast<std::shared_ptr<Resource> *>(data);
  if (resource->js.env != js.env || resource->kind != kind)
    throw std::invalid_argument("Wrong tinyactors handle kind or environment");
  if (resource->closed)
    throw std::invalid_argument("Tinyactors handle is closed or expired");
  return resource;
}
std::shared_ptr<System> system(Env js, napi_value input) {
  auto s = std::static_pointer_cast<System>(unwrap(js, input, Kind::System));
  s->require();
  return s;
}
std::shared_ptr<Definition> definition(Env js, napi_value input) {
  auto d =
      std::static_pointer_cast<Definition>(unwrap(js, input, Kind::Definition));
  d->require();
  return d;
}
std::shared_ptr<Arena> arena(Env js, napi_value input) {
  auto a = std::static_pointer_cast<Arena>(unwrap(js, input, Kind::Arena));
  a->require();
  return a;
}
std::shared_ptr<Context> context(Env js, napi_value input) {
  auto c = std::static_pointer_cast<Context>(unwrap(js, input, Kind::Context));
  c->require();
  return c;
}
ta_chunk_allocator_t allocator(Env) { return {nullptr, acquire, release}; }
void watchArena(const std::shared_ptr<Arena> &a) {
  auto marker = std::make_unique<ArenaMarker>();
  marker->js = a->js;
  marker->pointer = a->arena;
  marker->weak = a;
  if (a->borrowed)
    marker->borrowed = a;
  auto status = ta_arena_defer(a->arena, arenaExpired, marker.get());
  if (status != TA_OK)
    throw std::bad_alloc();
  marker.release();
  addon(a->js).arenas[a->arena] = a;
}
std::shared_ptr<Arena> createArena(Env js, const ta_arena_options_t &options) {
  auto a = std::make_shared<Arena>(js);
  auto alloc = allocator(js);
  if (ta_arena_create(&alloc, &options, &a->arena) != TA_OK)
    throw std::bad_alloc();
  watchArena(a);
  return a;
}
std::shared_ptr<Arena> borrowArena(System &s, ta_arena_t *ptr) {
  auto &cache = addon(s.js).arenas;
  auto found = cache.find(ptr);
  if (found != cache.end())
    if (auto a = found->second.lock())
      return a;
  auto a = std::make_shared<Arena>(s.js);
  a->borrowed = true;
  a->owner = s.shared_from_this();
  a->arena = ptr;
  watchArena(a);
  return a;
}
Arena::~Arena() { close(true); }
void Arena::require() const {
  if (closed || !arena)
    throw std::invalid_argument("Arena lifetime has ended");
}
void Arena::close(bool silent) {
  if (!arena || closed || borrowed)
    return;
  suppress = silent;
  ta_arena_close(arena);
  arena = nullptr;
  closed = true;
}
Definition::~Definition() {
  if (owner)
    ++owner->silent;
  close();
  if (owner)
    --owner->silent;
}
void Definition::require() const {
  if (closed || !definition || !owner || owner->closed)
    throw std::invalid_argument("Definition is released");
}
void Definition::close() {
  if (!definition || closed)
    return;
  while (references) {
    --references;
    ta_definition_release(definition);
  }
  definition = nullptr;
  closed = true;
}
napi_value wrapDefinition(const std::shared_ptr<System> &s,
                          ta_definition_t *ptr, napi_value parent) {
  if (!ptr)
    return s->js.null();
  std::shared_ptr<Definition> d;
  try {
    d = std::make_shared<Definition>(s->js);
  } catch (...) {
    ta_definition_release(ptr);
    throw;
  }
  d->owner = s;
  d->definition = ptr;
  if (s->definitions.size() >= 64 && (s->definitions.size() & 63) == 0)
    s->definitions.erase(std::remove_if(s->definitions.begin(),
                                        s->definitions.end(),
                                        [](auto &weak) {
                                          auto value = weak.lock();
                                          return !value || value->closed;
                                        }),
                         s->definitions.end());
  s->definitions.push_back(d);
  return wrap(d, parent);
}
void Context::require() const {
  if (closed || !context || !owner || owner->closed || owner->closing)
    throw std::invalid_argument("Execution context is no longer active");
}
Guard::Guard(System &s) : system(s) {
  s.require();
  if (s.entered)
    throw std::invalid_argument("Reentrant calls on the same actor system are "
                                "forbidden; use the callback context");
  s.entered = true;
}
Guard::~Guard() { system.entered = false; }

ta_value_t root(System &s, napi_value input) {
  auto r = std::make_unique<Root>();
  r->js = s.js;
  r->owner = &s;
  napi_value box = s.js.array(1);
  s.js.element(box, 0, input);
  s.js.check(napi_create_reference(s.js.env, box, 1, &r->reference));
  ++s.roots;
  return {reinterpret_cast<uintptr_t>(r.release())};
}
napi_value value(ta_value_t v, Env js) {
  if (!v.handle)
    return js.undefined();
  auto *r = reinterpret_cast<Root *>(v.handle);
  if (r->js.env != js.env)
    throw std::invalid_argument(
        "Native value belongs to another JS environment");
  napi_value box;
  js.check(napi_get_reference_value(js.env, r->reference, &box));
  return js.element(box, 0);
}
void retainRoot(void *, ta_value_t v) {
  if (v.handle)
    ++reinterpret_cast<Root *>(v.handle)->references;
}
void releaseRoot(void *, ta_value_t v) {
  if (!v.handle)
    return;
  auto *r = reinterpret_cast<Root *>(v.handle);
  if (--r->references)
    return;
  napi_delete_reference(r->js.env, r->reference);
  if (r->owner)
    --r->owner->roots;
  delete r;
}
void System::require() const {
  if (closed || closing || !system)
    throw std::invalid_argument("Actor system is closed");
}
napi_value System::configuration() const {
  napi_value out = nullptr;
  if (callbacks)
    js.check(napi_get_reference_value(js.env, callbacks, &out));
  return out ? out : js.undefined();
}
napi_value System::callback(const char *group, const char *name) const {
  auto cfg = configuration();
  return js.get(group ? js.get(cfg, group) : cfg, name);
}
void System::capture() {
  bool pending = false;
  napi_is_exception_pending(js.env, &pending);
  if (!pending)
    return;
  napi_value error;
  napi_get_and_clear_last_exception(js.env, &error);
  napi_value box;
  if (napi_create_array_with_length(js.env, 1, &box) != napi_ok)
    return;
  if (napi_set_element(js.env, box, 0, error) != napi_ok)
    return;
  napi_ref ref;
  if (napi_create_reference(js.env, box, 1, &ref) == napi_ok) {
    if (errors.size() < 1024) {
      try {
        errors.push_back(ref);
      } catch (...) {
        napi_delete_reference(js.env, ref);
      }
    } else
      napi_delete_reference(js.env, ref);
  }
}
void System::capture(const std::exception &error) {
  bool pending = false;
  napi_is_exception_pending(js.env, &pending);
  if (!pending)
    napi_throw_error(js.env, nullptr, error.what());
  capture();
}
ta_status_t System::invoke(napi_value fn, const std::vector<napi_value> &args,
                           napi_value *out) {
  if (closing || silent || addon(js).closing)
    return TA_CANCELLED;
  try {
    if (!js.isFunction(fn))
      return TA_UNSUPPORTED;
    auto result = js.call(fn, js.undefined(), args);
    bool promise = false;
    js.check(napi_is_promise(js.env, result, &promise));
    if (promise)
      throw std::invalid_argument("Actor callbacks must be synchronous; use "
                                  "sends or invokes for async work");
    if (out)
      *out = result;
    return TA_OK;
  } catch (const std::exception &e) {
    capture(e);
    return TA_EXECUTION_ERROR;
  }
}
System::~System() {
  dispose();
  if (callbacks)
    napi_delete_reference(js.env, callbacks);
  if (clone)
    napi_delete_reference(js.env, clone);
  for (auto ref : errors)
    napi_delete_reference(js.env, ref);
}
void System::dispose() {
  if (closed || !system)
    return;
  closing = true;
  for (auto &weak : definitions)
    if (auto d = weak.lock())
      d->close();
  ta_system_shutdown(system);
  ta_run_options_t run{0, {SIZE_MAX, 1024}};
  /* The current time is not public. A zero-work probe rejects backwards time;
     use the last successfully supplied time, tracked by the binding. */
  run.now_ns = last_now;
  for (;;) {
    ta_run_result_t result{};
    ta_system_run(system, &run, &result);
    for (auto id : leases)
      ta_io_complete(system, {id}, TA_CANCELLED);
    leases.clear();
    const ta_io_request_t *request = nullptr;
    while (ta_io_next(system, &request) == TA_OK)
      ta_io_complete(system, request->request, TA_CANCELLED);
    ta_actor_ref_t actor{};
    ta_inspect_cursor_t cursor{};
    ta_inspect_options_t inspect{};
    inspect.work_units = SIZE_MAX;
    inspect.sink = {&actor, firstTerminal};
    ta_system_inspect(system, &inspect, &cursor);
    if (actor.id) {
      ta_actor_release(system, actor);
      continue;
    }
    if (!result.runnable)
      break;
  }
  if (ta_system_destroy(system) == TA_OK) {
    system = nullptr;
    closed = true;
    parent.reset();
  }
}

Parse::Parse(Env e, System *s) : js(e), owner(s) {
  if (owner) {
    auto config = owner->configuration();
    auto limits = js.get(config, "marshal_limits");
    max_nodes = js.size(js.get(limits, "nodes"), max_nodes);
    max_depth = js.size(js.get(limits, "depth"), max_depth);
    max_bytes = js.size(js.get(limits, "bytes"), max_bytes);
  }
}
Parse::~Parse() {
  for (auto r : roots)
    releaseRoot(nullptr, r);
  if (storage)
    ta_arena_close(storage);
}
void *Parse::alloc(size_t count, size_t size, size_t alignment) {
  if (count == 0)
    return nullptr;
  if (size && count > SIZE_MAX / size)
    throw std::range_error("Native descriptor size overflow");
  if (count > max_nodes || nodes > max_nodes - count)
    throw std::range_error("Descriptor marshal node limit exceeded");
  if (!storage) {
    ta_arena_options_t options{4096, SIZE_MAX, max_bytes, SIZE_MAX};
    auto host = allocator(js);
    if (ta_arena_create(&host, &options, &storage) != TA_OK)
      throw std::bad_alloc();
  }
  nodes += count;
  ta_layout_t layout{count * size, alignment};
  void *out;
  if (ta_arena_alloc(storage, &layout, &out) != TA_OK)
    throw std::bad_alloc();
  std::memset(out, 0, layout.size);
  return out;
}
ta_text_t Parse::text(napi_value input) {
  if (js.absent(input))
    return {};
  auto s = js.utf8(input);
  if (s.find('\0') != std::string::npos)
    throw std::invalid_argument("Text cannot contain NUL");
  auto data = static_cast<char *>(alloc(1, s.size() + 1, 1));
  std::memcpy(data, s.data(), s.size());
  return {data, s.size()};
}
const char *Parse::cstr(napi_value input) { return text(input).data; }
ta_bytes_t Parse::bytes(napi_value input) {
  auto b = js.bytes(input);
  if (!b.size)
    return {};
  auto data = alloc(1, b.size, 1);
  std::memcpy(data, b.data, b.size);
  return {data, b.size};
}
ta_value_t Parse::native(napi_value input) {
  if (!owner)
    throw std::invalid_argument("Native value requires an actor system");
  auto r = root(*owner, input);
  try {
    roots.push_back(r);
  } catch (...) {
    releaseRoot(nullptr, r);
    throw;
  }
  return r;
}
void Parse::enter() {
  if (depth >= max_depth)
    throw std::range_error(
        "Descriptor marshal depth limit exceeded (possible cycle)");
  ++depth;
}
void Parse::leave() { --depth; }
CallbackContext::CallbackContext(ta_context_t *c) {
  auto *s = static_cast<System *>(ta_context_host_user(c));
  s->js.check(napi_open_handle_scope(s->js.env, &scope));
  try {
    handle = std::make_shared<Context>(s->js);
    handle->context = c;
    handle->owner = s;
    object = wrap(handle);
  } catch (...) {
    napi_close_handle_scope(s->js.env, scope);
    scope = nullptr;
    throw;
  }
}
CallbackContext::~CallbackContext() {
  handle->context = nullptr;
  handle->closed = true;
  if (scope)
    napi_close_handle_scope(handle->js.env, scope);
}
} // namespace tn
