type Foo<T extends abstract new (...args: any) => any> = T;
type Foo<T extends new (...args: any) => any> = T;
