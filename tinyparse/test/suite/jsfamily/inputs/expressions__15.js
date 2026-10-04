class Foo {
  #e

  static one(a) { return a; };
  two(b) { return b; }
  three(c) { return c; }
  #four(d) { return this.#e; }
}

class Foo extends require('another-class') {
  constructor() {
    super()
  }

  static
  {
    this.#foo = 'bar';
  }

  bar() {
    super.a()
    this.#baz()
  }

  #baz() {
    super.a()
  }
}

class A{;}
