"use strict";
const ll = require("../lowlevel");
const send = (event, options = {}) =>
  Object.freeze({ ...options, kind: "send", event });
const raise = (event) => Object.freeze({ kind: "raise", event });
const cancel = (sendid) => Object.freeze({ kind: "cancel", sendid });
const assign = (patch) => Object.freeze({ kind: "assign", patch });
function sendTarget(value) {
  if (value == null || value === "self") return { kind: ll.TA_TARGET_SELF };
  if (typeof value === "bigint")
    return { kind: ll.TA_TARGET_INSTANCE, instance: value };
  if (typeof value === "object" && typeof value.kind === "number") return value;
  if (typeof value !== "string")
    throw new TypeError("A target must be a name, URI, BigInt, or descriptor.");
  if (value === "internal" || value === "#_internal")
    return { kind: ll.TA_TARGET_INTERNAL };
  if (value === "parent" || value === "#_parent")
    return { kind: ll.TA_TARGET_PARENT };
  return {
    kind:
      value.startsWith("#") || value.includes(":")
        ? ll.TA_TARGET_URI
        : ll.TA_TARGET_NAME,
    text: value,
  };
}
function delayNs(options) {
  if (options.delayNs !== undefined) return options.delayNs;
  if (options.delayMs !== undefined) {
    if (!Number.isSafeInteger(options.delayMs) || options.delayMs < 0)
      throw new RangeError("delayMs must be a nonnegative safe integer.");
    return BigInt(options.delayMs) * 1000000n;
  }
  return 0n;
}
module.exports = { send, raise, cancel, assign };
Object.defineProperties(module.exports, {
  sendTarget: { value: sendTarget },
  delayNs: { value: delayNs },
});
