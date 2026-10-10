/* policy.c — the tracing policy's rules (docs/adr/0007 in the repository
 * root): compiled into the system block when the policy is set, asked only
 * where something new starts (a trace, an actor taking an external event).
 * What a rule decides is written into the context's flags, so everything
 * the step causes carries it: nothing is asked per record or per event.
 * Rules hold indices and slab strings, never pointers. */
#include "base/memory.h"
#include "image/records.h"
#include "system/system_internal.h"

#define POLICY_KEEP_DETAIL  0xFFu
#define POLICY_NO_NAME      0xFFu
#define POLICY_ABSENT_CHART (TA_NIL - 1u) /* a chart this image does not have */

static ta_trace_rule_compiled *rules_of(const ta_system *system) {
    return SYSTEM_POINTER(system, rules_relative, ta_trace_rule_compiled);
}

static uint8_t *chart_points_of(const ta_system *system) {
    return SYSTEM_POINTER(system, chart_points_relative, uint8_t);
}

/* ---- matching ------------------------------------------------------------------------------ */

static int text_equal(ta_string_view a, ta_string_view b) {
    return a.length == b.length && (!a.length || ta_memcmp(a.pointer, b.pointer, a.length) == 0);
}

/* SCXML's descriptor match: `name` is the descriptor or starts with it and a dot. */
static int descriptor_matches(ta_string_view descriptor, ta_string_view name) {
    if (name.length < descriptor.length ||
        (descriptor.length && ta_memcmp(name.pointer, descriptor.pointer, descriptor.length)))
        return 0;
    return name.length == descriptor.length || name.pointer[descriptor.length] == '.';
}

static int name_matches(const ta_core *core, const ta_trace_rule_compiled *rule,
                        ta_string_view name) {
    ta_string_view want = ta_shared_string_get(core, rule->actor);
    if (!rule->actor_prefix) return text_equal(want, name);
    return name.length >= want.length &&
           (!want.length || ta_memcmp(name.pointer, want.pointer, want.length) == 0);
}

/* Which name rules match actor `index` (cached in its record until the
 * policy changes). */
static uint32_t actor_names(ta_system *system, uint32_t index) {
    ta_actor *actor = ta_actor_at(system, index);
    if (actor->flags & ACTOR_FLAG_TRACE_NAMES) return actor->trace_names;
    ta_string_view name = ta_shared_string_get(&system->core, actor->name);
    uint32_t mask = 0;
    const ta_trace_rule_compiled *rules = rules_of(system);
    for (uint32_t i = 0; i < system->rule_count; i++)
        if (rules[i].name_bit != POLICY_NO_NAME && name_matches(&system->core, &rules[i], name))
            mask |= 1u << rules[i].name_bit;
    actor->trace_names = (uint16_t)mask;
    actor->flags |= ACTOR_FLAG_TRACE_NAMES;
    return mask;
}

static int event_matches(const ta_core *core, const ta_trace_rule_compiled *rule, uint32_t event) {
    if (rule->event_first == TA_NIL && rule->event_text == TA_NIL) return 1;
    if (event == TA_NIL) return 0;
    const ta_event *e = ta_event_at(core, event);
    if (rule->event_first != TA_NIL)
        return e->name_id >= rule->event_first && e->name_id < rule->event_end;
    return descriptor_matches(ta_shared_string_get(core, rule->event_text),
                              ta_shared_string_get(core, e->name));
}

/* A token for a rate-limited rule, if one is there (logical time). */
static int rule_token(ta_system *system, ta_trace_rule_compiled *rule) {
    if (!rule->per_second) return 1;
    uint64_t tps = system->config.ticks_per_second;
    uint64_t capacity = (uint64_t)(rule->burst ? rule->burst : 1u) * tps;
    uint64_t elapsed = system->now > rule->refilled ? system->now - rule->refilled : 0;
    if (elapsed > capacity / rule->per_second + 1u) elapsed = capacity / rule->per_second + 1u;
    rule->tokens += elapsed * rule->per_second;
    if (rule->tokens > capacity) rule->tokens = capacity;
    rule->refilled = system->now;
    if (rule->tokens < tps) return 0;
    rule->tokens -= tps;
    return 1;
}

static int rule_matches(ta_system *system, const ta_trace_rule_compiled *rule,
                        const ta_trace_ask *ask) {
    if (!(rule->at & ask->at)) return 0;
    if (rule->from && !(rule->from & ask->from)) return 0;
    if (rule->chart != TA_NIL && rule->chart != ask->chart) return 0;
    if (rule->session || rule->name_bit != POLICY_NO_NAME) {
        if (ask->actor == TA_NIL) return 0;
        if (rule->session && ta_actor_at(system, ask->actor)->serial != rule->session) return 0;
        if (rule->name_bit != POLICY_NO_NAME &&
            !(actor_names(system, ask->actor) & (1u << rule->name_bit)))
            return 0;
    }
    return event_matches(&system->core, rule, ask->event);
}

int ta_system_trace_rules(ta_system *system, const ta_trace_ask *ask, ta_trace_id trace,
                          uint32_t *flags) {
    if (!(system->rule_points & ask->at)) return 0;
    if (ask->chart != TA_NIL && !(chart_points_of(system)[ask->chart] & ask->at)) return 0;
    ta_trace_rule_compiled *rules = rules_of(system);
    for (uint32_t i = 0; i < system->rule_count; i++) {
        ta_trace_rule_compiled *rule = &rules[i];
        if (!rule_matches(system, rule, ask) || !rule_token(system, rule)) continue;
        uint32_t sampled = *flags & TA_TRACE_FLAG_SAMPLED;
        if (rule->sample == TA_TRACE_SAMPLE) sampled = TA_TRACE_FLAG_SAMPLED;
        if (rule->sample == TA_TRACE_DROP) sampled = 0;
        if (rule->sample == TA_TRACE_RATIO)
            sampled = ta_trace_sampled(trace, rule->threshold) ? TA_TRACE_FLAG_SAMPLED : 0;
        uint32_t detail = TA_TRACE_DETAIL(*flags);
        if (!sampled)
            detail = TA_TRACE_DETAIL_UNSAMPLED;
        else if (rule->detail != POLICY_KEEP_DETAIL)
            detail = rule->detail;
        else if (detail == TA_TRACE_DETAIL_UNSAMPLED)
            detail = TA_TRACE_DETAIL_SAMPLED;
        uint32_t class_bits = (TA_TRACE_DETAILS - 1u) << TA_TRACE_DETAIL_SHIFT;
        *flags = (*flags & ~(uint32_t)TA_TRACE_FLAG_SAMPLED & ~class_bits) | sampled |
                 detail << TA_TRACE_DETAIL_SHIFT;
        return 1;
    }
    return 0;
}

/* ---- taking an event ------------------------------------------------------------------------ */

void ta_system_trace_take(ta_system *system, uint32_t index, uint32_t event_index) {
    ta_core *core = &system->core;
    ta_actor *actor = ta_actor_at(system, index);
    ta_event *event = ta_event_at(core, event_index);
    ta_trace_ask ask;
    ask.at = TA_TRACE_AT_TAKE;
    ask.actor = index;
    ask.chart = ta_actor_chart(system, actor);
    ask.event = event_index;
    uint32_t context = event->trace, flags = 0;
    ta_trace_id trace = {0, 0};
    ta_step_key cause = {0, 0, 0, 0};
    if (context != TA_NIL) {
        const ta_trace_entry *entry = ta_trace_entry_at(core, context);
        flags = entry->flags;
        trace = entry->trace;
        cause.session = entry->session;
        cause.macrostep = entry->macrostep;
        cause.microstep = entry->microstep;
    }
    ask.from = (event->origin ? TA_TRACE_FROM_ACTOR : TA_TRACE_FROM_HOST) |
               (flags & TA_TRACE_FLAG_SAMPLED ? TA_TRACE_FROM_SAMPLED : TA_TRACE_FROM_UNSAMPLED);
    if (context == TA_NIL) { /* untraced so far: a new trace, if a rule samples it */
        trace = ta_trace_generator_next(&system->generator);
        flags = TA_TRACE_FLAG_RANDOM;
    }
    uint32_t decided = flags;
    if (!ta_system_trace_rules(system, &ask, trace, &decided) || decided == flags) return;
    uint32_t replaced = TA_NIL;
    if (decided & TA_TRACE_FLAG_SAMPLED) {
        if (context == TA_NIL) {
            system->stats_traces_started++;
            system->stats_traces_sampled++;
        }
        replaced = ta_trace_context_new(core, trace, decided, cause);
        if (replaced == TA_NIL) return; /* no room: it stays as it was */
    }
    ta_trace_context_release(core, context); /* the sending step's other events keep theirs */
    event->trace = replaced;
}

/* ---- setting the rules ----------------------------------------------------------------------- */

static ta_string_view chart_name(const ta_core *core, uint32_t chart) {
    return ta_image_view_string(&core->image_view, core->image_view.charts[chart].name);
}

static void rules_release(ta_system *system) {
    ta_trace_rule_compiled *rules = rules_of(system);
    for (uint32_t i = 0; i < system->rule_count; i++) {
        ta_shared_string_release(&system->core, rules[i].actor);
        ta_shared_string_release(&system->core, rules[i].event_text);
    }
    system->rule_count = 0;
    system->rule_points = 0;
}

ta_status ta_system_trace_rules_set(ta_system *system, const ta_trace_policy *policy) {
    ta_core *core = &system->core;
    uint32_t count = policy->size >= offsetof(ta_trace_policy, rules) + sizeof policy->rules
                         ? policy->rule_count
                         : 0;
    if (count > system->config.trace_rules) return TA_ERROR_NO_MEMORY;
    if (count && !policy->rules) return TA_ERROR_INVALID;
    uint32_t names = 0;
    for (uint32_t i = 0; i < count; i++) {
        const ta_trace_rule *rule = &policy->rules[i];
        if ((rule->detail != TA_NIL && rule->detail >= TA_TRACE_DETAILS) ||
            rule->sample > TA_TRACE_RATIO || rule->at > (TA_TRACE_AT_START | TA_TRACE_AT_TAKE))
            return TA_ERROR_INVALID;
        names += rule->actor.length != 0;
    }
    if (names > TA_TRACE_NAME_RULES) return TA_ERROR_INVALID;
    /* the strings first: when the slab is full, nothing changes */
    uint32_t actors[256], texts[256];
    for (uint32_t i = 0; i < count; i++) {
        const ta_trace_rule *rule = &policy->rules[i];
        ta_string_view actor = rule->actor, event = rule->event;
        if (actor.length && actor.pointer[actor.length - 1] == '*') actor.length--;
        if (event.length >= 2 && event.pointer[event.length - 1] == '*' &&
            event.pointer[event.length - 2] == '.')
            event.length -= 2;
        else if (event.length == 1 && event.pointer[0] == '*')
            event.length = 0;
        int exact = 0;
        if (event.length)
            ta_event_name_lookup(&core->image_view, event.pointer, event.length, &exact);
        actors[i] =
            rule->actor.length ? ta_shared_string_new(core, actor.pointer, actor.length) : TA_NIL;
        texts[i] = event.length && !exact ? ta_shared_string_new(core, event.pointer, event.length)
                                          : TA_NIL;
        if ((rule->actor.length && actors[i] == TA_NIL) ||
            (event.length && !exact && texts[i] == TA_NIL)) {
            for (uint32_t k = 0; k <= i; k++) {
                ta_shared_string_release(core, actors[k]);
                ta_shared_string_release(core, texts[k]);
            }
            return TA_ERROR_NO_MEMORY;
        }
    }
    rules_release(system);
    ta_trace_rule_compiled *rules = rules_of(system);
    uint8_t *chart_points = chart_points_of(system);
    ta_memset(chart_points, 0, system->chart_count);
    uint32_t name_bit = 0;
    for (uint32_t i = 0; i < count; i++) {
        const ta_trace_rule *rule = &policy->rules[i];
        ta_trace_rule_compiled *c = &rules[i];
        ta_memset(c, 0, sizeof *c);
        c->at = (uint8_t)(rule->at ? rule->at : TA_TRACE_AT_START | TA_TRACE_AT_TAKE);
        c->sample = (uint8_t)rule->sample;
        c->detail = rule->detail == TA_NIL ? (uint8_t)POLICY_KEEP_DETAIL : (uint8_t)rule->detail;
        c->from = rule->from;
        c->session = rule->session;
        c->per_second = rule->per_second;
        c->burst = rule->burst;
        c->tokens = (uint64_t)(rule->burst ? rule->burst : 1u) * system->config.ticks_per_second;
        c->refilled = system->now;
        c->threshold = rule->sample_whole
                           ? ta_trace_ratio_threshold(rule->sample_parts, rule->sample_whole)
                           : ta_trace_ratio_threshold(0, 1);
        c->chart = TA_NIL;
        if (rule->chart.length) {
            c->chart = POLICY_ABSENT_CHART;
            for (uint32_t k = 0; k < system->chart_count; k++)
                if (text_equal(chart_name(core, k), rule->chart)) c->chart = k;
        }
        c->actor = actors[i];
        c->actor_prefix = rule->actor.length && rule->actor.pointer[rule->actor.length - 1] == '*';
        c->name_bit = c->actor != TA_NIL ? (uint8_t)name_bit++ : (uint8_t)POLICY_NO_NAME;
        c->event_text = texts[i];
        c->event_first = c->event_end = TA_NIL;
        ta_string_view event = rule->event;
        if (event.length >= 2 && event.pointer[event.length - 1] == '*' &&
            event.pointer[event.length - 2] == '.')
            event.length -= 2;
        else if (event.length == 1 && event.pointer[0] == '*')
            event.length = 0;
        if (event.length && texts[i] == TA_NIL) {
            int exact = 0;
            uint32_t node =
                ta_event_name_lookup(&core->image_view, event.pointer, event.length, &exact);
            c->event_first = node;
            c->event_end = core->image_view.event_nodes[node].end;
        }
        system->rule_points |= c->at;
        if (c->chart == POLICY_ABSENT_CHART) continue; /* it never matches here */
        for (uint32_t k = 0; k < system->chart_count; k++)
            if (c->chart == TA_NIL || c->chart == k) chart_points[k] |= c->at;
    }
    system->rule_count = count;
    /* the actors' name matches are for the old rules */
    for (uint32_t i = 0; i < system->actors.high_water_mark; i++)
        if (ta_pool_live(&system->actors, i))
            ta_actor_at(system, i)->flags &= (uint8_t)~ACTOR_FLAG_TRACE_NAMES;
    return TA_OK;
}
