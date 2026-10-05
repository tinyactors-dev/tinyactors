# Arrow functions, patterns, const, and what keeps every evaluation finite.

=== calling an arrow function
(x => x + 1)(41)
--- value
42

=== no parameters
(() => "none")()
--- value
"none"

=== several parameters, and a trailing comma
((a, b, c,) => a + b + c)(1, 2, 3)
--- value
6

=== missing arguments are undefined, extra ones are dropped
[((a, b) => [a, b])(1), ((a) => a)(1, 2, 3)]
--- value
[[1,undefined],1]

=== a block body
(x => { const y = x * 2; const z = y + 1; return z; })(5)
--- value
11

=== a block body with no return gives undefined
(x => { const y = x; })(1)
--- value
undefined

=== an empty block
(() => {})()
--- value
undefined

=== return; gives undefined
(() => { return; })()
--- value
undefined

=== a return before the closing brace needs no semicolon
(() => { return 1 })()
--- value
1

=== several const declarators
(() => { const a = 1, b = a + 1; return [a, b]; })()
--- value
[1,2]

=== an arrow returning an object
(x => ({value: x}))(3)
--- value
{"value":3}

=== closures capture by value, and every binding is const
((a) => (b) => (c) => a + b + c)(1)(2)(3)
--- value
6

=== a closure that skips a level
(x => () => () => x)("deep")()()
--- value
"deep"

=== closures made in a loop keep their own values
[1, 2, 3].map(n => () => n * 10).map(f => f())
--- value
[10,20,30]

=== a const used by a later arrow
(() => { const k = 3; const times = x => x * k; return [1, 2].map(times); })()
--- value
[3,6]

=== array patterns in parameters
(([a, b], [, c]) => [a, b, c])([1, 2], [3, 4])
--- value
[1,2,4]

=== object patterns in parameters
(({a, b: {c}}) => a + c)({a: 1, b: {c: 2}})
--- value
3

=== a pattern takes undefined for what is missing
(([a, b]) => [a, b])([1])
--- value
[1,undefined]

=== an object pattern with a renaming
(x => { const {first: f, second: s} = x; return f + s; })({first: 1, second: 2})
--- value
3

=== nested patterns in const
(x => { const [{a}, [b, [c]]] = x; return [a, b, c]; })([{a: 1}, [2, [3]]])
--- value
[1,2,3]

=== destructuring the entries of an object
Object.entries({a: 1, b: 2}).map(([k, v]) => k + "=" + v)
--- value
["a=1","b=2"]

=== a set can be destructured as an array
(([first]) => first)(new Set(["x", "y"]))
--- value
"x"

=== a callback that is a built-in function
[1, 2, 3].map(String)
--- value
["1","2","3"]

=== a callback with a rest parameter sees every argument
[5, 6].map((...xs) => xs.length + ":" + xs[1])
--- value
["3:0","3:1"]

=== a callback with fewer parameters than the arguments
[[1, 2], [3]].map((xs, i) => xs.length + i)
--- value
[2,2]

=== a reduce callback that reads no parameter drops the accumulator
[[1], [2], [3]].reduce(() => [0], [9])
--- value
[0]

=== a reduce callback that reads only the accumulator
[1, 2, 3].reduce(acc => acc.concat([acc.length]), [])
--- value
[0,1,2]

=== a parameter past the arguments is undefined in a callback
[1].map((x, i, a, extra) => extra)
--- value
[undefined]

=== a toSorted comparator with one parameter
[3, 1, 2].toSorted(a => 0)
--- value
[3,1,2]

=== a loop inside a function that calls that function back is refused
(f => f(f))(g => [1, 2].map(x => x).concat([3].map(g)))
--- fault already running

=== loops nested inside a callback with callbacks of their own
(h => [1, 2].map(h))(x => [x].map(y => y + 1)[0] + [0].findIndex(z => true))
--- value
[2,3]

=== a function that is passed itself is refused when it calls itself
(g => g(g))(g => g(g))
--- fault already running

=== the same function at two depths is refused
((f) => f(f, 3))((self, n) => n === 0 ? 0 : self(self, n - 1))
--- fault already running

=== the same function called one after another is fine
((f) => [f(1), f(2), f(3)])(x => x * x)
--- value
[1,4,9]

=== a function may call another one
(() => { const inc = x => x + 1; const twice = x => inc(inc(x)); return twice(1); })()
--- value
3

=== functions are values
[x => x].length
--- value
1

=== calling something that is not a function
(1)(2)
--- fault not a function

=== shadowing a name from outside
((x) => ((x) => x * 2)(x + 1))(1)
--- value
4

=== an arrow function's body runs as far as it can
[1, 2].map(x => x ? "a" : "b")
--- value
["a","a"]

=== an arrow in a conditional
(true ? x => 1 : x => 2)()
--- value
1

# A loop's callback runs in one frame kept for the whole loop. Each element
# starts from the slots a fresh call would have: its arguments, undefined
# past them, and nothing an earlier element left behind.

=== closures made in a callback's body keep their own values
[1, 2, 3].map(x => { const y = [x]; return () => y; }).map(f => f())
--- value
[[1],[2],[3]]

=== closures made in a callback capture each element's arguments
[1, 2].map((x, i) => () => x + i).map(f => f())
--- value
[1,3]

=== a parameter past the arguments starts undefined for every element
[1, 2, 3].map((x, i, a, extra = x * 10) => (() => extra)())
--- value
[10,20,30]

=== reduce: a parameter past the arguments starts undefined for every element
[1, 2, 3].reduce((acc, x, i, a, last = x) => (() => acc + last)(), 0)
--- value
6

=== toSorted: a parameter past the two compared values starts undefined
[3, 1, 2].toSorted((p, q, r = p - q) => (() => r)())
--- value
[1,2,3]

=== toSorted: a parameter past the two compared values, with no default
[3, 1, 2].toSorted((p, q, r) => (() => r === undefined ? p - q : 99)())
--- value
[1,2,3]

=== a default parameter runs for each element that leaves it undefined
[1, undefined, 3, undefined].map((x = 7) => (() => x)())
--- value
[1,7,3,7]

=== array patterns in a callback's parameter, element by element
[[1, 2], [3]].map(([a, b = a]) => [a, b])
--- value
[[1,2],[3,3]]

=== object patterns with a rest in a callback's parameter, element by element
[{k: 1, z: 2}, {y: 3}].map(({k = 0, ...r}) => [k, r])
--- value
[[1,{"z":2}],[0,{"y":3}]]

=== a computed key in a callback's pattern, per element
[{a: 1}, {b: 2}].map((o, i, xs, {[i === 0 ? "a" : "b"]: v} = o) => v)
--- value
[1,2]

=== a callback's rest parameter gets the arguments past the others
[1, 2].map((x, ...r) => r.length)
--- value
[2,2]

=== a callback's rest parameter holds every argument
[1, 2].map((...r) => r)
--- value
[[1,0,[1,2]],[2,1,[1,2]]]

=== reduce with a rest parameter
[1, 2, 3].reduce((acc, ...r) => acc + r[0], 0)
--- value
6

=== toSorted with a rest parameter
[3, 1, 2].toSorted((...p) => p[0] - p[1])
--- value
[1,2,3]

=== each element's rest parameter is an array of its own
[1, 2].map((...r) => () => r[0]).map(f => f())
--- value
[1,2]

=== a loop nested in a callback keeps its own callback
[1, 2].map(x => [10, 20].map(y => x + y))
--- value
[[11,21],[12,22]]

=== three levels of loops, with block locals at each
[1, 2].map(x => { const a = x * 100; return [3, 4].map(y => { const b = a + y * 10; return [5, 6].filter(z => { const c = b + z; return c % 2 === 0; }).map(z => { const d = [b, z]; return d; }); }); })
--- value
[[[[130,6]],[[140,6]]],[[[230,6]],[[240,6]]]]

=== the same callback in a new inner loop for each element
(f => [[1], [2]].map(r => r.map(f)))(v => v + 1)
--- value
[[2],[3]]

=== the same callback in two loops, one after the other
(f => [[1].map(f), [2].map(f)])(v => v * 2)
--- value
[[2],[4]]

=== a loop's callback called directly after the loop
(f => [[1].map(f), f(2)])(v => v * 2)
--- value
[[2],4]

=== a callback that calls a function with a loop of its own
(g => [1, 2].map(x => g(x)))(n => [n, n].map(m => m * n).reduce((a, b) => a + b, 0))
--- value
[2,8]

=== a callback that runs a loop with itself as the callback is refused
(h => [h].map(h))(f => [1].map(f))
--- fault already running

=== a callback that calls a function whose loop calls back into it is refused
(h => [1, 2].map(x => h(h)))(g => [3].map(g))
--- fault already running

=== find, some, every, findIndex and findLast stop early, and a loop after them runs
[[1, 2, 3].find(x => { const y = x * 2; return y > 2; }), [1, 2, 3].some(x => { const y = x; return y === 2; }), [1, 2, 3].every(x => { const y = x; return y < 2; }), [1, 2, 3].findIndex(x => { const y = x; return y === 2; }), [1, 2, 3].findLast(x => { const y = x; return y < 3; }), [4, 5].map(x => { const y = x; return y; })]
--- value
[2,true,false,1,2,[4,5]]

=== a callback that returns for one element only
[1, 2, 3].map(x => { if (x === 2) return "two"; })
--- value
[undefined,"two",undefined]

=== a block-bodied comparator, with undefined sorted last
[3, undefined, 1].toSorted((a, b) => { const d = a - b; return d; })
--- value
[1,3,undefined]

=== flatMap and filter with locals in their callbacks
[1, 2, 3].flatMap(x => { const y = [x, x * 10]; return x === 2 ? [] : y; }).filter(v => { const big = v > 5; return big; })
--- value
[10,30]
