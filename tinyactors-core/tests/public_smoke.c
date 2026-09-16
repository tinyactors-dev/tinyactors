#include "../src/datamodel.h"
#include "../src/snapshot.h"
#include "../include/tinyactors.h"

static void use_public_types(void) {
    ta_datamodel datamodel = {0};
    ta_actor_directory directory = {0};
    ta_actor_address address = {0};
    ta_actor_spawn_params spawn = {0};

    (void)datamodel;
    (void)directory;
    (void)address;
    (void)spawn;
}

int main(void) {
    use_public_types();
    return 0;
}
