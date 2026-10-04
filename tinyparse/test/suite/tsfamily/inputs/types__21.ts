class A<
  B,
  C,
> {}

class D extends A<
  X,
  Y,
> {}

class E extends funThatEvalsToClass()<
  X,
  Y,
> {}

class F extends G, H<T> {}
