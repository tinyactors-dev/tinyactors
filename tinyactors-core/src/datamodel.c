#include "datamodel.h"
#include "actor.h"
#include "interpreter.h"
#include "scxml_uri.h"

bool ta_dm_host_in_state(const ta_dm_host *host, ta_state_id state) {
    if (host == NULL || host->impl == NULL || host->impl->actor == NULL) {
        return false;
    }
    return ta_actor_state_active(host->impl->actor, state);
}

const ta_event *ta_dm_host_event(const ta_dm_host *host) {
    if (host == NULL || host->impl == NULL || host->impl->actor == NULL ||
        !host->impl->actor->has_current_event) {
        return NULL;
    }
    return &host->impl->actor->current_event;
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

/*
 * The URI of an actor reference, written with the grammar core parses
 * (ADR 0044): a session URI for a runtime actor, a logical address for a
 * named one. A chart can send to whatever it reads here.
 */
static bool ta_dm_write_actor_uri(const ta_system *system, ta_text_writer *writer, ta_actor_ref actor) {
    ta_scxml_target target = { .kind = TA_SCXML_TARGET_ACTOR, .as.actor = actor };
    return ta_scxml_uri_write(system, writer, &target);
}

/* Writes one event field; false when the field has no value. */
static bool ta_dm_write_event_field(const ta_dm_host *host, ta_dm_text_kind kind, ta_text_writer *writer) {
    const ta_system *system = host->impl->system;
    const ta_event *event = ta_dm_host_event(host);
    if (event == NULL) {
        return false;
    }
    switch (kind) {
        case TA_DM_TEXT_EVENT_NAME: {
            ta_atom segments[TA_EVENT_NAME_MAX_DEPTH];
            uint32_t count = ta_event_name_segments(&system->event_names, event->name, segments);
            for (uint32_t index = 0; index < count; index += 1) {
                if (index != 0) {
                    ta_text_put(writer, (ta_str){ .ptr = ".", .len = 1 });
                }
                ta_text_put(writer, ta_atom_text(&system->atoms, segments[index]));
            }
            return count != 0;
        }
        case TA_DM_TEXT_EVENT_TYPE: {
            static const char *const types[] = { "external", "internal", "platform" };
            if ((uint32_t)event->type >= TA_ARRAY_COUNT(types)) {
                return false;
            }
            ta_text_put(writer, ta_str_from_cstr(types[event->type]));
            return true;
        }
        case TA_DM_TEXT_EVENT_SENDID:
            if (event->send_id == 0) {
                return false;
            }
            ta_send_id_write(system, event->send_id, writer);
            return true;
        case TA_DM_TEXT_EVENT_ORIGIN:
            if (event->origin.kind == TA_ORIGIN_ACTOR) {
                return ta_dm_write_actor_uri(system, writer, event->origin.as.actor);
            }
            if (event->origin.kind == TA_ORIGIN_INVOKE) {
                ta_scxml_target target = { .kind = TA_SCXML_TARGET_INVOKE, .as.invoke = event->origin.as.invoke };
                return ta_scxml_uri_write(system, writer, &target);
            }
            return false;
        case TA_DM_TEXT_EVENT_ORIGINTYPE:
            if (event->origin.kind != TA_ORIGIN_ACTOR && event->origin.kind != TA_ORIGIN_INVOKE) {
                return false;
            }
            ta_text_put(writer, ta_atom_text(&system->atoms, system->scxml_io_type));
            return true;
        case TA_DM_TEXT_EVENT_INVOKEID:
            if (event->invoke_id == TA_INVOKE_ID_NONE) {
                return false;
            }
            /* The SCXML ID text, e.g. the author's ID or `<stateid>.<platformid>`. */
            ta_text_put(writer, ta_invoke_id_text(system, event->invoke_id));
            return true;
        default:
            return false;
    }
}

ta_result ta_dm_host_text(const ta_dm_host *host, const ta_dm_text_params *params) {
    if (!ta_dm_host_valid(host) || params == NULL || params->needed == NULL) {
        return TA_ERR_INVALID;
    }
    const ta_system *system = host->impl->system;
    const ta_actor *actor = host->impl->actor;
    ta_text_writer writer = { .buffer = params->buffer, .capacity = params->capacity };
    bool present = true;
    switch (params->kind) {
        case TA_DM_TEXT_SESSION_ID:
            ta_text_put_u64(&writer, actor->session_id);
            break;
        case TA_DM_TEXT_SESSION_NAME:
            present = actor->definition->document_name != TA_ATOM_NONE;
            if (present) {
                ta_text_put(&writer, ta_atom_text(&system->atoms, actor->definition->document_name));
            }
            break;
        case TA_DM_TEXT_SCXML_IO_LOCATION:
            present = ta_dm_write_actor_uri(system, &writer, actor->record.self);
            break;
        case TA_DM_TEXT_SCXML_IO_TYPE:
            ta_text_put(&writer, ta_atom_text(&system->atoms, system->scxml_io_type));
            break;
        default:
            present = ta_dm_write_event_field(host, params->kind, &writer);
            break;
    }
    if (!present) {
        *params->needed = 0;
        return TA_ERR_NOT_FOUND;
    }
    return ta_text_finish(&writer, params->needed);
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
