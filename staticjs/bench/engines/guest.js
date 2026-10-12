// The guest library of `mise run bench-engines`: the fixture generators, a
// value printer and a result fold, as plain JavaScript (a script, not a
// module) that runs the same in Node (JIT or --jitless) and in QuickJS.
// guest.mjs pastes it into every guest program.
//
// Each __make* function mirrors a make_* function of bench/bench.c (the
// comment names it). They are written by hand, so before timing anything
// compare.mjs checks that each gives, as __show prints it, exactly what
// bench.c's builds as staticjs's writer writes it (dump.c), at the smoke
// size and the measured size: change a generator in bench.c, and that check
// fails until this file follows.

// xorshift64 from the seed, as bench.c's xorshift().
const __M64 = (1n << 64n) - 1n;
const __SEED = 0x9E3779B97F4A7C15n;
function __xorshift(st) {
  let x = st.s;
  x ^= (x << 13n) & __M64;
  x ^= x >> 7n;
  x ^= (x << 17n) & __M64;
  st.s = x;
  return x;
}
// row_id(), row_kwh(), make_row(), make_rows().
function __rowID(i) { return "r" + String(i).padStart(6, "0"); }
function __rowKwh(i) { return ((i * 37) % 1000) / 10; }
function __makeRow(i) { return { id: __rowID(i), kwh: __rowKwh(i), priority: i % 4, tags: ["a", "b"] }; }
function __makeRows(n) { const a = []; for (let i = 0; i < n; i++) a.push(__makeRow(i)); return a; }
// make_xs(): 0.5 * (0 .. n - 1), shuffled (Fisher-Yates on xorshift64).
function __makeXs(n) {
  const order = [];
  for (let i = 0; i < n; i++) order.push(i);
  const st = { s: __SEED };
  for (let i = n; i > 1; i--) {
    const j = Number(__xorshift(st) % BigInt(i));
    const t = order[i - 1]; order[i - 1] = order[j]; order[j] = t;
  }
  return order.map(o => o * 0.5);
}
// make_ids(): every row's id, every fifth twice.
function __makeIDs(n) {
  const a = [];
  for (let i = 0; i < n; i++) { a.push(__rowID(i)); if (i % 5 === 4) a.push(__rowID(i)); }
  return a;
}
// make_id_map(): each row's id to its kwh.
function __makeIDMap(n) { const m = new Map(); for (let i = 0; i < n; i++) m.set(__rowID(i), __rowKwh(i)); return m; }
// make_string_set(): "s%06u" over a range (FIX_SET_A, FIX_SET_B).
function __makeStringSet(first, count) {
  const s = new Set();
  for (let i = first; i < first + count; i++) s.add("s" + String(i).padStart(6, "0"));
  return s;
}
// make_pending_half(): the ids of the even rows.
function __makePendingHalf(n) { const s = new Set(); for (let i = 0; i < n; i += 2) s.add(__rowID(i)); return s; }
// make_numbers(): n doubles in [0, 1000) from xorshift64's top 53 bits.
function __numbersRaw(n) {
  const st = { s: __SEED }, a = [];
  for (let i = 0; i < n; i++) a.push(Number(__xorshift(st) >> 11n) / 9007199254740992 * 1000);
  return a;
}
// C's printf("%.*g", p, x) for finite x >= 0.
function __fmtG(x, p) {
  if (x === 0) return "0";
  const e = Number(x.toExponential(p - 1).split("e")[1]);
  const strip = s => s.includes(".") ? s.replace(/0+$/, "").replace(/\.$/, "") : s;
  if (e < -4 || e >= p) {
    const [m, ex] = x.toExponential(p - 1).split("e");
    const exn = Number(ex);
    return strip(m) + "e" + (exn < 0 ? "-" : "+") + String(Math.abs(exn)).padStart(2, "0");
  }
  return strip(x.toFixed(p - 1 - e));
}
// make_number_texts(): make_numbers() as "%.15g" or "%.17g".
function __makeNumberTexts(n, digits) { return __numbersRaw(n).map(x => __fmtG(x, digits)); }
// make_bigints(): 1n to n.
function __makeBigints(n) { const a = []; for (let i = 0; i < n; i++) a.push(BigInt(i + 1)); return a; }
// make_text(): the row ids joined by commas.
function __makeText(n) { const a = []; for (let i = 0; i < n; i++) a.push(__rowID(i)); return a.join(","); }
// rows_json(), make_rows_json(): the rows as JSON.stringify writes them.
function __makeRowsJSON(n) {
  let t = "[";
  for (let i = 0; i < n; i++) {
    const tenths = (i * 37) % 1000;
    const kwh = tenths % 10 === 0 ? String(tenths / 10 | 0) : String(Math.floor(tenths / 10)) + "." + String(tenths % 10);
    t += (i === 0 ? "" : ",") + '{"id":"' + __rowID(i) + '","kwh":' + kwh + ',"priority":' + (i % 4) + ',"tags":["a","b"]}';
  }
  return t + "]";
}
// make_demand(): n ports "p%03u" with want_kw and priority.
function __makeDemand(n) {
  const o = {};
  for (let i = 0; i < n; i++) o["p" + String(i).padStart(3, "0")] = { want_kw: 1 + ((i * 7) % 20), priority: i % 3 };
  return o;
}
// Values written as staticjs writes them (the bench's .expect).
function __show(v) {
  const t = typeof v;
  if (v === null) return "null";
  if (t === "undefined") return "undefined";
  if (t === "number") return Object.is(v, -0) ? "-0" : String(v);
  if (t === "boolean") return String(v);
  if (t === "string") return JSON.stringify(v);
  if (t === "bigint") return String(v) + "n";
  if (t === "function") return "<function>";
  if (Array.isArray(v)) return "[" + v.map(__show).join(",") + "]";
  if (v instanceof Set) return "new Set([" + [...v].map(__show).join(",") + "])";
  if (v instanceof Map) return "new Map([" + [...v].map(([k, x]) => "[" + __show(k) + "," + __show(x) + "]").join(",") + "])";
  return "{" + Object.keys(v).map(k => JSON.stringify(k) + ":" + __show(v[k])).join(",") + "}";
}
function __hash(s) { // FNV-1a 32 over UTF-16 code units
  let h = 0x811c9dc5;
  for (let i = 0; i < s.length; i++) { h ^= s.charCodeAt(i); h = Math.imul(h, 0x01000193) >>> 0; }
  return h.toString(16).padStart(8, "0");
}
// What each op folds into the checksum, so no engine can drop the work.
function __fold(v) {
  const t = typeof v;
  if (t === "number") return v === v ? v : 0;
  if (t === "string") return v.length;
  if (t === "boolean") return v ? 1 : 0;
  if (t === "bigint") return Number(v & 0xffffn);
  if (t === "function") return 1;
  if (Array.isArray(v)) return v.length;
  if (v instanceof Set || v instanceof Map) return v.size;
  return v === null || v === undefined ? 0 : 1;
}
