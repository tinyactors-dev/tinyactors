interface A { a: string; }

interface B extends A { b: string; }

interface Friend {
    name: string;
    favoriteColor?: string;
}

interface G<T, U extends B> {
    x: T;
    y: U;
}

interface X {
  hook: {
  }
  get(key: Key): Promise<T>;
}

interface Enum extends Bar, Baz, Foo<A> {
  public toEnum(): Int;
}
