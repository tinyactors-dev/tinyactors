#include "datamodel.h"
#include "actor.h"
#include "interpreter.h"
#include "scxml_uri.h"
#include "system.h"
#include "memory.h"
#include "str.h"
#include "text_writer.h"
#include "../include/tinyactors.h"
#include "atom.h"
#include "trie.h"
#include "io.h"

bool ta_dm_host_in_state(const ta_dm_host *host, ta_state_id state) {
    if (host == NULL || host->impl == NULL || host->impl->actor == NULL) {
        return false;
    }
    return ta_actor_state_active(host->impl->actor, state);
}

const ta_event *ta_dm_host_event(const ta_dm_host *host) {
    if (host == NULL || host->impl == NULL || host->impl->actor == NULL) {
        return NULL;
    }
    return ta_actor_bound_event(host->impl->actor);
}

ta_actor_id ta_dm_host_actor(const ta_dm_host *host) {
    if (host == NULL || host->impl == NULL || host->impl->actor == NULL) {
        return 0;
    }
    return host->impl->actor->record.id;
}

static bool ta_dm_host_valid(const ta_dm_host *host) {
    return host != NULL && host->impl != NULL && host->impl->actor != NULL && host->impl->system != NULL;
}

ta_time ta_dm_host_time(const ta_dm_host *host) {
    if (!ta_dm_host_valid(host)) {
        return 0;
    }
    return ta_interpreter_macrostep_time(host->impl->system, host->impl->actor);
}

/* The render area of the interpreter run that is calling the data model
 * (ADR 0068): rendered text is content-sized (ADR 0092 section 3). */
static ta_allocator ta_dm_arena(const ta_dm_host *host) {
    return ta_arena_allocator(&host->impl->system->render);
}

typedef struct {
    const ta_dm_host *host;
    ta_dm_text_kind kind;
} ta_dm_field;

/* Writes the fields that core renders as text; false when the field has no value. */
static bool ta_dm_write_field(const void *context, ta_text_writer *writer) {
    const ta_dm_field *field = context;
    const ta_system *system = field->host->impl->system;
    const ta_actor *actor = field->host->impl->actor;
    if (field->kind == TA_DM_TEXT_SESSION_ID) {
        ta_session_text_write(system, writer, actor->session_id, actor->definition->identity.family.name);
        return true;
    }
    if (field->kind == TA_DM_TEXT_IO_LOCATION) {
        /* The SCXML processor's location: the address a chart can send to (ADR 0044). */
        ta_scxml_target target = { .kind = TA_SCXML_TARGET_ACTOR, .as.actor = actor->record.self };
        return ta_scxml_uri_write(system, writer, &target);
    }
    const ta_event *event = ta_dm_host_event(field->host);
    if (event == NULL) {
        return false;
    }
    if (field->kind == TA_DM_TEXT_EVENT_SENDID) {
        if (event->send_id == 0) {
            return false;
        }
        ta_send_id_write(system, event->send_id, writer);
        return true;
    }
    ta_atom segments[TA_TRIE_MAX_DEPTH];
    uint32_t count = ta_trie_path(&system->event_names, event->name, segments);
    for (uint32_t index = 0; index < count; index += 1) {
        if (index != 0) {
            ta_text_put(writer, (ta_str){ .ptr = ".", .len = 1 });
        }
        ta_text_put(writer, ta_atom_text(&system->atoms, segments[index]));
    }
    return count != 0;
}

static ta_result ta_dm_render_field(const ta_dm_host *host, ta_dm_text_kind kind, ta_str *out) {
    ta_dm_field field = { .host = host, .kind = kind };
    return ta_text_render(ta_dm_arena(host),
                          &(ta_text_render_params){ .write = ta_dm_write_field, .context = &field, .out = out });
}

static ta_address *ta_dm_address(const ta_actor *actor) {
    return actor->record.self.kind == TA_ACTOR_REF_NAMED ? actor->record.self.as.named : NULL;
}

/* An event field that is stored or rendered from the event's origin. */
static ta_result ta_dm_event_text(const ta_dm_host *host, ta_dm_text_kind kind, ta_str *out) {
    const ta_system *system = host->impl->system;
    const ta_actor *actor = host->impl->actor;
    const ta_event *event = ta_dm_host_event(host);
    if (event == NULL) {
        return TA_ERR_NOT_FOUND;
    }
    switch (kind) {
        case TA_DM_TEXT_EVENT_TYPE: {
            static const char *const types[] = { "external", "internal", "platform" };
            if ((uint32_t)event->type >= TA_ARRAY_COUNT(types)) {
                return TA_ERR_NOT_FOUND;
            }
            *out = ta_str_from_cstr(types[event->type]);
            return TA_OK;
        }
        case TA_DM_TEXT_EVENT_ORIGIN:
            return ta_io_origin_uri(system, &(ta_io_origin_render_params){
                                                .origin = &event->origin,
                                                .session_id = actor->session_id,
                                                .address = ta_dm_address(actor),
                                                .arena = ta_dm_arena(host),
                                                .out = out,
                                            });
        case TA_DM_TEXT_EVENT_ORIGINTYPE:
            /*
             * A closed scope keeps its type: a reply then reaches the
             * processor with an empty target, which the processor refuses,
             * so the sender sees error.communication (ADR 0068).
             */
            return ta_io_origin_type(system, &event->origin, out);
        case TA_DM_TEXT_EVENT_INVOKEID:
            if (event->invoke_id == TA_INVOKE_ID_NONE) {
                return TA_ERR_NOT_FOUND;
            }
            /* The SCXML ID text, e.g. the author's ID or `<stateid>.<platformid>`. */
            *out = ta_invoke_id_text(system, event->invoke_id);
            return TA_OK;
        default:
            return ta_dm_render_field(host, kind, out);
    }
}

/* One entry of `_ioprocessors`: its type, or this session's location through it. */
static ta_result ta_dm_io_text(const ta_dm_host *host, const ta_dm_text_params *params) {
    const ta_system *system = host->impl->system;
    const ta_actor *actor = host->impl->actor;
    if (params->io_index == 0) {
        if (params->kind == TA_DM_TEXT_IO_TYPE) {
            *params->out = ta_atom_text(&system->atoms, system->scxml_io_type);
            return TA_OK;
        }
        return ta_dm_render_field(host, TA_DM_TEXT_IO_LOCATION, params->out);
    }
    const ta_io_registration *registration = ta_system_io_at(system, params->io_index);
    if (registration == NULL) {
        return TA_ERR_NOT_FOUND;
    }
    if (params->kind == TA_DM_TEXT_IO_TYPE) {
        *params->out = ta_atom_text(&system->atoms, registration->type);
        return TA_OK;
    }
    if (registration->io.v->text == NULL) {
        return TA_ERR_NOT_FOUND;
    }
    return registration->io.v->text(registration->io.self, &(ta_io_text_params){
                                                               .kind = TA_IO_TEXT_LOCATION,
                                                               .session_id = actor->session_id,
                                                               .address = ta_dm_address(actor),
                                                               .arena = ta_dm_arena(host),
                                                               .out = params->out,
                                                           });
}

ta_result ta_dm_host_text(const ta_dm_host *host, const ta_dm_text_params *params) {
    if (!ta_dm_host_valid(host) || params == NULL || params->out == NULL) {
        return TA_ERR_INVALID;
    }
    *params->out = (ta_str){0};
    const ta_system *system = host->impl->system;
    const ta_actor *actor = host->impl->actor;
    ta_result result;
    switch (params->kind) {
        case TA_DM_TEXT_SESSION_ID:
            result = ta_dm_render_field(host, params->kind, params->out);
            break;
        case TA_DM_TEXT_SESSION_NAME:
            if (actor->definition->document_name == TA_ATOM_NONE) {
                return TA_ERR_NOT_FOUND;
            }
            *params->out = ta_atom_text(&system->atoms, actor->definition->document_name);
            return TA_OK;
        case TA_DM_TEXT_IO_TYPE:
        case TA_DM_TEXT_IO_LOCATION:
            result = ta_dm_io_text(host, params);
            break;
        default:
            result = ta_dm_event_text(host, params->kind, params->out);
            break;
    }
    if (result != TA_OK) {
        *params->out = (ta_str){0};
    }
    return result;
}

ta_result ta_dm_host_state_name(const ta_dm_host *host, ta_state_id state, ta_str *out) {
    if (!ta_dm_host_valid(host) || out == NULL) {
        return TA_ERR_INVALID;
    }
    const ta_definition *definition = host->impl->actor->definition;
    if (state >= definition->state_count) {
        return TA_ERR_NOT_FOUND;
    }
    *out = ta_atom_text(&host->impl->system->atoms, definition->states[state].name);
    return TA_OK;
}

ta_result ta_dm_host_find_state(const ta_dm_host *host, ta_str id, ta_state_id *out) {
    if (!ta_dm_host_valid(host) || out == NULL) {
        return TA_ERR_INVALID;
    }
    const ta_system *system = host->impl->system;
    const ta_definition *definition = host->impl->actor->definition;
    ta_atom name = 0;
    if (ta_atom_find(&system->atoms, id, &name) != TA_OK) {
        return TA_ERR_NOT_FOUND;
    }
    for (ta_state_id state = 0; state < definition->state_count; state += 1) {
        if (definition->states[state].name == name) {
            *out = state;
            return TA_OK;
        }
    }
    return TA_ERR_NOT_FOUND;
}
