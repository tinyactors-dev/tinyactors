#include "internal.h"

static uint64_t actor_priority(uint64_t value)
{
    value += UINT64_C(0x9e3779b97f4a7c15);
    value = (value ^ (value >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    value = (value ^ (value >> 27)) * UINT64_C(0x94d049bb133111eb);
    return value ^ (value >> 31);
}

static void actor_left(tai_actor_t **root, tai_actor_t *entry)
{
    tai_actor_t *child = entry->right;
    entry->right = child->left;
    if (child->left != NULL) child->left->parent = entry;
    child->parent = entry->parent;
    if (entry->parent == NULL) *root = child;
    else if (entry == entry->parent->left) entry->parent->left = child;
    else entry->parent->right = child;
    child->left = entry; entry->parent = child;
}

static void actor_right(tai_actor_t **root, tai_actor_t *entry)
{
    tai_actor_t *child = entry->left;
    entry->left = child->right;
    if (child->right != NULL) child->right->parent = entry;
    child->parent = entry->parent;
    if (entry->parent == NULL) *root = child;
    else if (entry == entry->parent->left) entry->parent->left = child;
    else entry->parent->right = child;
    child->right = entry; entry->parent = child;
}

tai_actor_t *tai_actor_find(const ta_system_t *system, ta_actor_ref_t ref)
{
    tai_actor_t *at;
    if (system == NULL || ref.id == 0) return NULL;
    at = system->actor_index;
    while (at != NULL) {
        if (ref.id == at->id) return at;
        at = ref.id < at->id ? at->left : at->right;
    }
    return NULL;
}

void tai_actor_insert(ta_system_t *system, tai_actor_t *actor)
{
    tai_actor_t *at, *parent = NULL;
    if (system == NULL || actor == NULL) return;
    at = system->actor_index;
    while (at != NULL) { parent = at; at = actor->id < at->id ? at->left : at->right; }
    actor->left = actor->right = NULL; actor->parent = parent;
    if (parent == NULL) system->actor_index = actor;
    else if (actor->id < parent->id) parent->left = actor;
    else parent->right = actor;
    while (actor->parent != NULL && actor_priority(actor->id) < actor_priority(actor->parent->id)) {
        if (actor == actor->parent->left) actor_right(&system->actor_index, actor->parent);
        else actor_left(&system->actor_index, actor->parent);
    }
    actor->next = system->actors;
    actor->previous = NULL;
    if (system->actors != NULL) system->actors->previous = actor;
    system->actors = actor;
}

void tai_actor_remove(ta_system_t *system, tai_actor_t *actor)
{
    if (system == NULL || actor == NULL) return;
    while (actor->left != NULL || actor->right != NULL) {
        if (actor->right == NULL || (actor->left != NULL && actor_priority(actor->left->id) < actor_priority(actor->right->id)))
            actor_right(&system->actor_index, actor);
        else actor_left(&system->actor_index, actor);
    }
    if (actor->parent == NULL) system->actor_index = NULL;
    else if (actor->parent->left == actor) actor->parent->left = NULL;
    else actor->parent->right = NULL;
    actor->parent = NULL;
    if (actor->previous != NULL) actor->previous->next = actor->next;
    else if (system->actors == actor) system->actors = actor->next;
    if (actor->next != NULL) actor->next->previous = actor->previous;
    actor->next = actor->previous = NULL;
}

void tai_ready(tai_actor_t *actor)
{
    ta_system_t *s;
    if (actor == NULL || actor->queued) return;
    s = actor->system; if (s == NULL) return;
    actor->ready_next = NULL; actor->ready_previous = s->ready_tail;
    if (s->ready_tail != NULL) s->ready_tail->ready_next = actor; else s->ready_head = actor;
    s->ready_tail = actor; actor->queued = 1; ++s->stats.runnable_actors;
}

void tai_unready(tai_actor_t *actor)
{
    ta_system_t *s;
    if (actor == NULL || !actor->queued || actor->system == NULL) return;
    s = actor->system;
    if (actor->ready_previous != NULL) actor->ready_previous->ready_next = actor->ready_next; else s->ready_head = actor->ready_next;
    if (actor->ready_next != NULL) actor->ready_next->ready_previous = actor->ready_previous; else s->ready_tail = actor->ready_previous;
    actor->ready_next = actor->ready_previous = NULL; actor->queued = 0;
    if (s->stats.runnable_actors != 0) --s->stats.runnable_actors;
}

uint8_t tai_accepting(const tai_actor_t *actor)
{
    if (actor == NULL || actor->stop || actor->system == NULL || actor->system->shutdown) return 0;
    return actor->status != TA_ACTOR_STOPPING && actor->status != TA_ACTOR_DONE &&
           actor->status != TA_ACTOR_STOPPED && actor->status != TA_ACTOR_FAILED;
}

static uint8_t decimal_id(ta_text_t text, uint64_t *out)
{
    size_t i; uint64_t n = 0;
    if (text.data == NULL || text.size == 0 || (text.size > 1 && text.data[0] == '0')) return 0;
    for (i = 0; i < text.size; ++i) {
        uint8_t d;
        if (text.data[i] < '0' || text.data[i] > '9') return 0;
        d = (uint8_t)(text.data[i] - '0');
        if (n > (UINT64_MAX - d) / 10u) return 0;
        n = n * 10u + d;
    }
    if (n == 0) return 0;
    *out = n;
    return 1;
}

ta_status_t ta_name_bind(ta_system_t *system, const ta_name_binding_t *binding, ta_actor_ref_t replacement)
{
    tai_registry_t *entry; tai_actor_t *actor; ta_arena_t *arena; void *memory; ta_status_t st;
    if (system == NULL || binding == NULL || !tai_text_valid(binding->name, 0) || replacement.id == 0) return TA_INVALID;
    if (system->running) return TA_BUSY;
    if (system->shutdown) return TA_CANCELLED;
    actor = tai_actor_find(system, replacement); if (actor == NULL) return TA_STALE;
    if (!tai_accepting(actor)) return TA_CANCELLED;
    entry = tai_registry_find(system->names, binding->name);
    if ((binding->expected.id == 0 && entry != NULL) || (binding->expected.id != 0 && (entry == NULL || entry->actor.id != binding->expected.id))) return TA_CONFLICT;
    if (actor->name != NULL && actor->name != entry) return TA_CONFLICT;
    if (entry != NULL && entry->actor.id == replacement.id) return TA_OK;
    if (entry == NULL) {
        if (system->stats.names >= system->config.limits.names) return TA_LIMIT;
        st = tai_scope(system, TA_LIFETIME_DEFINITION, &arena); if (st != TA_OK) return st;
        st = tai_alloc(arena, sizeof(*entry), &memory); if (st != TA_OK) { ta_arena_close(arena); return st; }
        entry = (tai_registry_t *)memory; *entry = (tai_registry_t){0}; entry->arena = arena;
        st = tai_text_copy(arena, binding->name, &entry->key); if (st != TA_OK) { ta_arena_close(arena); return st; }
        tai_registry_insert(&system->names, entry); ++system->stats.names;
    } else if (entry->actor.id != 0) { tai_actor_t *old = tai_actor_find(system, entry->actor); if (old != NULL) old->name = NULL; }
    entry->actor = replacement; actor->name = entry; tai_mutated(system); return TA_OK;
}

ta_status_t ta_name_unbind(ta_system_t *system, const ta_name_binding_t *binding)
{
    tai_registry_t *entry;
    if (system == NULL || binding == NULL || !tai_text_valid(binding->name, 0)) return TA_INVALID;
    if (system->running) return TA_BUSY;
    entry = tai_registry_find(system->names, binding->name); if (entry == NULL) return TA_NOT_FOUND;
    if (binding->expected.id == 0 || entry->actor.id != binding->expected.id) return TA_CONFLICT;
    { tai_actor_t *a = tai_actor_find(system, entry->actor); if (a != NULL && a->name == entry) a->name = NULL; }
    tai_registry_remove(&system->names, entry); --system->stats.names; ta_arena_close(entry->arena); tai_mutated(system); return TA_OK;
}

ta_status_t ta_name_lookup(ta_system_t *system, ta_text_t name, ta_actor_ref_t *out_actor)
{
    tai_registry_t *e;
    if (system == NULL || out_actor == NULL || !tai_text_valid(name, 0)) return TA_INVALID;
    *out_actor = (ta_actor_ref_t){0}; e = tai_registry_find(system->names, name);
    if (e == NULL) return TA_NOT_FOUND;
    if (tai_actor_find(system, e->actor) == NULL) return TA_STALE;
    *out_actor = e->actor; return TA_OK;
}

ta_status_t ta_session_lookup(ta_system_t *system, ta_text_t sessionid, ta_actor_ref_t *out_actor)
{
    size_t p; uint64_t id; tai_actor_t *a;
    if (system == NULL || out_actor == NULL || !tai_text_valid(sessionid, 0)) return TA_INVALID;
    *out_actor = (ta_actor_ref_t){0}; p = system->config.session_prefix.size;
    if (sessionid.size <= p + 1 || tai_text_compare((ta_text_t){sessionid.data, p}, system->config.session_prefix) != 0 || sessionid.data[p] != '.') return TA_NOT_FOUND;
    if (!decimal_id((ta_text_t){sessionid.data + p + 1, sessionid.size - p - 1}, &id)) return TA_NOT_FOUND;
    a = tai_actor_find(system, (ta_actor_ref_t){id}); if (a == NULL || !tai_accepting(a)) return TA_STALE;
    *out_actor = (ta_actor_ref_t){id}; return TA_OK;
}

ta_status_t ta_target_parse(ta_text_t raw, ta_target_t *out_target)
{
    size_t i;
    if (out_target == NULL || (raw.data == NULL && raw.size != 0)) return TA_INVALID;
    for (i = 0; i < raw.size; ++i) if (raw.data[i] == '\0') return TA_INVALID;
    *out_target = (ta_target_t){0}; if (raw.data == NULL) return TA_OK;
    if (raw.size == 0) return TA_INVALID;
    if (raw.data[0] != '#') { out_target->kind = TA_TARGET_URI; out_target->text = raw; return TA_OK; }
    if (tai_text_compare(raw, (ta_text_t){"#_internal", 10}) == 0) { out_target->kind = TA_TARGET_INTERNAL; return TA_OK; }
    if (tai_text_compare(raw, (ta_text_t){"#_parent", 8}) == 0) { out_target->kind = TA_TARGET_PARENT; return TA_OK; }
    if (raw.size == 8 && tai_text_compare(raw, (ta_text_t){"#_scxml_", 8}) == 0) return TA_INVALID;
    if (raw.size > 2 && raw.data[1] == '_') {
        if (raw.size > 8 && tai_text_compare((ta_text_t){raw.data, 8}, (ta_text_t){"#_scxml_", 8}) == 0) {
            out_target->kind = TA_TARGET_URI; out_target->text = raw; return TA_OK;
        }
        out_target->kind = TA_TARGET_INVOKE; out_target->text = (ta_text_t){raw.data + 2, raw.size - 2}; return TA_OK;
    }
    out_target->kind = TA_TARGET_URI; out_target->text = raw; return TA_OK;
}
