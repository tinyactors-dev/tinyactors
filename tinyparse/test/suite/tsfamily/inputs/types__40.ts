type T = X extends Y ? Z : Y
type T = X extends ?Y ? ?X : Y
type F<T, X, Y> = ((t: T) => X extends Y ? X : Y) extends ((t: T) => X extends Y ? Y : X) ? X : Y
type F<T, X, Y> = (t: T) => X extends Y ? X : Y extends (t: T) => X extends Y ? Y : X ? X : Y
type T<X, Y> = T extends X<infer Y> ? Y : X
type T<X> = X extends (infer X)[] ? X : never;
type T<X> = T extends { x: infer X } ? X : never;
type T<X> = T extends { x: infer X extends number } ? X : never;
