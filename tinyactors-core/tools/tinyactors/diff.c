#include "tool.h"

void tool_diff(tool *t) {
    tool_complain(t, ta_string("diff: not built yet"));
    t->exit = TOOL_EXIT_USAGE;
}
