#ifndef TINYACTORS_NAPI_BRIDGE_HPP
#define TINYACTORS_NAPI_BRIDGE_HPP

#include "../../../src/tinyactors.h"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <node_api.h>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace tn {
struct Env {
  napi_env env;
  void check(napi_status status) const;
  napi_value object() const;
  napi_value array(size_t count = 0) const;
  napi_value undefined() const;
  napi_value null() const;
  napi_value boolean(bool value) const;
  napi_value number(double value) const;
  napi_value bigint(uint64_t value) const;
  napi_value string(const std::string &value) const;
  napi_value text(ta_text_t value) const;
  napi_value buffer(ta_bytes_t bytes) const;
  napi_value get(napi_value object, const char *key) const;
  void set(napi_value object, const char *key, napi_value value) const;
  napi_value element(napi_value array, size_t index) const;
  void element(napi_value array, size_t index, napi_value value) const;
  bool has(napi_value object, const char *key) const;
  bool absent(napi_value value) const;
  bool isFunction(napi_value value) const;
  bool isArray(napi_value value) const;
  bool isObject(napi_value value) const;
  bool truth(napi_value value) const;
  uint64_t u64(napi_value value, uint64_t fallback = 0) const;
  size_t size(napi_value value, size_t fallback = 0) const;
  uint32_t u32(napi_value value, uint32_t fallback = 0) const;
  std::string utf8(napi_value value) const;
  size_t length(napi_value array) const;
  ta_bytes_t bytes(napi_value value) const;
  napi_value call(napi_value fn, napi_value receiver,
                  const std::vector<napi_value> &args) const;
  napi_value result(ta_status_t status, const char *key = nullptr,
                    napi_value value = nullptr) const;
};

enum class Kind { Arena, System, Definition, Context, Memory, Cursor };
struct Resource {
  Kind kind;
  Env js;
  bool closed = false;
  Resource(Kind k, Env e) : kind(k), js(e) {}
  virtual ~Resource() = default;
};
struct System;
struct Context;
struct Arena : Resource {
  ta_arena_t *arena = nullptr;
  std::shared_ptr<Arena> parent;
  std::weak_ptr<System> owner;
  bool borrowed = false, suppress = false;
  Arena(Env e) : Resource(Kind::Arena, e) {}
  ~Arena();
  void close(bool silent = false);
  void require() const;
};
struct Definition : Resource {
  ta_definition_t *definition = nullptr;
  std::shared_ptr<System> owner;
  size_t references = 1;
  Definition(Env e) : Resource(Kind::Definition, e) {}
  ~Definition();
  void close();
  void require() const;
};
struct Root {
  Env js;
  napi_ref reference = nullptr;
  size_t references = 1;
  System *owner = nullptr;
};
struct System : Resource, std::enable_shared_from_this<System> {
  ta_system_t *system = nullptr;
  ta_system_config_t config{};
  std::shared_ptr<Arena> parent;
  napi_ref callbacks =
      nullptr; /* Weak; JS handle owns config through a JS edge. */
  napi_ref clone = nullptr;
  std::vector<napi_ref> errors;
  std::vector<std::weak_ptr<Definition>> definitions;
  std::unordered_set<uint64_t> leases;
  std::vector<std::shared_ptr<void>> registrations;
  bool entered = false, closing = false;
  size_t roots = 0;
  size_t silent = 0;
  uint64_t last_now = 0;
  System(Env e) : Resource(Kind::System, e) {}
  ~System();
  void dispose();
  void require() const;
  napi_value callback(const char *group, const char *name) const;
  napi_value configuration() const;
  void capture();
  void capture(const std::exception &error);
  ta_status_t invoke(napi_value fn, const std::vector<napi_value> &args,
                     napi_value *out);
};
struct Context : Resource {
  ta_context_t *context = nullptr;
  System *owner = nullptr;
  Context(Env e) : Resource(Kind::Context, e) {}
  void require() const;
};
struct Memory : Resource {
  std::shared_ptr<Arena> owner;
  void *data = nullptr;
  size_t size = 0;
  Memory(Env e) : Resource(Kind::Memory, e) {}
};
struct Cursor : Resource {
  std::weak_ptr<System> owner;
  ta_inspect_cursor_t cursor{};
  Cursor(Env e) : Resource(Kind::Cursor, e) {}
};
struct Guard {
  System &system;
  explicit Guard(System &s);
  ~Guard();
};

struct Addon {
  Env js;
  bool closing = false;
  std::vector<std::weak_ptr<Resource>> resources;
  std::unordered_map<ta_arena_t *, std::weak_ptr<Arena>> arenas;
  size_t description_depth = 0, description_nodes = 0;
};
Addon &addon(Env js);
napi_value wrap(const std::shared_ptr<Resource> &value,
                napi_value parent = nullptr);
std::shared_ptr<Resource> unwrap(Env js, napi_value value, Kind kind);
std::shared_ptr<System> system(Env js, napi_value value);
std::shared_ptr<Definition> definition(Env js, napi_value value);
std::shared_ptr<Arena> arena(Env js, napi_value value);
std::shared_ptr<Context> context(Env js, napi_value value);
std::shared_ptr<Arena> borrowArena(System &system, ta_arena_t *arena);
napi_value wrapDefinition(const std::shared_ptr<System> &owner,
                          ta_definition_t *ptr, napi_value parent);
ta_chunk_allocator_t allocator(Env js);
std::shared_ptr<Arena> createArena(Env js, const ta_arena_options_t &options);
void watchArena(const std::shared_ptr<Arena> &arena);
void initializeAddon(Env js);

ta_value_t root(System &system, napi_value value);
napi_value value(ta_value_t value, Env js);
void retainRoot(void *user, ta_value_t value);
void releaseRoot(void *user, ta_value_t value);
void installCallbacks(System &system, napi_value config);
void installTelemetry(System &system, napi_value config);

/* A transient parser owns stable native copies and temporary JS roots. The C
 * definition builder deep-copies these before this object is destroyed. */
struct Parse {
  Env js;
  System *owner;
  ta_arena_t *storage = nullptr;
  std::vector<ta_value_t> roots;
  size_t nodes = 0, depth = 0, max_nodes = 1000000, max_depth = 64,
         max_bytes = 64u * 1024u * 1024u;
  Parse(Env js, System *owner = nullptr);
  ~Parse();
  Parse(const Parse &) = delete;
  void *alloc(size_t count, size_t size,
              size_t alignment = alignof(std::max_align_t));
  template <class T> T *items(size_t n) {
    return static_cast<T *>(alloc(n, sizeof(T), alignof(T)));
  }
  template <class T> T *terminated(size_t n) {
    if (n == SIZE_MAX)
      throw std::range_error("Sentinel array size overflow");
    return items<T>(n + 1);
  }
  ta_text_t text(napi_value input);
  const char *cstr(napi_value input);
  ta_bytes_t bytes(napi_value input);
  ta_value_t native(napi_value input);
  void enter();
  void leave();
};

/* structures.cc: complete descriptor and scalar marshalling. */
ta_arena_options_t readArenaOptions(Parse &p, napi_value input);
ta_layout_t readLayout(Parse &p, napi_value input);
ta_actor_limits_t readActorLimits(Parse &p, napi_value input,
                                  const ta_actor_limits_t *base = nullptr);
ta_system_limits_t readSystemLimits(Parse &p, napi_value input,
                                    const ta_system_limits_t *base = nullptr);
ta_chart_t readChart(Parse &p, napi_value input);
ta_definition_desc_t readDescription(Parse &p, napi_value input);
ta_target_t readTarget(Parse &p, napi_value input);
ta_payload_t readPayload(Parse &p, napi_value input);
ta_body_t readBody(Parse &p, napi_value input);
ta_event_t readEvent(Parse &p, napi_value input);
ta_send_t readSend(Parse &p, napi_value input);
const ta_trace_context_t *readTrace(Parse &p, napi_value input);
napi_value writeTrace(Env js, const ta_trace_context_t *input);
napi_value writePayload(Env js, const ta_payload_t &input);
napi_value writeBody(Env js, const ta_body_t &input);
napi_value writeEvent(Env js, const ta_event_t &input);
napi_value writeData(Env js, const ta_data_view_t &input);
napi_value writeMemory(Env js, const ta_memory_stats_t &input);
napi_value writeActorInfo(Env js, const ta_actor_info_t &input);
napi_value writeSystemStats(Env js, const ta_system_stats_t &input);
napi_value writeDiagnostic(Env js, const ta_diagnostic_t &input);
napi_value writeRequest(Env js, const ta_io_request_t &input);
napi_value writeDescription(Env js, const ta_definition_desc_t &input,
                            System *owner);
napi_value writeContext(System &system, ta_context_t *input);
napi_value writeSystemValues(Env js, const ta_system_values_t &input);
napi_value constants(Env js);

/* Context callback wrapper invalidates its handle on every return path. */
struct CallbackContext {
  napi_handle_scope scope = nullptr;
  std::shared_ptr<Context> handle;
  napi_value object;
  explicit CallbackContext(ta_context_t *context);
  ~CallbackContext();
};
} // namespace tn
#endif
