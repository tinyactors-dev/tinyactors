// Exercise the wasm value seam independently of actor semantics. The fake C
// caller uses the same pointer/out-parameter ABI as web/src/node_api.h.
import test from 'node:test';
import assert from 'node:assert/strict';
import { createNative } from '../../bindings/web/native.mjs';

function harness(methods) {
  const memory = new WebAssembly.Memory({ initial: 1 });
  let api, top = 1024;
  const view = () => new DataView(memory.buffer);
  const read = (p) => view().getUint32(p, true);
  const write = (p, n) => view().setUint32(p, n, true);
  function alloc(n = 16) {
    const p = top;
    top += Math.max(8, (n + 7) & ~7);
    while (top > memory.buffer.byteLength) memory.grow(1);
    return p;
  }
  function text(s) {
    const b = new TextEncoder().encode(s), p = alloc(b.length + 1);
    new Uint8Array(memory.buffer).set(b, p);
    return p;
  }
  function call(name, ...args) { return api[`napi_${name}`](0, ...args); }
  function value(name, ...args) {
    const out = alloc();
    assert.equal(call(name, ...args, out), 0);
    return read(out);
  }
  function args(info, n) {
    const count = alloc(), out = alloc(4 * n);
    write(count, n);
    assert.equal(call('get_cb_info', info, count, out, 0, 0), 0);
    return Array.from({ length: n }, (_, i) => read(out + 4 * i));
  }
  const c = { memory, alloc, read, write, text, call, value, args };
  const entries = Object.entries(methods);
  const freed = [];
  const native = createNative(({ napi }) => {
    api = napi;
    return { exports: {
      memory, w_alloc: alloc, w_free: (p) => freed.push(p),
      w_init(exports) {
        entries.forEach(([name], i) => {
          const fn = value('create_function', text(name), -1, i, 0);
          call('set_named_property', exports, text(name), fn);
        });
        return exports;
      },
      w_call: (cb, info) => entries[cb][1](c, info),
    } };
  });
  return { native, c, freed };
}

test('adapter scopes: nested callbacks, persistent references, and exception identity', () => {
  let ref;
  const { native } = harness({
    hold(c, info) {
      const [v] = c.args(info, 1);
      ref = c.value('create_reference', v, 1);
      return v;
    },
    get(c) { return c.value('get_reference_value', ref); },
    release(c) { c.call('delete_reference', ref); return 0; },
    echo(c, info) { return c.args(info, 1)[0]; },
    invoke(c, info) {
      const [fn, v] = c.args(info, 2), argv = c.alloc(), out = c.alloc();
      c.write(argv, v);
      const status = c.call('call_function', c.value('get_undefined'), fn, 1, argv, out);
      if (status === 10) return c.value('get_and_clear_last_exception');
      assert.equal(status, 0);
      return c.read(out);
    },
  });
  const object = { snow: '雪☃\0é', n: 2 ** 40 + 7 };
  assert.equal(native.hold(object), object);
  for (const v of [undefined, null, false, 0, -3.5, 2 ** 40 + 7, '雪\0é', Symbol('x'), 13n])
    assert.equal(native.invoke((x) => native.echo(x), v), v);
  assert.equal(native.get(), object);
  const error = new Error('exact identity');
  assert.equal(native.invoke(() => { throw error; }), error);
  assert.equal(native.invoke(() => { throw undefined; }), undefined);
  assert.equal(native.invoke(() => 19), 19);
  native.release();
});

test('adapter bytes: offsets, C-to-JS visibility, growth and JS-to-C writes', () => {
  const { native, freed } = harness({
    bytes(c, info) {
      const [input, check] = c.args(info, 2);
      const type = c.alloc(), len = c.alloc(), ptr = c.alloc(), off = c.alloc();
      assert.equal(c.call('get_typedarray_info', input, type, len, ptr, 0, off), 0);
      assert.equal(c.read(off), 3);
      assert.equal(c.read(len), 4);
      const source = c.read(ptr);
      assert.deepEqual([...new Uint8Array(c.memory.buffer, source, 4)], [3, 17, 91, 255]);
      const data = c.alloc(), ab = c.value('create_arraybuffer', 70001, data);
      c.memory.grow(2); // invalidate all cached views before writing C's result
      const p = c.read(data);
      new Uint8Array(c.memory.buffer).set([17, 91, 255], p + 69998);
      const out = c.value('create_typedarray', 1, 3, ab, 69998);
      const argv = c.alloc();
      c.write(argv, out);
      assert.equal(c.call('call_function', c.value('get_undefined'), check, 1, argv, 0), 0);
      assert.equal(new Uint8Array(c.memory.buffer)[p + 69999], 42);
      return out;
    },
  });
  const backing = new Uint8Array([88, 77, 66, 3, 17, 91, 255, 99]);
  const out = native.bytes(backing.subarray(3, 7), (b) => {
    assert.deepEqual([...b], [17, 91, 255]);
    b[1] = 42;
  });
  assert.deepEqual([...out], [17, 42, 255]);
  assert.deepEqual([...backing], [88, 77, 66, 3, 17, 91, 255, 99]);
  assert.equal(freed.length, 2, 'both temporary mirrors released');
});

test('adapter strings: byte lengths, embedded NUL and whole UTF-8 truncation', () => {
  const { native } = harness({
    roundtrip(c, info) {
      const [v] = c.args(info, 1), length = c.alloc(), buf = c.alloc();
      assert.equal(c.call('get_value_string_utf8', v, 0, 0, length), 0);
      assert.equal(c.read(length), 9);
      assert.equal(c.call('get_value_string_utf8', v, buf, 6, length), 0);
      assert.equal(c.read(length), 5); // 'é\0é', without half of the final 4-byte character
      assert.equal(new Uint8Array(c.memory.buffer)[buf + 5], 0);
      return c.value('create_string_utf8', buf, 5);
    },
  });
  assert.equal(native.roundtrip('é\0é😀'), 'é\0é');
});
