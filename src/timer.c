#include "internal.h"

static int before(const tai_timer_t *left, const tai_timer_t *right)
{
    return left->deadline < right->deadline ||
           (left->deadline == right->deadline && left->sequence < right->sequence);
}

static uint64_t priority(uint64_t sequence)
{
    sequence ^= sequence >> 30; sequence *= 0xbf58476d1ce4e5b9ull;
    sequence ^= sequence >> 27; sequence *= 0x94d049bb133111ebull;
    return sequence ^ (sequence >> 31);
}

static void rotate_left(ta_system_t *system, tai_timer_t *node)
{
    tai_timer_t *up = node->right;
    node->right = up->left; if (up->left) up->left->parent = node;
    up->parent = node->parent;
    if (!node->parent) system->timers = up;
    else if (node == node->parent->left) node->parent->left = up; else node->parent->right = up;
    up->left = node; node->parent = up;
}

static void rotate_right(ta_system_t *system, tai_timer_t *node)
{
    tai_timer_t *up = node->left;
    node->left = up->right; if (up->right) up->right->parent = node;
    up->parent = node->parent;
    if (!node->parent) system->timers = up;
    else if (node == node->parent->left) node->parent->left = up; else node->parent->right = up;
    up->right = node; node->parent = up;
}

static void tree_insert(ta_system_t *system, tai_timer_t *timer)
{
    tai_timer_t *node = system->timers, *parent = NULL;
    while (node != NULL) { parent = node; node = before(timer, node) ? node->left : node->right; }
    timer->parent = parent;
    if (parent == NULL) system->timers = timer;
    else if (before(timer, parent)) parent->left = timer; else parent->right = timer;
    while (timer->parent != NULL && priority(timer->sequence) < priority(timer->parent->sequence)) {
        if (timer == timer->parent->left) rotate_right(system, timer->parent);
        else rotate_left(system, timer->parent);
    }
}

static void tree_remove(ta_system_t *system, tai_timer_t *timer)
{
    while (timer->left != NULL || timer->right != NULL) {
        if (timer->left == NULL) rotate_left(system, timer);
        else if (timer->right == NULL) rotate_right(system, timer);
        else if (priority(timer->left->sequence) < priority(timer->right->sequence)) rotate_right(system, timer);
        else rotate_left(system, timer);
    }
    if (timer->parent == NULL) system->timers = NULL;
    else if (timer == timer->parent->left) timer->parent->left = NULL;
    else timer->parent->right = NULL;
    timer->parent = NULL;
}

static tai_timer_t *tree_min(ta_system_t *system)
{
    tai_timer_t *node = system->timers;
    if (node == NULL) return NULL;
    while (node->left != NULL) node = node->left;
    return node;
}

static void actor_link(tai_timer_t *timer)
{
    tai_actor_extra_t *extra = timer->actor->extra;
    timer->previous = NULL; timer->next = extra->timers;
    if (extra->timers != NULL) extra->timers->previous = timer;
    extra->timers = timer;
}

static void actor_unlink(tai_timer_t *timer)
{
    tai_actor_extra_t *extra = timer->actor->extra;
    if (timer->previous) timer->previous->next = timer->next; else extra->timers = timer->next;
    if (timer->next) timer->next->previous = timer->previous;
    timer->next = timer->previous = NULL;
}

static ta_status_t copy_send(const tai_value_scope_t *scope, const ta_send_t *source, ta_send_t *out)
{
    ta_status_t status;
    ta_arena_t *arena = scope->arena;
    *out = *source;
    status = tai_text_copy(arena, source->event, &out->event);
    if (status == TA_OK) status = tai_text_copy(arena, source->type, &out->type);
    if (status == TA_OK) status = tai_text_copy(arena, source->target.text, &out->target.text);
    if (status == TA_OK) status = tai_text_copy(arena, source->sendid, &out->sendid);
    if (status == TA_OK) status = tai_body_snapshot(scope, &source->body, &out->body);
    return status;
}

ta_status_t tai_timer_schedule(tai_actor_t *actor, const ta_send_t *send, tai_timer_t **out)
{
    ta_system_t *system;
    ta_arena_t *arena;
    tai_timer_t *timer;
    tai_value_scope_t scope;
    ta_status_t status;
    uint64_t sequence, deadline;
    if (out == NULL) return TA_INVALID;
    *out = NULL;
    if (actor == NULL || send == NULL || actor->system == NULL || send->delay_ns == 0u) return TA_INVALID;
    system = actor->system;
    if (!system->time_set || send->delay_ns > UINT64_MAX - system->now_ns) return TA_INVALID;
    deadline = system->now_ns + send->delay_ns;
    if (deadline == TA_TIME_NEVER || actor->limits == NULL ||
        (actor->extra != NULL && actor->extra->delayed_count >= actor->limits->delayed_sends) ||
        actor->limits->delayed_sends == 0 ||
        system->stats.delayed_sends >= system->config.limits.delayed_sends || system->next_message == 0u) return TA_LIMIT;
    status = tai_extra(actor);
    if (status != TA_OK) return status;
    sequence = system->next_message;
    status = tai_actor_scope(actor, TA_LIFETIME_MESSAGE, &arena);
    if (status != TA_OK) return status;
    status = tai_alloc(arena, sizeof(*timer), (void **)&timer);
    if (status != TA_OK) { ta_arena_close(arena); return status; }
    *timer = (tai_timer_t){0};
    timer->arena = arena; timer->actor = actor; timer->accepted_ns = system->now_ns;
    timer->deadline = deadline; timer->sequence = sequence;
    scope = (tai_value_scope_t){system, arena};
    status = copy_send(&scope, send, &timer->send);
    if (status != TA_OK) { ta_arena_close(arena); return status; }
    tree_insert(system, timer); actor_link(timer);
    ++actor->extra->delayed_count; ++system->stats.delayed_sends;
    system->next_message = sequence == UINT64_MAX ? 0 : sequence + 1;
    tai_mutated(system);
    *out = timer;
    return TA_OK;
}

tai_timer_t *tai_timer_take(ta_system_t *system)
{
    tai_timer_t *timer;
    if (system == NULL) return NULL;
    timer = tree_min(system); if (timer == NULL) return NULL;
    tree_remove(system, timer); actor_unlink(timer);
    if (timer->actor->extra->delayed_count) --timer->actor->extra->delayed_count;
    if (system->stats.delayed_sends) --system->stats.delayed_sends;
    tai_mutated(system);
    return timer;
}

void tai_timer_dispose(tai_timer_t *timer, ta_status_t status)
{
    if (timer == NULL) return;
    if (timer->span != NULL) tai_span_finish(timer->span, status);
    ta_arena_close(timer->arena);
}

uint8_t tai_timer_due(ta_system_t *system)
{
    tai_timer_t *timer = tree_min(system);
    return (uint8_t)(timer != NULL && timer->deadline <= system->now_ns);
}

uint64_t ta_system_next_deadline(const ta_system_t *system)
{
    tai_timer_t *timer;
    if (system == NULL) return TA_TIME_NEVER;
    timer = system->timers;
    if (timer == NULL) return TA_TIME_NEVER;
    while (timer->left != NULL) timer = timer->left;
    return timer->deadline;
}

void tai_cancel_timers(tai_actor_t *actor)
{
    tai_timer_t *timer;
    if (actor == NULL || actor->extra == NULL) return;
    while ((timer = actor->extra->timers) != NULL) {
        tree_remove(actor->system, timer); actor_unlink(timer);
        if (actor->extra->delayed_count) --actor->extra->delayed_count;
        if (actor->system->stats.delayed_sends) --actor->system->stats.delayed_sends;
        tai_timer_dispose(timer, TA_CANCELLED);
        tai_mutated(actor->system);
    }
}

ta_status_t ta_context_cancel(ta_context_t *context, ta_text_t sendid)
{
    tai_actor_t *actor;
    tai_timer_t *timer, *next;
    if (context == NULL || context->actor == NULL || !context->effects ||
        !tai_text_valid(sendid, 0)) return TA_INVALID;
    actor = context->actor;
    if (actor->extra == NULL) return TA_OK;
    for (timer = actor->extra->timers; timer != NULL; timer = next) {
        next = timer->next;
        if (tai_text_compare(timer->send.sendid, sendid) == 0) {
            tree_remove(actor->system, timer);
            actor_unlink(timer);
            --actor->extra->delayed_count;
            --actor->system->stats.delayed_sends;
            tai_timer_dispose(timer, TA_CANCELLED);
            tai_mutated(actor->system);
        }
    }
    return TA_OK;
}
