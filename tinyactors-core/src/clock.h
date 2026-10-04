#pragma once

#include "../include/tinyactors.h"

typedef struct {
    ta_time now;
} ta_clock;

static inline ta_time ta_clock_now(const ta_clock *clock) { return clock->now; }
static inline void ta_clock_set(ta_clock *clock, ta_time now) { clock->now = now; }
