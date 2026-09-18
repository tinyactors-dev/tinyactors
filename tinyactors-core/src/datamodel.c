#include "datamodel.h"
#include "actor.h"
#include "interpreter.h"

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

/* "#_scxml_<session>", the SCXML Event I/O Processor address of a runtime actor. */
static void ta_dm_write_session_uri(ta_text_writer *writer, ta_actor_id actor) {
    ta_text_put(writer, ta_str_from_cstr("#_scxml_"));
    ta_text_put_u64(writer, actor);
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
            if (event->origin.kind == TA_ORIGIN_ACTOR && event->origin.as.actor.kind == TA_ACTOR_REF_RUNTIME) {
                ta_dm_write_session_uri(writer, event->origin.as.actor.as.runtime);
                return true;
            }
            if (event->origin.kind == TA_ORIGIN_INVOKE) {
                ta_text_put(writer, ta_str_from_cstr("#_"));
                ta_text_put_u64(writer, event->origin.as.invoke);
                return true;
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
            ta_text_put_u64(writer, event->invoke_id);
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
            ta_text_put_u64(&writer, actor->record.id);
            break;
        case TA_DM_TEXT_SESSION_NAME:
            present = actor->definition->document_name != TA_ATOM_NONE;
            if (present) {
                ta_text_put(&writer, ta_atom_text(&system->atoms, actor->definition->document_name));
            }
            break;
        case TA_DM_TEXT_SCXML_IO_LOCATION:
            ta_dm_write_session_uri(&writer, actor->record.id);
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
    ta_atom atom = 0;
    if (ta_atom_find(&host->impl->system->atoms, id, &atom) != TA_OK) {
        return TA_ERR_NOT_FOUND;
    }
    const ta_definition *definition = host->impl->actor->definition;
    for (ta_state_id state = 0; state < definition->state_count; state += 1) {
        if (definition->states[state].name == atom) {
            *out = state;
            return TA_OK;
        }
    }
    return TA_ERR_NOT_FOUND;
}

ta_result ta_dm_host_atom_text(const ta_dm_host *host, ta_atom atom, ta_str *out) {
    if (!ta_dm_host_valid(host) || out == NULL) {
        return TA_ERR_INVALID;
    }
    if (!ta_atom_valid(&host->impl->system->atoms, atom)) {
        return TA_ERR_NOT_FOUND;
    }
    *out = ta_atom_text(&host->impl->system->atoms, atom);
    return TA_OK;
}
