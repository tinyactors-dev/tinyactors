/*
 * Private: the private module headers in dependency order.
 *
 * mise run amalgamate inlines these, in this order, after include/txml.h and
 * before every src/*.c, so a header may rely only on the ones listed above it.
 * A private header that is not listed here is not part of the amalgamation
 * and the task refuses to build one that a source includes.
 */
#pragma once

#include "text.h"
#include "doc.h"
#include "parser.h"
