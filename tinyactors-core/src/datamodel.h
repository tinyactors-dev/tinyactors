#pragma once

#include "configuration.h"
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

bool ta_dm_host_in_state(const ta_dm_host *host, ta_state_id state);
const ta_event *ta_dm_host_event(const ta_dm_host *host);
ta_actor_id ta_dm_host_actor(const ta_dm_host *host);
