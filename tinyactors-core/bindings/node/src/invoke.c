/* invoke.c — external invocations (<invoke type> the core does not run itself): their
 * table, so effects come with the invocation they are about; core.mjs starts the
 * services (binding.h). */
#include "binding.h"

static nd_invocation *invocation_at(nd_system *system, uint32_t segment, uint32_t token) {
    for (uint32_t i = 0; i < system->invocation_count; i++)
        if (system->invocations[i].segment == segment && system->invocations[i].token == token)
            return &system->invocations[i];
    return NULL;
}

static void invocation_drop(nd_system *system, nd_invocation *inv) {
    set_ref(system->environment, &inv->object, NULL);
    free(inv->invokeid);
    *inv = system->invocations[--system->invocation_count];
}

/* TA_EFFECT_INVOKE: a new invocation { session, segment, token, invokeid }, kept */
napi_value invocation_new(nd_system *system, const ta_host_effect *effect) {
    napi_env environment = system->environment;
    napi_value o;
    napi_create_object(environment, &o);
    set(environment, o, "session", num(environment, (double)effect->session));
    set(environment, o, "segment", num(environment, effect->segment));
    set(environment, o, "token", num(environment, effect->token));
    set(environment, o, "invokeid", str_value(environment, effect->sendid));
    grow((void **)&system->invocations, &system->invocation_capacity, system->invocation_count + 1,
         sizeof(nd_invocation));
    nd_invocation *inv = &system->invocations[system->invocation_count++];
    memset(inv, 0, sizeof *inv);
    inv->segment = effect->segment;
    inv->token = effect->token;
    inv->session = effect->session;
    inv->invokeid = (char *)malloc(effect->sendid.length + 1);
    if (!inv->invokeid) abort();
    memcpy(inv->invokeid, effect->sendid.pointer, effect->sendid.length);
    inv->invokeid[effect->sendid.length] = 0;
    inv->object = make_ref(environment, o);
    return o;
}

/* the invocation a TA_EFFECT_SEND is for (an invoked service, or "#_<invokeid>"), if any */
napi_value invocation_of_send(nd_system *system, const ta_host_effect *effect) {
    nd_invocation *inv = NULL;
    if (effect->io_processor == TA_NIL && effect->token)
        inv = invocation_at(system, effect->segment, effect->token);
    if (!inv && effect->target.length > 2 && effect->target.pointer[0] == '#' &&
        effect->target.pointer[1] == '_')
        for (uint32_t i = 0; i < system->invocation_count && !inv; i++) {
            nd_invocation *c = &system->invocations[i];
            if (c->session == effect->session && strlen(c->invokeid) == effect->target.length - 2 &&
                !memcmp(c->invokeid, effect->target.pointer + 2, effect->target.length - 2))
                inv = c;
        }
    return inv ? get_ref(system->environment, inv->object) : NULL;
}

/* TA_EFFECT_UNINVOKE: its invocation, no longer kept */
napi_value invocation_end(nd_system *system, const ta_host_effect *effect) {
    nd_invocation *inv = invocation_at(system, effect->segment, effect->token);
    if (!inv) return NULL;
    napi_value o = get_ref(system->environment, inv->object);
    invocation_drop(system, inv);
    return o;
}

void invocations_clear(nd_system *system) {
    while (system->invocation_count)
        invocation_drop(system, &system->invocations[system->invocation_count - 1]);
    free(system->invocations);
}

/* uninvoked(system, segment, token): the service ended (done, fail, no invoker) */
napi_value js_uninvoked(napi_env environment, napi_callback_info info) {
    size_t argc = 3;
    napi_value argv[3];
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    nd_system *system = argc >= 3 ? get_system(environment, argv[0]) : NULL;
    if (!system) return NULL;
    nd_invocation *inv =
        invocation_at(system, get_u32(environment, argv[1]), get_u32(environment, argv[2]));
    if (inv) invocation_drop(system, inv);
    return undef(environment);
}

/* invocations(system) -> the invocations kept (to abort when the system stops) */
napi_value js_invocations(napi_env environment, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1], out;
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    nd_system *system = argc >= 1 ? get_system(environment, argv[0]) : NULL;
    if (!system) return NULL;
    napi_create_array_with_length(environment, system->invocation_count, &out);
    for (uint32_t i = 0; i < system->invocation_count; i++)
        napi_set_element(environment, out, i, get_ref(environment, system->invocations[i].object));
    return out;
}
