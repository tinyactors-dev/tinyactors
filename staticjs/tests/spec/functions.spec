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
