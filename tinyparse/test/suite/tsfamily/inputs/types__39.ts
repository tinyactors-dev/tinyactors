type t = []
type t = [A,]
type t = [A|B, C]
type t = readonly [A[], B.C]

// special optional and rest type syntax
type t = [string?, ...B]

// labeled elements
type t = [a: A, b?: B, ...c: C[]]
