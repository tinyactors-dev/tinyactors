type A<B> = { [B in keyof C & string as `get${Capitalize<P>}`]: () => A[B] };
type A<B> = { [B in keyof C & string as `${P}1` | `${P}2`]: A[B] }
