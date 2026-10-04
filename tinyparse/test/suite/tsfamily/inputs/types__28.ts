interface X {
  x(): Promise<Array<Foo>>;
}

var y: Map<number, Promise<Map<bool, Map<Foo, Bar>>>>
