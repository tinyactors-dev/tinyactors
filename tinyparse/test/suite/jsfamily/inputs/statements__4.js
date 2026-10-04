export { name1, name2, name3, nameN };
export { variable1 as name1, variable2 as name2, nameN };
export { variable1 as "string name" };
export let name1, name2, nameN;
export let name1 = value1, name2 = value2, name3, nameN;

export default expression;
export default { field1: 42, field2: [] }
export default function () { }
export default function name1() { }
export { name1 as default };

export * from 'foo';
export * as someIdentifier from "someModule";
export * as "string name" from "someModule";
export { name1, name2, nameN } from 'foo';
export { import1 as name1, import2 as name2, nameN } from 'foo';
export { import1 as "string name" } from 'foo';
export { "string import" as "string export" } from 'foo';
