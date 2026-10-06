/* event_names.c — resolve a dynamic event name to its event-trie node.
 *
 * Called once per *created* event with a non-static name (eventexpr,
 * host-injected events, done.invoke.<generated id>). Matching afterwards is
 * an integer interval test: descriptor [lo, hi) matches event node e iff
 * lo <= e < hi.
 */
#include "image/records.h"
#include "base/memory.h"

uint32_t ta_event_name_lookup(const ta_image_view *view, const char *name, uint32_t length,
                              int *exact) {
    uint32_t node = 0, position = 0;
    int ex = length == 0;
    while (position < length) {
        uint32_t tend = position;
        while (tend < length && name[tend] != '.')
            tend++;
        uint32_t tlen = tend - position, found = TA_NIL;
        for (uint32_t child = node + 1; child < view->event_nodes[node].end;
             child = view->event_nodes[child].end) {
            ta_string_view tok = ta_image_view_string(view, view->event_nodes[child].token);
            if (tok.length == tlen && ta_memcmp(tok.pointer, name + position, tlen) == 0) {
                found = child;
                break;
            }
        }
        if (found == TA_NIL) break;
        node = found;
        if (tend == length) {
            ex = 1;
            break;
        }
        position = tend + 1;
        if (position == length) break; /* trailing '.' : not exact */
    }
    if (exact) *exact = ex;
    return node;
}
