"use strict";
// Development-only check: npm exec --package=acorn@8.15.0 -- node
// bindings/napi/scripts/check-arity.js. Clang must also be on PATH.
const { spawnSync } = require("node:child_process");
const { readdirSync, existsSync } = require("node:fs");
const path = require("node:path");
const binding = path.resolve(__dirname, "..");
const include = [
  process.env.NODE_INCLUDE_DIR,
  path.resolve(process.execPath, "../../include/node"),
  "/usr/include/node",
  "/usr/local/include/node",
].find((dir) => dir && existsSync(path.join(dir, "node_api.h")));
if (!include)
  throw new Error("Set NODE_INCLUDE_DIR to Node development headers.");
const failures = [];
let functions = 0;
function run(command, args) {
  const result = spawnSync(command, args, {
    encoding: "utf8",
    maxBuffer: 64 * 1024 * 1024,
  });
  if (result.error) throw result.error;
  if (result.status) throw new Error(result.stderr || command + " failed");
  return result.stdout;
}
function check(node, file) {
  if (node.params) {
    functions++;
    if (node.params.length > 3)
      failures.push(
        file +
          ": " +
          node.loc.start.line +
          " has " +
          node.params.length +
          " parameters",
      );
  }
  if (
    [
      "FunctionDecl",
      "CXXMethodDecl",
      "CXXConstructorDecl",
      "CXXConversionDecl",
    ].includes(node.kind) &&
    node.inner?.some((child) => child.kind === "CompoundStmt")
  ) {
    functions++;
    const count = node.inner.filter(
      (child) => child.kind === "ParmVarDecl",
    ).length;
    if (count > 3)
      failures.push(file + ": " + node.name + " has " + count + " parameters");
  }
  for (const value of Object.values(node)) {
    if (Array.isArray(value)) {
      for (const child of value)
        if (child && typeof child === "object") check(child, file);
    } else if (value && typeof value === "object") check(value, file);
  }
}
function visit(directory) {
  for (const entry of readdirSync(directory, { withFileTypes: true })) {
    if (entry.name === "build" || entry.name === "node_modules") continue;
    const file = path.join(directory, entry.name);
    if (entry.isDirectory()) visit(file);
    else if (/\.(m?js)$/.test(file))
      check(
        JSON.parse(
          run("acorn", ["--ecma2024", "--module", "--locations", file]),
        ),
        file,
      );
    else if (file.endsWith(".cc")) {
      const output = run("clang++", [
        "-std=c++17",
        "-DNAPI_VERSION=8",
        "-I" + include,
        "-Xclang",
        "-ast-dump=json",
        "-Xclang",
        "-ast-dump-filter=tn",
        "-fsyntax-only",
        file,
      ]);
      for (const document of output.trim().split(/\n(?=\{)/))
        check(JSON.parse(document), file);
    }
  }
}
visit(binding);
if (failures.length) throw new Error(failures.join("\n"));
console.log(
  "All " +
    functions +
    " parsed C++/JS function definitions take at most three parameters.",
);
