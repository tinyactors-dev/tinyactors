#include "io.h"
#include "system.h"
#include "scxml_uri.h"
#include "text_writer.h"
#include "../include/tinyactors.h"
#include "atom.h"
#include "io_origin.h"

typedef struct {
    const ta_system *system;
    ta_scxml_target target;
} ta_io_target_text;

static bool ta_io_write_target(const void *context, ta_text_writer *writer) {
    const ta_io_target_text *text = context;
    return ta_scxml_uri_write(text->system, writer, &text->target);
}

static ta_result ta_io_render_target(const ta_system *system, ta_scxml_target target, ta_allocator arena,
                                     ta_str *out) {
    ta_io_target_text text = { .system = system, .target = target };
    return ta_text_render(arena, &(ta_text_render_params){ .write = ta_io_write_target, .context = &text, .out = out });
}

/* The live scope a SCOPED origin names, or null once it closed. */
static const ta_io_origin_entry *ta_io_scope_record(const ta_system *system, const ta_io_origin *io) {
    const ta_io_origin_entry *record = ta_io_origin_at(&system->io_scopes, io->as.scope.index);
    if (record == NULL || record->generation != io->as.scope.generation || record->processor != io->processor) {
        return NULL;
    }
    return record;
}

ta_result ta_io_origin_uri(const ta_system *system, const ta_io_origin_render_params *params) {
    const ta_origin *origin = params->origin;
    switch (origin->kind) {
        case TA_ORIGIN_ACTOR:
            return ta_io_render_target(system,
                                       (ta_scxml_target){ .kind = TA_SCXML_TARGET_ACTOR, .as.actor = origin->as.actor },
                                       params->arena, params->out);
        case TA_ORIGIN_INVOKE:
            return ta_io_render_target(
                system, (ta_scxml_target){ .kind = TA_SCXML_TARGET_INVOKE, .as.invoke = origin->as.invoke.invoke },
                params->arena, params->out);
        case TA_ORIGIN_IO:
            break;
        default:
            return TA_ERR_NOT_FOUND;
    }
    const ta_io_origin *io = &origin->as.io;
    switch (io->origin_class) {
        case TA_IO_ORIGIN_COMPUTED: {
            const ta_io_registration *registration = ta_system_io_at(system, io->processor);
            if (registration == NULL || registration->io.v->text == NULL) {
                return TA_ERR_NOT_FOUND;
            }
            return registration->io.v->text(registration->io.self, &(ta_io_text_params){
                                                                       .kind = TA_IO_TEXT_ORIGIN,
                                                                       .session_id = params->session_id,
                                                                       .address = params->address,
                                                                       .token = io->as.token,
                                                                       .arena = params->arena,
                                                                       .out = params->out,
                                                                   });
        }
        case TA_IO_ORIGIN_SCOPED: {
            const ta_io_origin_entry *record = ta_io_scope_record(system, io);
            if (record == NULL) {
                return TA_ERR_NOT_FOUND;
            }
            *params->out = ta_io_origin_entry_uri(record);
            return TA_OK;
        }
        case TA_IO_ORIGIN_MESSAGE:
            if (io->as.entry == NULL) {
                return TA_ERR_NOT_FOUND;
            }
            *params->out = ta_io_origin_entry_uri(io->as.entry);
            return TA_OK;
        default:
            return TA_ERR_NOT_FOUND;
    }
}

ta_result ta_io_origin_type(const ta_system *system, const ta_origin *origin, ta_str *out) {
    ta_atom type = TA_ATOM_NONE;
    switch (origin->kind) {
        case TA_ORIGIN_ACTOR:
            type = system->scxml_io_type;
            break;
        case TA_ORIGIN_INVOKE: {
            /*
             * The processor that reaches `#_<invokeid>`: the SCXML one for an
             * SCXML child session, otherwise the invocation's own type, which
             * SCXML 6.4 uses for sends to a non-SCXML service.
             */
            ta_atom invoker = ta_system_invoker_type(system, origin->as.invoke.invoker);
            type = invoker == TA_ATOM_NONE || invoker == system->scxml_invoke_type ? system->scxml_io_type : invoker;
            break;
        }
        case TA_ORIGIN_IO: {
            const ta_io_origin_entry *entry = origin->as.io.origin_class == TA_IO_ORIGIN_MESSAGE ? origin->as.io.as.entry
                                                                                                 : NULL;
            if (ta_io_origin_entry_detached(entry)) {
                *out = ta_io_origin_detached_type(entry);
                return TA_OK;
            }
            const ta_io_registration *registration = ta_system_io_at(system, origin->as.io.processor);
            if (registration == NULL) {
                return TA_ERR_NOT_FOUND;
            }
            type = registration->type;
            break;
        }
        default:
            return TA_ERR_NOT_FOUND;
    }
    *out = ta_atom_text(&system->atoms, type);
    return TA_OK;
}

ta_result ta_io_origin_make(ta_system *system, const ta_io_origin_make_params *params) {
    if (system == NULL || params == NULL || params->out == NULL) {
        return TA_ERR_INVALID;
    }
    *params->out = (ta_origin){ .kind = TA_ORIGIN_NONE };
    const ta_io_registration *registration = ta_system_io_at(system, params->processor);
    if (registration == NULL) {
        return TA_ERR_NOT_FOUND;
    }
    ta_io_origin io = { .processor = params->processor, .origin_class = params->origin_class };
    switch (params->origin_class) {
        case TA_IO_ORIGIN_COMPUTED:
            if (registration->io.v->text == NULL) {
                return TA_ERR_UNSUPPORTED;
            }
            io.as.token = params->token;
            break;
        case TA_IO_ORIGIN_SCOPED:
            io.as.scope = params->scope;
            if (ta_io_scope_record(system, &io) == NULL) {
                return TA_ERR_NOT_FOUND;
            }
            break;
        case TA_IO_ORIGIN_MESSAGE: {
            if (params->uri.ptr == NULL && params->uri.len != 0) {
                return TA_ERR_INVALID;
            }
            io.as.entry = ta_io_origin_take(&system->io_messages,
                                            &(ta_io_origin_take_params){ .processor = params->processor,
                                                                         .uri = params->uri });
            if (io.as.entry == NULL) {
                return TA_ERR_NOMEM;
            }
            break;
        }
        default:
            return TA_ERR_INVALID;
    }
    *params->out = (ta_origin){ .kind = TA_ORIGIN_IO, .as.io = io };
    return TA_OK;
}

ta_result ta_io_scope_open(ta_system *system, const ta_io_scope_open_params *params) {
    if (system == NULL || params == NULL || params->out == NULL ||
        (params->uri.ptr == NULL && params->uri.len != 0)) {
        return TA_ERR_INVALID;
    }
    if (ta_system_io_at(system, params->processor) == NULL) {
        return TA_ERR_NOT_FOUND;
    }
    uint32_t index = 0;
    ta_io_origin_entry *record = ta_io_origin_take(
        &system->io_scopes,
        &(ta_io_origin_take_params){ .processor = params->processor, .uri = params->uri, .index = &index });
    if (record == NULL) {
        return TA_ERR_NOMEM;
    }
    *params->out = (ta_io_scope){ .index = index, .generation = record->generation };
    return TA_OK;
}

void ta_io_scope_close(ta_system *system, ta_io_scope scope) {
    if (system == NULL) {
        return;
    }
    ta_io_origin_entry *record = ta_io_origin_at(&system->io_scopes, scope.index);
    if (record != NULL && record->generation == scope.generation) {
        ta_io_origin_give(record);
    }
}

ta_result ta_origin_text(const ta_system *system, const ta_origin_text_params *params) {
    if (system == NULL || params == NULL || params->origin == NULL || params->type == NULL ||
        params->uri == NULL) {
        return TA_ERR_INVALID;
    }
    *params->type = (ta_str){0};
    *params->uri = (ta_str){0};
    ta_result result = ta_io_origin_type(system, params->origin, params->type);
    if (result != TA_OK) {
        return result;
    }
    return ta_io_origin_uri(system, &(ta_io_origin_render_params){
                                        .origin = params->origin,
                                        .session_id = params->session_id,
                                        .arena = params->arena,
                                        .out = params->uri,
                                    });
}
