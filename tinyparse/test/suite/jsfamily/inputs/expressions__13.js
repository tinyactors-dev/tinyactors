{
  foo: true,

  add(a, b) {
    return a + b;
  },

  get bar() { return c; },

  set bar(a) { c = a; },

  *barGenerator() { yield c; },

  get() { return 1; }
};
