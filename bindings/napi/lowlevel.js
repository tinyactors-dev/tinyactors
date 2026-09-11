"use strict";
try {
  module.exports = require("./build/Release/tinyactors.node");
} catch (error) {
  if (error.code === "MODULE_NOT_FOUND") {
    error.message =
      "Build the tinyactors Node-API addon first: npm --prefix bindings/napi run build\n" +
      error.message;
  }
  throw error;
}
