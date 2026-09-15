#include "../src/datamodel.h"
#include "../src/snapshot.h"
#include "../include/tinyactors.h"

static void use_public_types(void) {
    ta_datamodel datamodel = {0};
    ta_snapshot_batch *batch = 0;
    ta_actor_spawn_params spawn = {0};

    (void)datamodel;
    (void)batch;
    (void)spawn;
}

int main(void) {
    use_public_types();
    return 0;
}
