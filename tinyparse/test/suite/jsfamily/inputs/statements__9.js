for (item in items)
  item();

for (var item in items || {})
  item();

for (const {thing} in things)
  thing();

for (x in a, b, c)
  foo();

for (x[i] in a) {}

for (x.y in a) {}

for ([a, b] in c) {}

for ((a) in b) {}

for (var foo = bar in baz) {}
