function greeter(person: string) {
  return "Hello, " + person;
}

function foo<T>(x: T): T {

}

function foo<T, U>(a: T[], f: (x: T) => U): U[] {

}

function foo<T, U>(this: T[]): U[] {
  return []
}

function foo<const T, const U extends string>(x: T, y: U) {

}
