function f(x: any): asserts x is number {
}

class Foo<T> {
  test(): this is T {}
}

function isT(t: T): t is T {
  return true
}
