#pragma once

#include "configuration.h"
#include "event.h"
#include "fault.h"

typedef struct {
    ta_system *system;
    ta_actor_id actor;
    const ta_event *current_event;
    const ta_configuration *configuration;
} ta_dm_host_impl;

struct ta_dm_host {
    const ta_dm_host_impl *impl;
};
