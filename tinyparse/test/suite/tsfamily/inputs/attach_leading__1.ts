// binds to the arrow, not the paren-expr it backtracks through
const g = (a, b, c) => a + b + c;

// binds to the generic call, not the comparison it looks like first
const r = f<number>(1);
