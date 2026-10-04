let person: {name: string, age: number};
let thing: { [type: string]: string };
type T = { [K in keyof T]-?: any }
type T = { [K in keyof T]?: any }
type T = { -readonly [K in keyof T]: any }
