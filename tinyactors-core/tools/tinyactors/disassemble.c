#include "tool.h"

void tool_disassemble(tool *t) {
    tool_complain(t, ta_string("disassemble: not built yet"));
    t->exit = TOOL_EXIT_USAGE;
}
