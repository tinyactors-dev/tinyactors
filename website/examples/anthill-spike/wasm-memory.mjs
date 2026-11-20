// Captures the module's linear memory, which the package keeps private, so a
// measurement can report its size. Import before @tinyactors/wasm.
export const captured = { memory: null };
const Instance = WebAssembly.Instance;
WebAssembly.Instance = function (module, imports) {
  const instance = new Instance(module, imports);
  if (instance.exports.memory) captured.memory = instance.exports.memory;
  return instance;
};
WebAssembly.Instance.prototype = Instance.prototype;
export const wasmBytes = () => (captured.memory ? captured.memory.buffer.byteLength : NaN);
