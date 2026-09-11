{
  "targets": [{
    "target_name": "tinyactors",
    "defines": ["NAPI_VERSION=8"],
    "sources": [
      "native/bridge.cc", "native/structures.cc", "native/callbacks.cc", "native/telemetry.cc", "native/addon.cc",
      "../../src/actions.c", "../../src/actor.c", "../../src/actor_index.c", "../../src/arena.c",
      "../../src/chart.c", "../../src/common.c", "../../src/context.c", "../../src/definition.c",
      "../../src/definition_validate.c", "../../src/expression.c", "../../src/inspect.c",
      "../../src/interpreter.c", "../../src/invoke.c", "../../src/io.c", "../../src/message.c",
      "../../src/registry.c", "../../src/send.c", "../../src/system.c", "../../src/timer.c",
      "../../src/trace.c", "../../src/upgrade.c", "../../src/value.c"
    ],
    "cflags_c": ["-std=c99", "-ffreestanding"],
    "cflags_cc": ["-std=c++17", "-fexceptions", "-frtti"],
    "cflags_cc!": ["-fno-exceptions", "-fno-rtti"],
    "xcode_settings": {
      "CLANG_CXX_LANGUAGE_STANDARD": "c++17",
      "GCC_C_LANGUAGE_STANDARD": "c99",
      "GCC_ENABLE_CPP_EXCEPTIONS": "YES",
      "GCC_ENABLE_CPP_RTTI": "YES"
    },
    "msvs_settings": {"VCCLCompilerTool": {"ExceptionHandling": 1, "RuntimeTypeInfo": "true", "AdditionalOptions": ["/std:c++17"]}}
  }]
}
