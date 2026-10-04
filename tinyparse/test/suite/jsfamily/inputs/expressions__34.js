async function foo() {}

{
  async bar() {
  }
}

class Foo {
  async bar() {}
}

async (a) => { return foo; };

async function* foo() { yield 1; }
