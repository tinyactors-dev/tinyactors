#include "tool.h"

void tool_graph(tool *t) {
    tool_complain(t, ta_string("graph: not built yet"));
    t->exit = TOOL_EXIT_USAGE;
}
