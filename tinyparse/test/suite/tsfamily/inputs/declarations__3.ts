declare interface IFoo {
  bar(): number,
  baz(): IBaz,
}

declare class Foo {
  bar(): number,
  baz(): Baz,
}
