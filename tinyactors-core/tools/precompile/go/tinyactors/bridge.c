// The C half of the bridge: the datamodel whose one function is the Go
// function taGoCall (exported by bridge.go), and the shared demo driver.
#include <tinyactors/datamodel_precompiled.h>
#include <tinyactors/runtime.h>

#include "_cgo_export.h"
#include "../../demo/demo.c"

static ta_status ta_go_function(void *context, ta_precompiled_request *request) {
    (void)context;
    return (ta_status)taGoCall(request);
}

ta_status ta_go_datamodel(ta_datamodel_precompiled **out) {
    ta_datamodel_precompiled_config config = {sizeof config, 0, "go", ta_go_function, 0, ta_runtime_malloc()};
    return ta_datamodel_precompiled_new(&config, out);
}
