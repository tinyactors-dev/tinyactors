"use strict";
/* Zero-dependency build for a source checkout. NODE_INCLUDE_DIR is a directory
 * containing node_api.h; no Node internals or C++ engine headers are used. */
const { spawnSync } = require("node:child_process");
const { existsSync, mkdirSync, readdirSync } = require("node:fs");
const path = require("node:path");
const root = path.resolve(__dirname, "../../..");
const binding = path.resolve(__dirname, "..");
const build = path.join(binding, "build/Release");
const candidates = [
  process.env.NODE_INCLUDE_DIR,
  path.resolve(process.execPath, "../../include/node"),
  "/usr/include/node",
  "/usr/local/include/node",
];
const include = candidates.find(
  (dir) => dir && existsSync(path.join(dir, "node_api.h")),
);
if (!include)
  throw new Error(
    "Node headers not found. Set NODE_INCLUDE_DIR to the directory containing node_api.h.",
  );
if (process.platform === "win32")
  throw new Error("Use node-gyp rebuild with binding.gyp on Windows.");
mkdirSync(build, { recursive: true });
const cc = process.env.CC || "cc",
  cxx = process.env.CXX || "c++";
const instrumentation =
  process.env.TINYACTORS_SANITIZE === "undefined"
    ? ["-fsanitize=undefined", "-fno-sanitize-recover=all"]
    : [];
const objects = [];
function run(command, args) {
  const result = spawnSync(command, args, { stdio: "inherit" });
  if (result.error) throw result.error;
  if (result.status !== 0) process.exit(result.status || 1);
}
function compile(file, cpp) {
  const object = path.join(build, path.basename(file) + ".o");
  objects.push(object);
  const flags = cpp
    ? ["-std=c++17", "-DNAPI_VERSION=8", "-I" + include]
    : ["-std=c99", "-ffreestanding"];
  run(cpp ? cxx : cc, [
    ...flags,
    ...instrumentation,
    "-fPIC",
    "-O2",
    "-g",
    "-Wall",
    "-Wextra",
    "-Werror",
    "-c",
    file,
    "-o",
    object,
  ]);
}
for (const file of readdirSync(path.join(root, "src")).sort()) {
  if (file.endsWith(".c") && file !== "compiler_runtime.c")
    compile(path.join(root, "src", file), false);
}
for (const file of [
  "bridge.cc",
  "structures.cc",
  "callbacks.cc",
  "telemetry.cc",
  "addon.cc",
])
  compile(path.join(binding, "native", file), true);
run(cxx, [
  ...(process.platform === "darwin"
    ? ["-bundle", "-undefined", "dynamic_lookup"]
    : ["-shared"]),
  ...instrumentation,
  ...objects,
  "-o",
  path.join(build, "tinyactors.node"),
]);
console.log("Built bindings/napi/build/Release/tinyactors.node (Node-API 8)");
