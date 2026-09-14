#ifndef TINYACTORS_RAW_REGISTRATIONS_H
#define TINYACTORS_RAW_REGISTRATIONS_H

#include "ruby.h"

/* Each class assignment supplies one initializer.  Keeping the registry in
 * one header makes load order explicit and makes duplicate registration
 * auditable without inventing exports for unfinished wrappers. */
void tar_init_support(VALUE raw);

#endif
