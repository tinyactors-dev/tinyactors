/*
** engine.c -- the freestanding pocketparse engine spine.
**
** Opens and closes an engine over the host's memory grant and reports faults.
** The grant is wrapped as an arena (pp_arena_open); every later allocation
** -- grammars, parsers, trees, diagnostics -- is carved from that one arena, so
** closing the engine is a no-op beyond the host freeing its own block. The
** grammar, parser, tree, diagnostic, reader, and lexer entry points live in
** their own translation units (grammar.c, parse.c, tree.c, diag.c, reader.c,
** token.c, util.c). What remains here is the lifecycle plus the not-yet-built
** plug/binding stubs. Freestanding: no libc, no malloc.
*/
#include "internal.h"

/* ** Open an engine over the host grant. The grant becomes the engine's root
** arena (arena.c); the engine state is the first thing carved from it, and
** every later allocation -- grammars, parsers, trees, diagnostics -- comes from
** that arena or from a parser's child arena on the same pool. Returns NULL if the
** grant is absent or too small to hold the arena bookkeeping plus the engine. */
pocketparse_engine *pocketparse_open(pocketparse_configuration configuration)
{
    pp_arena           *arena;
    pocketparse_engine *engine;

    if (configuration.memory.base == 0 || configuration.memory.size == 0)
        return 0;

    arena = pp_arena_open(configuration.memory);
    if (arena == 0)
        return 0;

    engine = (pocketparse_engine *)pp_alloc(arena, sizeof *engine);
    if (engine == 0)
        return 0;

    engine->arena               = arena;
    engine->fault               = pp_text("");
    engine->flags               = configuration.flags;
    engine->module_resolve      = 0;
    engine->module_user         = 0;
    engine->bindings            = 0;
    engine->binding_count       = 0;
    engine->binding_capacity    = 0;
    engine->plugs               = 0;
    engine->plug_count          = 0;
    engine->plug_capacity       = 0;
    engine->params              = 0;
    engine->param_count         = 0;
    engine->param_capacity      = 0;
    engine->languages           = 0;
    engine->language_count      = 0;
    engine->language_capacity   = 0;
    engine->vm_rule_memo_force  = 0;
    engine->emit_parser_prefix  = 0;
    return engine;
}

/* ** Close an engine. The arena and the engine both live inside the host's
** block, which the host owns and frees, so there is nothing to release. */
void pocketparse_close(pocketparse_engine *engine)
{
    (void)engine;
}

/* ** Set the rule-memo force override (see internal.h). Host-set, since the
** freestanding core cannot read the environment; the CLI maps POCKETPARSE_RULEMEMO
** to it so the differential can exercise the memo's replay path. */
void pp_engine_force_rule_memo(pocketparse_engine *engine, int mode)
{
    if (engine != 0)
        engine->vm_rule_memo_force = mode;
}

/* ** Set the compiled-parser symbol prefix (see internal.h / emit_parser_prefix).
** Host-set behind the frozen public API: the CLI passes the `--name P' argument
** here so the next `c ... --parser' emit renames every file-scope symbol to P_...
** and two generated parsers can link into one program. NULL restores the default
** names, leaving the cc-diff oracle byte-for-byte unchanged. The pointer is
** host-owned (argv) and outlives the synchronous emit, so nothing is copied. */
void pp_engine_set_parser_prefix(pocketparse_engine *engine, const char *prefix)
{
    if (engine != 0)
        engine->emit_parser_prefix = prefix;
}

/* ** The engine's last fault message, or an empty string. */
pocketparse_string pocketparse_fault(const pocketparse_engine *engine)
{
    if (engine == 0)
        return pp_text("");
    return engine->fault;
}

/* ** Copy a name's bytes into the engine arena so a stored binding outlives the
** host's pocketparse_binding. An empty name copies to an empty slice. */
static pocketparse_string pp_arena_copy_name(pp_arena *arena,
                                             pocketparse_string name)
{
    pocketparse_string out;
    uint8_t           *bytes;
    uint32_t           i;
    if (name.length == 0) {
        out.pointer = (const uint8_t *)"";
        out.length  = 0;
        return out;
    }
    bytes = (uint8_t *)pp_alloc(arena, name.length);
    if (bytes == 0) {
        out.pointer = 0;
        out.length  = 0;
        return out;
    }
    for (i = 0; i < name.length; i++)
        bytes[i] = name.pointer[i];
    out.pointer = bytes;
    out.length  = name.length;
    return out;
}

/* ** Register a host binding. The binding record and its name are copied into the
** engine arena, so the host need not keep its pocketparse_binding alive. Binding
** a name that already exists with the same kind replaces the previous entry (its
** function/user are overwritten in place) rather than accumulating duplicates,
** so the CLI may install a built-in class twice without harm. The table grows
** geometrically from the arena -- arenas never realloc, so a fresh, larger
** block is allocated and the existing entries copied across. */
pocketparse_result pocketparse_bind(
    pocketparse_engine        *engine,
    const pocketparse_binding *binding)
{
    uint32_t i;

    if (engine == 0)
        return POCKETPARSE_INVALID;
    if (binding == 0 || binding->name.length == 0) {
        engine->fault = pp_text("bind needs a named binding");
        return POCKETPARSE_INVALID;
    }

    /* Replace a same-kind binding of the same name in place. */
    for (i = 0; i < engine->binding_count; i++) {
        if (engine->bindings[i].kind == binding->kind &&
            pp_string_equal(engine->bindings[i].name, binding->name)) {
            engine->bindings[i].function = binding->function;
            engine->bindings[i].user     = binding->user;
            return POCKETPARSE_OK;
        }
    }

    if (engine->binding_count == engine->binding_capacity) {
        uint32_t    new_cap = engine->binding_capacity == 0
                                  ? 8 : engine->binding_capacity * 2;
        pp_binding *grown = (pp_binding *)pp_alloc(
            engine->arena, new_cap * sizeof *grown);
        if (grown == 0) {
            engine->fault = pp_text("out of memory binding");
            return POCKETPARSE_OUT_OF_MEMORY;
        }
        for (i = 0; i < engine->binding_count; i++)
            grown[i] = engine->bindings[i];
        engine->bindings         = grown;
        engine->binding_capacity = new_cap;
    }

    {
        pp_binding *slot = &engine->bindings[engine->binding_count];
        slot->name = pp_arena_copy_name(engine->arena, binding->name);
        if (slot->name.pointer == 0) {
            engine->fault = pp_text("out of memory binding");
            return POCKETPARSE_OUT_OF_MEMORY;
        }
        slot->kind     = binding->kind;
        slot->function = binding->function;
        slot->user     = binding->user;
        engine->binding_count++;
    }
    return POCKETPARSE_OK;
}

/* ** Resolve a class name to its installed CLASS binding, or NULL. The matcher
** calls this for each (class NAME) atom; the binding count is small, so a linear
** scan is fine on the cold path of installing-then-parsing. */
const pp_binding *pp_engine_find_class(const pocketparse_engine *engine,
                                       pocketparse_string name)
{
    uint32_t i;
    if (engine == 0)
        return 0;
    for (i = 0; i < engine->binding_count; i++)
        if (engine->bindings[i].kind == POCKETPARSE_BIND_CLASS &&
            pp_string_equal(engine->bindings[i].name, name))
            return &engine->bindings[i];
    return 0;
}

/* ** Register an embedded language under a name (W17-3 native nesting). The name is
** copied into the engine arena; the entry (grammar pointer + optional generated-
** parser run function) is stored by value, so the host owns the lifetimes those
** pointers reference. Registering a name already present replaces its entry in
** place, so a host may swap an embedded parser without accumulating duplicates.
** The table grows geometrically from the arena (arenas never realloc, so a
** larger block is allocated and the live entries copied across). Registration is a
** cold path the host runs before parsing, never during it. */
pocketparse_result pocketparse_register_language(
    pocketparse_engine               *engine,
    pocketparse_string                name,
    const pocketparse_language_entry *entry)
{
    uint32_t i;

    if (engine == 0 || entry == 0)
        return POCKETPARSE_INVALID;
    if (name.length == 0) {
        engine->fault = pp_text("register_language needs a named language");
        return POCKETPARSE_INVALID;
    }

    /* Replace an existing registration of the same name in place. */
    for (i = 0; i < engine->language_count; i++)
        if (pp_string_equal(engine->languages[i].name, name)) {
            engine->languages[i].entry = *entry;
            return POCKETPARSE_OK;
        }

    if (engine->language_count == engine->language_capacity) {
        uint32_t          new_cap = engine->language_capacity == 0
                                        ? 4 : engine->language_capacity * 2;
        pp_language_slot *grown = (pp_language_slot *)pp_alloc(
            engine->arena, new_cap * sizeof *grown);
        if (grown == 0) {
            engine->fault = pp_text("out of memory registering language");
            return POCKETPARSE_OUT_OF_MEMORY;
        }
        for (i = 0; i < engine->language_count; i++)
            grown[i] = engine->languages[i];
        engine->languages         = grown;
        engine->language_capacity = new_cap;
    }

    {
        pp_language_slot *slot = &engine->languages[engine->language_count];
        slot->name = pp_arena_copy_name(engine->arena, name);
        if (slot->name.pointer == 0) {
            engine->fault = pp_text("out of memory registering language");
            return POCKETPARSE_OUT_OF_MEMORY;
        }
        slot->entry = *entry;
        engine->language_count++;
    }
    return POCKETPARSE_OK;
}

/* ** Resolve a registered embedded language by name, or NULL if none is registered
** under that name. The inject runtime calls this to decide whether to run a
** registered (possibly generated) parser over a region or fall back to the
** compile-time embedded grammar. The registry is small and consulted only at an
** inject site (cold relative to the scanner loop), so a linear scan is fine. */
const pocketparse_language_entry *pp_engine_find_language(
    const pocketparse_engine *engine, pocketparse_string name)
{
    uint32_t i;
    if (engine == 0 || name.length == 0)
        return 0;
    for (i = 0; i < engine->language_count; i++)
        if (pp_string_equal(engine->languages[i].name, name))
            return &engine->languages[i].entry;
    return 0;
}

/* ** Install a host plug. The plug record and its name are copied into the engine
** arena, so the host need not keep its pocketparse_plug alive. Installing a name
** that already exists replaces the previous plug in place (its emit/user are
** overwritten) rather than accumulating duplicates, so re-installing "trace" with
** a different sink simply swaps it. The table grows geometrically from the arena;
** arenas never realloc, so a fresh, larger block is allocated and the existing
** entries copied across. Plugs are cold-path host boundaries -- this is the only
** mutation of the plug table, and it happens before parsing, never during it. */
pocketparse_result pocketparse_plug_install(
    pocketparse_engine     *engine,
    const pocketparse_plug *plug)
{
    uint32_t i;

    if (engine == 0)
        return POCKETPARSE_INVALID;
    if (plug == 0 || plug->name.length == 0 || plug->emit == 0) {
        engine->fault = pp_text("plug_install needs a named plug with an emit");
        return POCKETPARSE_INVALID;
    }

    /* Replace a plug of the same name in place. */
    for (i = 0; i < engine->plug_count; i++) {
        if (pp_string_equal(engine->plugs[i].name, plug->name)) {
            engine->plugs[i].emit = plug->emit;
            engine->plugs[i].user = plug->user;
            return POCKETPARSE_OK;
        }
    }

    if (engine->plug_count == engine->plug_capacity) {
        uint32_t new_cap = engine->plug_capacity == 0
                               ? 4 : engine->plug_capacity * 2;
        pp_plug *grown = (pp_plug *)pp_alloc(
            engine->arena, new_cap * sizeof *grown);
        if (grown == 0) {
            engine->fault = pp_text("out of memory plug");
            return POCKETPARSE_OUT_OF_MEMORY;
        }
        for (i = 0; i < engine->plug_count; i++)
            grown[i] = engine->plugs[i];
        engine->plugs        = grown;
        engine->plug_capacity = new_cap;
    }

    {
        pp_plug *slot = &engine->plugs[engine->plug_count];
        slot->name = pp_arena_copy_name(engine->arena, plug->name);
        if (slot->name.pointer == 0) {
            engine->fault = pp_text("out of memory plug");
            return POCKETPARSE_OUT_OF_MEMORY;
        }
        slot->emit = plug->emit;
        slot->user = plug->user;
        engine->plug_count++;
    }
    return POCKETPARSE_OK;
}

/* ** Resolve a plug name to its installed plug, or NULL. The plug count is small,
** so a linear scan is fine on this cold path. */
const pp_plug *pp_engine_find_plug(const pocketparse_engine *engine,
                                   pocketparse_string name)
{
    uint32_t i;
    if (engine == 0)
        return 0;
    for (i = 0; i < engine->plug_count; i++)
        if (pp_string_equal(engine->plugs[i].name, name))
            return &engine->plugs[i];
    return 0;
}

/* ** Emit one trace event through the installed "trace" plug, if any. With no
** trace plug installed this returns immediately, having built and formatted
** nothing -- the trace seam on the hot path is then a single pointer comparison.
** When a plug is present, the event name (a C literal supplied at the trace point)
** and the data slice become a pocketparse_message and the plug's emit is called;
** the return code is ignored, since tracing is observational. */
void pp_trace(pocketparse_engine *engine, const char *event,
              pocketparse_string data)
{
    const pp_plug      *plug;
    pocketparse_message message;
    pocketparse_string  empty;

    if (engine == 0 || engine->plug_count == 0)
        return;
    plug = pp_engine_find_plug(engine, pp_text("trace"));
    if (plug == 0)
        return;

    empty.pointer = (const uint8_t *)"";
    empty.length  = 0;
    message.name   = pp_text(event);
    message.target = empty;
    message.data   = data;
    message.flags  = 0;
    (void)plug->emit(plug->user, &message);
}

/* ** Set (or clear, with value 0) a compile-time grammar parameter. The name is
** copied into the engine arena so the host need not keep it. Setting a name that
** is already present updates its value in place; the table grows geometrically
** from the arena (arenas never realloc, so a fresh, larger block is allocated
** and the existing entries copied across). The (when PARAM: X) compiler reads
** these back through pp_engine_param_set while a grammar is compiled. */
pocketparse_result pocketparse_set_param(
    pocketparse_engine *engine,
    pocketparse_string  name,
    int                 value)
{
    uint32_t i;

    if (engine == 0)
        return POCKETPARSE_INVALID;
    if (name.length == 0) {
        engine->fault = pp_text("set_param needs a parameter name");
        return POCKETPARSE_INVALID;
    }

    /* Update an existing parameter of the same name in place. */
    for (i = 0; i < engine->param_count; i++)
        if (pp_string_equal(engine->params[i].name, name)) {
            engine->params[i].value = value;
            return POCKETPARSE_OK;
        }

    if (engine->param_count == engine->param_capacity) {
        uint32_t  new_cap = engine->param_capacity == 0
                                ? 8 : engine->param_capacity * 2;
        pp_param *grown = (pp_param *)pp_alloc(
            engine->arena, new_cap * sizeof *grown);
        if (grown == 0) {
            engine->fault = pp_text("out of memory setting parameter");
            return POCKETPARSE_OUT_OF_MEMORY;
        }
        for (i = 0; i < engine->param_count; i++)
            grown[i] = engine->params[i];
        engine->params         = grown;
        engine->param_capacity = new_cap;
    }

    {
        pp_param *slot = &engine->params[engine->param_count];
        slot->name = pp_arena_copy_name(engine->arena, name);
        if (slot->name.pointer == 0) {
            engine->fault = pp_text("out of memory setting parameter");
            return POCKETPARSE_OUT_OF_MEMORY;
        }
        slot->value = value;
        engine->param_count++;
    }
    return POCKETPARSE_OK;
}

/* ** Report whether a grammar parameter is set (truthy) on the engine. A linear
** scan over the small parameter table; the (when PARAM: X) compiler calls it once
** per gated production on the cold compile path. */
int pp_engine_param_set(const pocketparse_engine *engine,
                        pocketparse_string name)
{
    uint32_t i;
    if (engine == 0)
        return 0;
    for (i = 0; i < engine->param_count; i++)
        if (pp_string_equal(engine->params[i].name, name))
            return engine->params[i].value != 0;
    return 0;
}

/* ** Install (or clear, with a NULL resolve) the host's module resolver. The
** grammar compiler reads it back when it meets an (import NAME) form. */
pocketparse_result pocketparse_set_module_resolver(
    pocketparse_engine          *engine,
    pocketparse_module_resolver  resolve,
    void                        *user)
{
    if (engine == 0)
        return POCKETPARSE_INVALID;
    engine->module_resolve = resolve;
    engine->module_user    = user;
    return POCKETPARSE_OK;
}
