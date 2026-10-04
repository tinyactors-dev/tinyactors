type A<B, C> = `${B}${C}`;
type A = `${B[0]}-foo-${C}-bar-${D<U, D>}`
type A = `[${'a'}${0}]`
type A<B, C> = B extends C
  ? C extends string
    ? `${C}${"" extends C ? "" : "."}${B}`
    : never
  : never
type Trim<S extends string> = S extends `${infer R}` ? Trim<R>  : S;
type A = `${true & ('foo' | false)}`;
type StringToNumber<S extends string> = S extends `${infer N extends number}` ? N : never;
