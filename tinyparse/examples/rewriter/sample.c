/* sample.c — input for the macro-to-enum rewriter example.
 *
 * It mixes object-like integer macros (which the rewriter folds into a
 * single enum) with ordinary C (a struct and a function) that must pass
 * through unchanged.
 */
#include <stddef.h>

#define MAX_USERS 100
#define BUFFER_SIZE 4096
#define TIMEOUT_MS 250

struct session {
    int   user_id;
    char  name[BUFFER_SIZE];
};

int session_count(const struct session *sessions, size_t n)
{
    size_t i;
    int    active = 0;
    for (i = 0; i < n && i < MAX_USERS; i++) {
        if (sessions[i].user_id != 0)
            active++;
    }
    return active;
}
