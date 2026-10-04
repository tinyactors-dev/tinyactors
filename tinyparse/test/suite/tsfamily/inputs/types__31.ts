type T = keyof Person;
type T = keyof Person.P;
type T = keyof Person<P>;
type T = keyof (Person);

type T = typeof Person;
type T = typeof Person.P;
type T = typeof import('person');

type T = keyof typeof Person;

type T = keyof U[number];
type T = keyof U & V;

type T = typeof array[number];
